/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "VarBlockService.h"
#include "FileNvram.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NvramFileLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/Variable.h>

STATIC NV_FILE_HEADER mIdentity;
STATIC UINT64 mSequence, mBootSequence, mSavedSequence;
STATIC BOOLEAN mActive;
STATIC EFI_GET_VARIABLE mOriginalGetVariable;

VOID FileNvramChanged (VOID) {
  if (mActive) {
    if (mSequence < MAX_UINT64 - 1) mSequence++;
    else mActive = FALSE;
  }
}

STATIC EFI_STATUS Snapshot (NV_FILE_IMAGE *Image) {
  if (!mActive) return EFI_NOT_READY;
  CopyMem (&Image->Header, &mIdentity, sizeof (mIdentity));
  Image->Header.Sequence = mSequence;
  CopyMem (Image->Data, (VOID *)mFvInstance->FvBase, NV_FILE_DATA_SIZE);
  NvFileSeal (Image);
  return NvFileValid (Image, NV_FILE_SIZE) ? EFI_SUCCESS : EFI_VOLUME_CORRUPTED;
}

/* Private, read-only export endpoints. Ordinary UEFI variables retain their
 * original implementation. Windows accesses these through its documented
 * ExGetFirmwareEnvironmentVariable API, with no physical-memory mappings. */
STATIC EFI_STATUS EFIAPI ExportGetVariable (CHAR16 *Name, EFI_GUID *Guid,
                      UINT32 *Attributes, UINTN *Size, VOID *Data) {
  BOOLEAN IsInfo;
  UINTN Required;
  NV_FILE_INFO Info;
  if (!Name || !Guid || !Size) return EFI_INVALID_PARAMETER;
  if (!CompareGuid (Guid, &gRpiNvramFileGuid) ||
      (StrCmp (Name, NV_FILE_META_NAME) && StrCmp (Name, NV_FILE_SNAPSHOT_NAME)))
    return mOriginalGetVariable (Name, Guid, Attributes, Size, Data);
  IsInfo = StrCmp (Name, NV_FILE_META_NAME) == 0;
  Required = IsInfo ? sizeof (Info) : NV_FILE_SIZE;
  if (*Size < Required) { *Size = Required; return EFI_BUFFER_TOO_SMALL; }
  if (!Data) return EFI_INVALID_PARAMETER;
  *Size = Required;
  if (Attributes) *Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
  if (!IsInfo) return Snapshot (Data);
  ZeroMem (&Info, sizeof (Info));
  Info.Size = sizeof (Info); Info.Version = NV_FILE_VERSION; Info.Active = mActive;
  Info.Sequence = mSequence; Info.BootSequence = mBootSequence;
  CopyMem (Info.StoreId, mIdentity.StoreId, sizeof (Info.StoreId));
  CopyMem (Data, &Info, sizeof (Info));
  return EFI_SUCCESS;
}

STATIC VOID EFIAPI VariablesReady (EFI_EVENT Event, VOID *Context) {
  VOID *Protocol;
  if (mOriginalGetVariable || EFI_ERROR (gBS->LocateProtocol (&gEfiVariableArchProtocolGuid, NULL, &Protocol))) return;
  mOriginalGetVariable = gRT->GetVariable;
  gRT->GetVariable = ExportGetVariable;
  gRT->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 (&gRT->Hdr, gRT->Hdr.HeaderSize, &gRT->Hdr.CRC32);
  gBS->CloseEvent (Event);
}

VOID FileNvramInitialize (VOID) {
  VOID *Hob;
  EFI_EVENT Event;
  VOID *Registration;
  EFI_STATUS Status;
  Hob = GetFirstGuidHob (&gRpiNvramFileGuid);
  if (Hob && GET_GUID_HOB_DATA_SIZE (Hob) == sizeof (mIdentity)) {
    CopyMem (&mIdentity, GET_GUID_HOB_DATA (Hob), sizeof (mIdentity));
    if (NvFileHeaderValid (&mIdentity) && mIdentity.FirmwareOffset == mFvInstance->Offset &&
        mFvInstance->FvLength == NV_FILE_DATA_SIZE &&
        NvFileVolumeValid ((VOID *)mFvInstance->FvBase, NV_FILE_DATA_SIZE)) {
      mSequence = mBootSequence = mSavedSequence = mIdentity.Sequence;
      mActive = TRUE;
    }
  }
  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, VariablesReady, NULL, &Event);
  if (!EFI_ERROR (Status)) {
    Status = gBS->RegisterProtocolNotify (&gEfiVariableArchProtocolGuid, Event, &Registration);
    if (EFI_ERROR (Status)) gBS->CloseEvent (Event);
    else VariablesReady (Event, NULL);
  }
}

VOID FileNvramVirtualAddressChange (VOID) {
  if (mOriginalGetVariable) EfiConvertPointer (0, (VOID **)&mOriginalGetVariable);
}

STATIC EFI_STATUS ReadFile (EFI_FILE_PROTOCOL *Root, CHAR16 *Name, NV_FILE_IMAGE *Image) {
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Status;
  UINT64 Length;
  UINTN Size = NV_FILE_SIZE;
  ZeroMem (Image, NV_FILE_SIZE);
  Status = Root->Open (Root, &File, Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) return Status;
  Status = File->SetPosition (File, MAX_UINT64);
  if (!EFI_ERROR (Status)) Status = File->GetPosition (File, &Length);
  if (!EFI_ERROR (Status) && Length != NV_FILE_SIZE) Status = EFI_VOLUME_CORRUPTED;
  if (!EFI_ERROR (Status)) Status = File->SetPosition (File, 0);
  if (!EFI_ERROR (Status)) Status = File->Read (File, &Size, Image);
  if (!EFI_ERROR (Status) && Size != NV_FILE_SIZE) Status = EFI_DEVICE_ERROR;
  File->Close (File);
  return Status;
}

