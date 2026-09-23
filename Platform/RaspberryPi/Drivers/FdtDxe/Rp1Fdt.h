/** @file
  Preserve the firmware-initialized RP1 PCI endpoint across a DT handoff.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef RP1_FDT_H_
#define RP1_FDT_H_

#include <Uefi.h>

// On failure the caller's device tree is unchanged. Repeated calls are safe.
EFI_STATUS
Rp1PreserveBridgeReset (
  IN OUT VOID  *Fdt
  );

#endif
