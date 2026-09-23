/** @file
 *
 *  Copyright (c) 2018, Andrei Warkentin <andrey.warkentin@gmail.com>
 *  Copyright (c) 2007-2009, Intel Corporation. All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include "VarBlockService.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

// Check the layout on every open, including a dump after removable media was
// replaced. A matching filename alone must never authorize extending a short
// file or overwriting data at a different firmware volume offset.
EFI_STATUS
ValidateStoreFile (
  IN EFI_FILE_PROTOCOL *File
  )
{
  struct {
    EFI_FIRMWARE_VOLUME_HEADER Header;
    EFI_FV_BLOCK_MAP_ENTRY End;
  } Volume;
  EFI_FIRMWARE_VOLUME_HEADER *Expected;
  EFI_STATUS Status;
  UINT64 Length;
  UINTN Size;

  if ((File == NULL) || (mFvInstance == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((mFvInstance->Offset > FixedPcdGet32 (PcdFdSize)) ||
      (mFvInstance->FvLength > FixedPcdGet32 (PcdFdSize) - mFvInstance->Offset) ||
      (mFvInstance->FvLength < sizeof (Volume))) {
    return EFI_VOLUME_CORRUPTED;
  }
  Status = File->SetPosition (File, MAX_UINT64);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = File->GetPosition (File, &Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Length != FixedPcdGet32 (PcdFdSize)) {
    return EFI_VOLUME_CORRUPTED;
  }
  Status = File->SetPosition (File, mFvInstance->Offset);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Size = sizeof (Volume);
  Status = File->Read (File, &Size, &Volume);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Size != sizeof (Volume)) {
    return EFI_VOLUME_CORRUPTED;
  }
  Status = GetFvbInfo (mFvInstance->FvLength, &Expected);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((Volume.Header.Signature != EFI_FVH_SIGNATURE) ||
      (Volume.Header.Revision != EFI_FVH_REVISION) ||
      (Volume.Header.FvLength != mFvInstance->FvLength) ||
      (Volume.Header.HeaderLength != sizeof (Volume)) ||
      (Volume.Header.ExtHeaderOffset != 0) ||
      !CompareGuid (&Volume.Header.FileSystemGuid, &Expected->FileSystemGuid) ||
      (CompareMem (Volume.Header.ZeroVector, Expected->ZeroVector,
                   sizeof (Volume.Header.ZeroVector)) != 0) ||
      (CompareMem (Volume.Header.BlockMap, Expected->BlockMap,
                   sizeof (EFI_FV_BLOCK_MAP_ENTRY)) != 0) ||
      (Volume.End.NumBlocks != 0) || (Volume.End.Length != 0) ||
      (CalculateSum16 ((UINT16 *)&Volume, sizeof (Volume)) != 0)) {
    return EFI_VOLUME_CORRUPTED;
  }
  return EFI_SUCCESS;
}


EFI_STATUS
FileWrite (
  IN EFI_FILE_PROTOCOL *File,
  IN UINTN Offset,
  IN UINTN Buffer,
  IN UINTN Size
  )
{
  EFI_STATUS Status;
  UINTN RequestedSize;

  if ((File == NULL) || ((Buffer == 0) && (Size != 0))) {
    return EFI_INVALID_PARAMETER;
  }

  Status = File->SetPosition (File, Offset);
  if (!EFI_ERROR (Status)) {
    RequestedSize = Size;
    Status = File->Write (File, &Size, (VOID*)Buffer);
    if (!EFI_ERROR (Status) && (Size != RequestedSize)) {
      return EFI_DEVICE_ERROR;
    }
  }
  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }
  return Status;
}


EFI_STATUS
FileClose (
  IN  EFI_FILE_PROTOCOL *File
  )
{
  return (File == NULL) ? EFI_INVALID_PARAMETER : File->Close (File);
}


EFI_STATUS
FileOpen (
  IN  EFI_DEVICE_PATH_PROTOCOL *Device,
  IN  CHAR16 *MappedFile,
  OUT EFI_FILE_PROTOCOL **File,
  IN  UINT64 OpenMode
  )
{
  EFI_HANDLE                        Handle;
  EFI_FILE_HANDLE                   Root;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL   *Volume;
  EFI_STATUS                        Status;
  EFI_STATUS                        CloseStatus;

  if (File == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *File = NULL;
  if ((Device == NULL) || (MappedFile == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->LocateDevicePath (
                  &gEfiSimpleFileSystemProtocolGuid,
                  &Device,
                  &Handle
                );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->HandleProtocol (
                  Handle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID**)&Volume
                );
  ASSERT_EFI_ERROR (Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Open the root directory of the volume
  //
  Root = NULL;
  Status = Volume->OpenVolume (
                     Volume,
                     &Root
                   );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Root == NULL) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Open file
  //
  Status = Root->Open (
                   Root,
                   File,
                   MappedFile,
                   OpenMode,
                   0
                 );
  if (EFI_ERROR (Status)) {
    *File = NULL;
  } else if (*File == NULL) {
    Status = EFI_DEVICE_ERROR;
  }

  //
  // Close the Root directory
  //
  CloseStatus = Root->Close (Root);
  if (!EFI_ERROR (Status) && EFI_ERROR (CloseStatus)) {
    FileClose (*File);
    *File = NULL;
    return CloseStatus;
  }
  return Status;
}


EFI_STATUS
CheckStore (
  IN  EFI_HANDLE SimpleFileSystemHandle,
  OUT EFI_DEVICE_PATH_PROTOCOL **Device
  )
{
  EFI_STATUS Status;
  EFI_BLOCK_IO_PROTOCOL *BlkIo;
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS CloseStatus;

  if (Device == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Device = NULL;
  Status = gBS->HandleProtocol (
                  SimpleFileSystemHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID*)&BlkIo
                );

  if (EFI_ERROR (Status)) {
    goto ErrHandle;
  }
  if (!BlkIo->Media->MediaPresent) {
    DEBUG ((DEBUG_ERROR, "FwhMappedFile: Media not present!\n"));
    Status = EFI_NO_MEDIA;
    goto ErrHandle;
  }
  if (BlkIo->Media->ReadOnly) {
    DEBUG ((DEBUG_ERROR, "FwhMappedFile: Media is read-only!\n"));
    Status = EFI_ACCESS_DENIED;
    goto ErrHandle;
  }

  Status = FileOpen (DevicePathFromHandle (SimpleFileSystemHandle),
             mFvInstance->MappedFile, &File,
             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE);
  if (EFI_ERROR (Status)) {
    goto ErrHandle;
  }

  Status = ValidateStoreFile (File);
  CloseStatus = FileClose (File);
  if (EFI_ERROR (Status) || EFI_ERROR (CloseStatus)) {
    return EFI_ERROR (Status) ? Status : CloseStatus;
  }
  *Device = DuplicateDevicePath (DevicePathFromHandle (SimpleFileSystemHandle));

  if (*Device == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
  }

ErrHandle:
  return Status;
}


EFI_STATUS
CheckStoreExists (
  IN  EFI_DEVICE_PATH_PROTOCOL *Device
  )
{
  EFI_HANDLE Handle;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Volume;
  EFI_STATUS Status;

  Status = gBS->LocateDevicePath (
                  &gEfiSimpleFileSystemProtocolGuid,
                  &Device,
                  &Handle
                );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->HandleProtocol (
                  Handle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID**)&Volume
                );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}
