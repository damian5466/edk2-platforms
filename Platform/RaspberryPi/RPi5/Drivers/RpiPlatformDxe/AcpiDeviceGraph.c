/** @file
 * Preserve boot-time wiring in an ACPI SSDT without an OS DT dependency.
 * _CRS and the documented service contracts, not graph reg/dma-ranges, own
 * resource allocation. Properties retain their original byte representation.
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 **/
#include <Uefi.h>
#include <IndustryStandard/Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/FdtPlatformLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/AcpiTable.h>
#include "AcpiDeviceGraph.h"

#define GRAPH_MAX_SIZE  SIZE_16MB
#define GRAPH_MAX_DEPTH 64
#define GRAPH_PATH_SIZE 1024
#define GRAPH_PAGE_SIZE 128

typedef struct {
  UINT8 *Data;
  UINTN Position;
  UINTN Capacity;
  EFI_STATUS Status;
} AML_WRITER;

typedef struct {
  CONST CHAR8 *Path;
  CONST CHAR8 *Owner;
} GRAPH_BINDING;

// Paths are stable hardware addresses, not Linux phandle numbers or aliases.
// Multiple DT functions may share a single ACPI resource owner.
STATIC CONST GRAPH_BINDING mGraphBindings[] = {
#include "AcpiGraphBindings.h"
};

STATIC VOID
Emit (AML_WRITER *Writer, CONST VOID *Data, UINTN Size)
{
  if (EFI_ERROR (Writer->Status)) {
    return;
  }
  if (Size > Writer->Capacity - Writer->Position) {
    Writer->Status = EFI_BAD_BUFFER_SIZE;
    return;
  }
  if ((Writer->Data != NULL) && (Size != 0)) {
    CopyMem (Writer->Data + Writer->Position, Data, Size);
  }
  Writer->Position += Size;
}

STATIC VOID
Byte (AML_WRITER *Writer, UINT8 Value)
{
  Emit (Writer, &Value, sizeof (Value));
}

STATIC VOID
Integer (AML_WRITER *Writer, UINT32 Value)
{
  Byte (Writer, 0x0C); // DWordPrefix; ARM64 firmware is little endian.
  Emit (Writer, &Value, sizeof (Value));
}

STATIC VOID
String (AML_WRITER *Writer, CONST CHAR8 *Value)
{
  Byte (Writer, 0x0D);
  Emit (Writer, Value, AsciiStrSize (Value));
}

STATIC UINTN
Begin (AML_WRITER *Writer, UINT8 Opcode)
{
  UINTN Offset;
  UINT32 Placeholder;

  Byte (Writer, Opcode);
  Offset = Writer->Position;
  Placeholder = 0;
  Emit (Writer, &Placeholder, sizeof (Placeholder));
  return Offset;
}

STATIC VOID
End (AML_WRITER *Writer, UINTN Offset)
{
  UINTN Length;
  UINT8 *Data;

  if (EFI_ERROR (Writer->Status) || (Writer->Data == NULL)) {
    return;
  }
  // A four-byte PkgLength includes its own four bytes, not the opcode.
  Length = Writer->Position - Offset;
  Data = Writer->Data + Offset;
  Data[0] = (UINT8)(0xC0 | (Length & 15));
  Data[1] = (UINT8)(Length >> 4);
  Data[2] = (UINT8)(Length >> 12);
  Data[3] = (UINT8)(Length >> 20);
}

STATIC UINTN
Package (AML_WRITER *Writer, UINT32 Count)
{
  UINTN Offset;

  Offset = Begin (Writer, 0x12);
  Byte (Writer, (UINT8)Count);
  return Offset;
}

// Windows does not create our static Name (VarPackage (...)) graph. Keep both
// node and property collections in pages of fixed PackageOp objects instead.
STATIC VOID
EndCountedPackage (AML_WRITER *Writer, UINTN Offset, UINT32 Count)
{
  if (Count > 255) {
    Writer->Status = EFI_BAD_BUFFER_SIZE;
  }
  if ((Writer->Data != NULL) && !EFI_ERROR (Writer->Status)) {
    Writer->Data[Offset + 4] = (UINT8)Count;
  }
  End (Writer, Offset);
}

STATIC CONST CHAR8 *
Owner (CONST CHAR8 *Path)
{
  UINTN Index;

  for (Index = 0; Index < ARRAY_SIZE (mGraphBindings); Index++) {
    if (AsciiStrCmp (Path, mGraphBindings[Index].Path) == 0) {
      return mGraphBindings[Index].Owner;
    }
  }
  return "";
}

