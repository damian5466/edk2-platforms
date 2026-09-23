/** @file
 *  PCI aperture reservation for the ACPI memory map.
 *
 *  Copyright (c) 2023-2024, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 **/

#include <PiDxe.h>
#include <IndustryStandard/Pci.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/PciIo.h>

#include "ConfigTable.h"
#include "PciMemory.h"

STATIC
EFI_STATUS
EFIAPI
GetPciMem32TotalRange (
  OUT UINT64  *Base,
  OUT UINT64  *Size
  )
{
  EFI_STATUS           Status;
  UINTN                Index;
  EFI_HANDLE           *Handles;
  UINTN                HandleCount;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  PCI_TYPE01           PciConfigHeader;
  UINT64               MemoryBase;
  UINT64               MinimumMemoryBase;
  UINT64               MemoryLimit;
  UINT64               MaximumMemoryLimit;

  *Base = PCI_RESERVED_MEM32_BASE;
  *Size = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPciIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MinimumMemoryBase  = MAX_UINT64;
  MaximumMemoryLimit = 0;

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiPciIoProtocolGuid,
                    (VOID **)&PciIo
                    );
    if (EFI_ERROR (Status)) {
      goto Exit;
    }

    Status = PciIo->Pci.Read (
                          PciIo,
                          EfiPciIoWidthUint32,
                          0,
                          sizeof (PciConfigHeader) / sizeof (UINT32),
                          &PciConfigHeader
                          );
    if (EFI_ERROR (Status)) {
      goto Exit;
    }

    //
    // Some inaccessible devices return all ones (or zeros) without a read
    // error. An unreadable handle must not be mistaken for an absent window.
    //
    if ((PciConfigHeader.Hdr.VendorId == MAX_UINT16) ||
        (PciConfigHeader.Hdr.VendorId == 0))
    {
      Status = EFI_DEVICE_ERROR;
      goto Exit;
    }

    if (!IS_PCI_BRIDGE (&PciConfigHeader)) {
      continue;
    }

    //
    // Decode the checked header snapshot. The low nibble is reserved and
    // the limit includes the entire final 1 MB block. Base > limit disables
    // the window. Keep the arithmetic wide enough for an end address of 4 GB.
    //
    MemoryBase  = (UINT64)(PciConfigHeader.Bridge.MemoryBase & 0xFFF0) << 16;
    MemoryLimit = ((UINT64)(PciConfigHeader.Bridge.MemoryLimit & 0xFFF0) << 16) |
                  (SIZE_1MB - 1);
    if (MemoryBase > MemoryLimit) {
      continue;
    }

    MinimumMemoryBase  = MIN (MinimumMemoryBase, MemoryBase);
    MaximumMemoryLimit = MAX (MaximumMemoryLimit, MemoryLimit);
  }

  //
  // Success with size zero means every handle was inspected and no enabled
  // window was found. Discovery errors, including LocateHandleBuffer's
  // EFI_NOT_FOUND, cannot establish that the hardware windows are closed.
  //
  if (MinimumMemoryBase != MAX_UINT64) {
    *Base = MinimumMemoryBase;
    *Size = MaximumMemoryLimit - MinimumMemoryBase + 1;
  }

Exit:
  FreePool (Handles);
  return Status;
}

