/** @file
 *  PCI aperture reservation for the ACPI memory map.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 **/

#ifndef RPI_PLATFORM_PCI_MEMORY_H_
#define RPI_PLATFORM_PCI_MEMORY_H_

#include <Uefi.h>

//
// Reclaim RAM outside the PCI aperture and return the range to publish in ACPI.
// FDT uses translated DMA and can reclaim the entire reservation.
//
VOID
EFIAPI
AdjustPciReservedMemory (
  IN  BOOLEAN  AcpiEnabled,
  IN  UINT32   PreferredSizeMB,
  IN  UINT64   SystemMemorySize,
  OUT UINT64   *AcpiPciMem32Base,
  OUT UINT64   *AcpiPciMem32Size
  );

#endif // RPI_PLATFORM_PCI_MEMORY_H_
