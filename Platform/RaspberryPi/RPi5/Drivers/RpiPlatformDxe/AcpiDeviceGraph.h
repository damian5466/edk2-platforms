/** SPDX-License-Identifier: BSD-2-Clause-Patent **/
#ifndef RPI_ACPI_DEVICE_GRAPH_H
#define RPI_ACPI_DEVICE_GRAPH_H

// Caller owns the returned, checksummed SSDT allocation.
EFI_STATUS
BuildAcpiDeviceGraph (
  IN CONST VOID *Fdt,
  OUT EFI_ACPI_DESCRIPTION_HEADER **Table
  );

EFI_STATUS
InstallAcpiDeviceGraph (VOID);

#endif