STATIC BOOLEAN FirmwareMatches (EFI_FILE_PROTOCOL *Root) {
  CHAR16 Path[128];
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Status;
  UINT64 Length;
  UINTN I;
  for (I = 0; I < ARRAY_SIZE (Path); I++) {
    Path[I] = mIdentity.FirmwarePath[I] == '/' ? L'\\' : (CHAR16)mIdentity.FirmwarePath[I];
    if (!Path[I]) break;
  }
  Status = Root->Open (Root, &File, Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) return FALSE;
  Status = File->SetPosition (File, MAX_UINT64);
  if (!EFI_ERROR (Status)) Status = File->GetPosition (File, &Length);
  File->Close (File);
  return !EFI_ERROR (Status) && Length == mIdentity.FirmwareSize;
}

EFI_STATUS FileNvramSave (VOID) {
  EFI_HANDLE *Handles = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root, *Chosen = NULL, *File;
  NV_FILE_IMAGE *Copies, *Image;
  EFI_STATUS Status, ReadStatus[2];
  UINTN Count, Index, Match = 0, Target = 0;
  INTN Selected;
  UINT64 Latest = 0, Length;
  CHAR16 Names[2][NV_FILE_PATH_SIZE];
  CHAR8 Path[NV_FILE_PATH_SIZE];
  UINTN Slot, Character;
  if (!mActive) return EFI_NOT_READY;
  if (mSequence == mSavedSequence) return EFI_SUCCESS;
  for (Slot = 0; Slot < 2; Slot++) {
    if (!NvFileName (&mIdentity, (UINT32)Slot, Path)) return EFI_VOLUME_CORRUPTED;
    for (Character = 0; Character < NV_FILE_PATH_SIZE; Character++) {
      Names[Slot][Character] = Path[Character] == '/' ? L'\\' : (CHAR16)Path[Character];
      if (!Path[Character]) break;
    }
  }
  Copies = AllocatePool (2 * NV_FILE_SIZE);
  Image = AllocatePool (NV_FILE_SIZE);
  if (!Copies || !Image) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Status = Snapshot (Image);
  if (EFI_ERROR (Status)) goto Done;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) goto Done;
  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status) || EFI_ERROR (Fs->OpenVolume (Fs, &Root))) continue;
    ReadStatus[0] = ReadFile (Root, Names[0], &Copies[0]);
    ReadStatus[1] = ReadFile (Root, Names[1], &Copies[1]);
    Selected = NvFileSelect (&Copies[0], &Copies[1]);
    if (Selected >= 0 && !EFI_ERROR (ReadStatus[Selected]) &&
        NvFileSameStore (&mIdentity, &Copies[Selected].Header) && FirmwareMatches (Root)) {
      Match++;
      if (!Chosen) {
        Chosen = Root;
        Latest = Copies[Selected].Header.Sequence;
        Target = (UINTN)Selected ^ 1;
        continue;
      }
    }
    Root->Close (Root);
  }
  if (Match != 1) { Status = Match ? EFI_NO_MAPPING : EFI_NOT_FOUND; goto Done; }
  if (Latest > Image->Header.Sequence) { Status = EFI_ACCESS_DENIED; goto Done; }
  if (Latest == Image->Header.Sequence) {
    // A previous flush may have completed even if its readback failed.
    Status = ReadFile (Chosen, Names[Target ^ 1], &Copies[0]);
    if (!EFI_ERROR (Status) && CompareMem (&Copies[0], Image, NV_FILE_SIZE)) Status = EFI_VOLUME_CORRUPTED;
    if (!EFI_ERROR (Status)) mSavedSequence = Image->Header.Sequence;
    goto Done;
  }
  Status = Chosen->Open (Chosen, &File, Names[Target], EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (EFI_ERROR (Status)) goto Done;
  Status = File->SetPosition (File, MAX_UINT64);
  if (!EFI_ERROR (Status)) Status = File->GetPosition (File, &Length);
  if (!EFI_ERROR (Status) && Length != NV_FILE_SIZE) Status = EFI_VOLUME_CORRUPTED;
  /* Write the payload, flush it, then commit the checksummed header. The other
   * file is never touched during this transaction. No truncate or rename. */
  if (!EFI_ERROR (Status)) Status = FileWrite (File, NV_FILE_HEADER_SIZE, (UINTN)Image->Data, NV_FILE_DATA_SIZE);
  if (!EFI_ERROR (Status)) Status = FileWrite (File, 0, (UINTN)&Image->Header, NV_FILE_HEADER_SIZE);
  File->Close (File);
  if (!EFI_ERROR (Status)) Status = ReadFile (Chosen, Names[Target], &Copies[0]);
  if (!EFI_ERROR (Status) && (!NvFileValid (&Copies[0], NV_FILE_SIZE) || CompareMem (&Copies[0], Image, NV_FILE_SIZE)))
    Status = EFI_DEVICE_ERROR;
  if (!EFI_ERROR (Status)) {
    mSavedSequence = Image->Header.Sequence;
    if (mSavedSequence != mSequence) Status = EFI_NOT_READY;
  }
Done:
  if (Chosen) Chosen->Close (Chosen);
  if (Handles) FreePool (Handles);
  if (Copies) FreePool (Copies);
  if (Image) FreePool (Image);
  return Status;
}