STATIC EFI_STATUS
EmitGraph (CONST VOID *Fdt, AML_WRITER *Writer)
{
  INT32 Node;
  INT32 Depth;
  INT32 Prop;
  INT32 Length;
  INT32 NameLength;
  UINT32 Count;
  UINT32 PropCount;
  UINT32 Parents[GRAPH_MAX_DEPTH];
  UINTN Scope;
  UINTN Nodes;
  UINTN NodePage;
  UINTN Record;
  UINTN Properties;
  UINTN PropertyPage;
  UINTN Property;
  UINTN Buffer;
  CONST FDT_PROPERTY *Value;
  CONST CHAR8 *Name;
  CHAR8 Path[GRAPH_PATH_SIZE];

  Scope = Begin (Writer, 0x10);
  // Scope (\\_SB.DGRF)
  Emit (Writer, "\x5C\x2E_SB_DGRF", 10);
  Byte (Writer, 0x08); // NameOp
  Emit (Writer, "NODS", 4);
  Nodes = Package (Writer, 0);
  NodePage = 0;
  Depth = -1;
  Count = 0;
  for (Node = FdtNextNode (Fdt, -1, &Depth);
       (Node >= 0) && (Depth >= 0);
       Node = FdtNextNode (Fdt, Node, &Depth)) {
    if ((Depth >= GRAPH_MAX_DEPTH) ||
        (FdtGetPath (Fdt, Node, Path, sizeof (Path)) != 0)) {
      return EFI_BAD_BUFFER_SIZE;
    }
    if ((Count % GRAPH_PAGE_SIZE) == 0) {
      if (Count != 0) {
        EndCountedPackage (Writer, NodePage, GRAPH_PAGE_SIZE);
      }
      NodePage = Package (Writer, 0);
    }
    Record = Package (Writer, 4);
    String (Writer, Path);
    Integer (Writer, (Depth == 0) ? MAX_UINT32 : Parents[Depth - 1]);
    String (Writer, Owner (Path));
    Parents[Depth] = Count++;
    Properties = Package (Writer, 0);
    PropertyPage = 0;
    PropCount = 0;
    for (Prop = FdtFirstPropertyOffset (Fdt, Node); Prop >= 0;
         Prop = FdtNextPropertyOffset (Fdt, Prop)) {
      Value = FdtGetPropertyByOffset (Fdt, Prop, &Length);
      if ((Value == NULL) || (Length < 0)) {
        return EFI_COMPROMISED_DATA;
      }
      Name = FdtGetString (Fdt, Fdt32ToCpu (Value->NameOffset), &NameLength);
      if ((Name == NULL) || (NameLength < 0) || (NameLength > 255)) {
        return EFI_COMPROMISED_DATA;
      }
      if ((PropCount % GRAPH_PAGE_SIZE) == 0) {
        if (PropCount != 0) {
          EndCountedPackage (Writer, PropertyPage, GRAPH_PAGE_SIZE);
        }
        PropertyPage = Package (Writer, 0);
      }
      Property = Package (Writer, 2);
      String (Writer, Name);
      Buffer = Begin (Writer, 0x11);
      Integer (Writer, (UINT32)Length);
      Emit (Writer, Value->Data, (UINTN)Length);
      End (Writer, Buffer);
      End (Writer, Property);
      PropCount++;
    }
    if (Prop != -FDT_ERR_NOTFOUND) {
      return EFI_COMPROMISED_DATA;
    }
    if (PropCount != 0) {
      EndCountedPackage (Writer, PropertyPage, ((PropCount - 1) % GRAPH_PAGE_SIZE) + 1);
    }
    EndCountedPackage (Writer, Properties, (PropCount + GRAPH_PAGE_SIZE - 1) / GRAPH_PAGE_SIZE);
    End (Writer, Record);
  }
  if ((Node < 0) && (Node != -FDT_ERR_NOTFOUND)) {
    return EFI_COMPROMISED_DATA;
  }
  if (Count != 0) {
    EndCountedPackage (Writer, NodePage, ((Count - 1) % GRAPH_PAGE_SIZE) + 1);
  }
  EndCountedPackage (Writer, Nodes, (Count + GRAPH_PAGE_SIZE - 1) / GRAPH_PAGE_SIZE);
  End (Writer, Scope);
  return Writer->Status;
}

EFI_STATUS
BuildAcpiDeviceGraph (CONST VOID *Fdt, EFI_ACPI_DESCRIPTION_HEADER **Table)
{
  AML_WRITER Writer;
  EFI_STATUS Status;
  EFI_ACPI_DESCRIPTION_HEADER *Header;
  UINTN Size;

  if (Table == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Table = NULL;
  if ((Fdt == NULL) || (FdtCheckHeader (Fdt) != 0) ||
      (FdtTotalSize (Fdt) > GRAPH_MAX_SIZE)) {
    return EFI_COMPROMISED_DATA;
  }
  ZeroMem (&Writer, sizeof (Writer));
  Writer.Capacity = GRAPH_MAX_SIZE;
  Writer.Position = sizeof (*Header);
  Status = EmitGraph (Fdt, &Writer);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Size = Writer.Position;
  Header = AllocateZeroPool (Size);
  if (Header == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Writer.Data = (UINT8 *)Header;
  Writer.Capacity = Size;
  Writer.Position = sizeof (*Header);
  Status = EmitGraph (Fdt, &Writer);
  if (EFI_ERROR (Status) || (Writer.Position != Size)) {
    FreePool (Header);
    return EFI_COMPROMISED_DATA;
  }
  Header->Signature = SIGNATURE_32 ('S', 'S', 'D', 'T');
  Header->Length = (UINT32)Size;
  Header->Revision = 2;
  CopyMem (Header->OemId, "RPIFDN", 6);
  Header->OemTableId = SIGNATURE_64 ('R', 'P', 'I', 'G', 'R', 'A', 'P', 'H');
  Header->OemRevision = 2;
  Header->CreatorId = SIGNATURE_32 ('E', 'D', 'K', '2');
  Header->CreatorRevision = 1;
  Header->Checksum = CalculateCheckSum8 ((UINT8 *)Header, Size);
  *Table = Header;
  return EFI_SUCCESS;
}

EFI_STATUS
InstallAcpiDeviceGraph (VOID)
{
  EFI_ACPI_DESCRIPTION_HEADER *Table;
  EFI_ACPI_TABLE_PROTOCOL *Acpi;
  EFI_STATUS Status;
  UINTN Key;

  Status = BuildAcpiDeviceGraph (FdtPlatformGetBase (), &Table);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&Acpi);
  if (!EFI_ERROR (Status)) {
    Status = Acpi->InstallAcpiTable (Acpi, Table, Table->Length, &Key);
  }
  FreePool (Table);
  return Status;
}
