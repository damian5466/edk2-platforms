/** @file
  Preserve the firmware-initialized RP1 PCI endpoint across a DT handoff.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Rp1Fdt.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>

EFI_STATUS
Rp1PreserveBridgeReset (
  IN OUT VOID  *Fdt
  )
{
  INT32         Node;
  INT32         Provider;
  INT32         Size;
  INT32         NamesSize;
  INT32         ResetsSize;
  INT32         NameCount;
  INT32         Index;
  INT32         Ret;
  CONST CHAR8   *Compatible;
  CONST CHAR8   *Names;
  CONST UINT32  *Resets;
  CONST UINT32  *Cells;
  UINT32        ArgumentCount;
  UINTN         CellCount;
  UINTN         CellOffset;
  UINTN         NameOffset;
  UINTN         NameSize;
  UINTN         BridgeCell;
  UINTN         BridgeCells;
  UINTN         BridgeName;
  UINTN         BridgeNameSize;
  UINTN         RescalCount;
  UINTN         FdtSize;
  VOID          *Scratch;
  UINT8         *NewResets;
  CHAR8         *NewNames;

  if ((Fdt == NULL) || (FdtCheckHeader (Fdt) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  // PCIe2 is the fixed, on-board RP1 connection. Do not alter the resets
  // of the external PCIe connector, even though it has the same compatible.
  Node = FdtPathOffset (Fdt, "/axi/pcie@1000120000");
  if (Node < 0) {
    return EFI_NOT_FOUND;
  }

  Compatible = FdtGetProp (Fdt, Node, "compatible", &Size);
  if ((Compatible == NULL) ||
      !FdtStringListContains (Compatible, Size, "brcm,bcm2712-pcie")) {
    return EFI_UNSUPPORTED;
  }

  Names = FdtGetProp (Fdt, Node, "reset-names", &NamesSize);
  Resets = FdtGetProp (Fdt, Node, "resets", &ResetsSize);
  if ((Names == NULL) && (NamesSize == -FDT_ERR_NOTFOUND) &&
      (Resets == NULL) && (ResetsSize == -FDT_ERR_NOTFOUND)) {
    return EFI_SUCCESS;
  }

  NameCount = FdtStringListCount (Fdt, Node, "reset-names");
  if ((Names == NULL) || (Resets == NULL) || (NameCount <= 0) ||
      (ResetsSize <= 0) || ((ResetsSize % sizeof (UINT32)) != 0)) {
    return EFI_COMPROMISED_DATA;
  }

  CellCount = (UINTN)ResetsSize / sizeof (UINT32);
  CellOffset = 0;
  NameOffset = 0;
  BridgeCell = 0;
  BridgeCells = 0;
  BridgeName = 0;
  BridgeNameSize = 0;
  RescalCount = 0;
  for (Index = 0; Index < NameCount; Index++) {
    // Validate every phandle/specifier, including entries we will retain.
    // Reset providers can have different #reset-cells (rescal has zero).
    if (CellOffset >= CellCount) {
      return EFI_COMPROMISED_DATA;
    }

    Provider = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Resets[CellOffset]));
    if (Provider < 0) {
      return EFI_COMPROMISED_DATA;
    }

    Cells = FdtGetProp (Fdt, Provider, "#reset-cells", &Size);
    if ((Cells == NULL) || (Size != sizeof (UINT32))) {
      return EFI_COMPROMISED_DATA;
    }

    ArgumentCount = Fdt32ToCpu (*Cells);
    if (ArgumentCount >= CellCount - CellOffset) {
      return EFI_COMPROMISED_DATA;
    }

    // StringListCount above has checked all terminating NUL bytes.
    NameSize = AsciiStrSize (Names + NameOffset);
    if (NameSize == 1) {
      return EFI_COMPROMISED_DATA;
    }

    if (AsciiStrCmp (Names + NameOffset, "bridge") == 0) {
      if (BridgeCells != 0) {
        return EFI_COMPROMISED_DATA;
      }

      BridgeCell = CellOffset;
      BridgeCells = (UINTN)ArgumentCount + 1;
      BridgeName = NameOffset;
      BridgeNameSize = NameSize;
    }

    if (AsciiStrCmp (Names + NameOffset, "rescal") == 0) {
      RescalCount++;
    }

    NameOffset += NameSize;
    CellOffset += (UINTN)ArgumentCount + 1;
  }

  if ((CellOffset != CellCount) || (RescalCount > 1)) {
    return EFI_COMPROMISED_DATA;
  }

  if (BridgeCells == 0) {
    return EFI_SUCCESS;
  }

  if (RescalCount != 1) {
    return EFI_UNSUPPORTED;
  }

  // Apply both shrinking edits to a private copy, then publish together.
  // A failed allocation or libfdt edit must never leave mismatched lists.
  FdtSize = FdtTotalSize (Fdt);
  Scratch = AllocateCopyPool (FdtSize, Fdt);
  if (Scratch == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  NewResets = AllocatePool ((UINTN)ResetsSize + (UINTN)NamesSize);
  if (NewResets == NULL) {
    FreePool (Scratch);
    return EFI_OUT_OF_RESOURCES;
  }

  NewNames = (CHAR8 *)NewResets + ResetsSize;
  CopyMem (NewResets, Resets, BridgeCell * sizeof (UINT32));
  CopyMem (NewResets + BridgeCell * sizeof (UINT32),
           Resets + BridgeCell + BridgeCells,
           (CellCount - BridgeCell - BridgeCells) * sizeof (UINT32));
  CopyMem (NewNames, Names, BridgeName);
  CopyMem (NewNames + BridgeName, Names + BridgeName + BridgeNameSize,
           (UINTN)NamesSize - BridgeName - BridgeNameSize);

  Ret = FdtSetProp (Scratch, Node, "resets", NewResets,
                   (INT32)((CellCount - BridgeCells) * sizeof (UINT32)));
  if (Ret == 0) {
    Ret = FdtSetProp (Scratch, Node, "reset-names", NewNames,
                     (INT32)((UINTN)NamesSize - BridgeNameSize));
  }

  if (Ret == 0) {
    // The external bridge reset loses RP1's initialized class/BAR geometry
    // after UEFI on the tested BCM2712 platform. Linux still initializes the
    // host using its internal bridge-reset fallback and retains PHY rescal.
    CopyMem (Fdt, Scratch, FdtSize);
    DEBUG ((DEBUG_INFO, "RP1: preserving external bridge reset state for DT boot\n"));
  }

  FreePool (NewResets);
  FreePool (Scratch);
  return (Ret == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}