//
// See Bcm2712PciHostBridgeLib.c for more details.
//
VOID
EFIAPI
AdjustPciReservedMemory (
  IN  BOOLEAN  AcpiEnabled,
  IN  UINT32   PreferredSizeMB,
  IN  UINT64   SystemMemorySize,
  OUT UINT64   *AcpiPciMem32Base,
  OUT UINT64   *AcpiPciMem32Size
  )
{
  EFI_STATUS  Status;
  UINT64      ReservedEnd;
  UINT64      MemoryToReclaimBase;
  UINT64      MemoryToReclaimEnd;
  UINT64      MemoryToReclaimSize;
  UINT64      PciMem32Base;
  UINT64      PciMem32Size;
  UINT64      PciMem32PreferredSize;

  //
  // Preserve the entire DMA exclusion until ACPI aperture discovery and
  // validation succeed. FDT uses translated DMA and needs no such exclusion.
  //
  ReservedEnd         = (UINT64)PCI_RESERVED_MEM32_BASE + PCI_RESERVED_MEM32_SIZE;
  MemoryToReclaimBase = PCI_RESERVED_MEM32_BASE;
  MemoryToReclaimEnd  = MIN (SystemMemorySize, ReservedEnd);

  *AcpiPciMem32Base = PCI_RESERVED_MEM32_BASE;
  *AcpiPciMem32Size = AcpiEnabled ? PCI_RESERVED_MEM32_SIZE : 0;

  //
  // Compute the reserved memory size for ACPI boot.
  // FDT uses DMA translation and does not need any reserved RAM.
  //
  if (AcpiEnabled) {
    Status = GetPciMem32TotalRange (&PciMem32Base, &PciMem32Size);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: Mem32 discovery failed (%r); keeping the full PCI DMA reservation.\n",
        __func__,
        Status
        ));

      return;
    }

    DEBUG ((
      DEBUG_INFO,
      "%a: Mem32 Base: 0x%lx, Size: 0x%lx\n",
      __func__,
      PciMem32Base,
      PciMem32Size
      ));

    // Validate by subtraction so a malformed size cannot wrap the end.
    if ((PciMem32Base < PCI_RESERVED_MEM32_BASE) ||
        (PciMem32Base >= ReservedEnd) ||
        (PciMem32Size > ReservedEnd - PciMem32Base))
    {
      DEBUG ((
        DEBUG_ERROR,
        "%a: Mem32 region outside reserved bounds; keeping the full PCI DMA reservation.\n",
        __func__
        ));
      return;
    }

    PciMem32PreferredSize = (UINT64)PreferredSizeMB * SIZE_1MB;
    if (PciMem32PreferredSize > ReservedEnd - PciMem32Base) {
      DEBUG ((
        DEBUG_WARN,
        "%a: Clamping Mem32 preferred size 0x%lx to the remaining reserved space.\n",
        __func__,
        PciMem32PreferredSize
        ));
      PciMem32PreferredSize = ReservedEnd - PciMem32Base;
    }

    PciMem32Size         = MAX (PciMem32Size, PciMem32PreferredSize);
    *AcpiPciMem32Base    = PciMem32Base;
    *AcpiPciMem32Size    = PciMem32Size;
    MemoryToReclaimBase = PciMem32Base + PciMem32Size;

    DEBUG ((
      DEBUG_INFO,
      "%a: ACPI Mem32 Base: 0x%lx, Size: 0x%lx\n",
      __func__,
      *AcpiPciMem32Base,
      *AcpiPciMem32Size
      ));
  }

  if (MemoryToReclaimBase >= MemoryToReclaimEnd) {
    return;
  }

  MemoryToReclaimSize = MemoryToReclaimEnd - MemoryToReclaimBase;
  DEBUG ((
    DEBUG_INFO,
    "%a: Reclaiming system RAM - Base: 0x%lx, Size: 0x%lx\n",
    __func__,
    MemoryToReclaimBase,
    MemoryToReclaimSize
    ));

  Status = gDS->AddMemorySpace (
                  EfiGcdMemoryTypeSystemMemory,
                  MemoryToReclaimBase,
                  MemoryToReclaimSize,
                  EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to reclaim RAM. Status=%r\n", __func__, Status));
    return;
  }

  Status = gDS->SetMemorySpaceAttributes (
                  MemoryToReclaimBase,
                  MemoryToReclaimSize,
                  EFI_MEMORY_WB
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to set reclaimed RAM attributes. Status=%r\n", __func__, Status));
  }
}
