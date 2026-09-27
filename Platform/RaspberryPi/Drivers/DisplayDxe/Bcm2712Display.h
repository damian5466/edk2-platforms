/** @file
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 **/
#ifndef BCM2712_DISPLAY_H_
#define BCM2712_DISPLAY_H_
#include <Uefi.h>
#include <Protocol/RpiFirmware.h>
EFI_STATUS Bcm2712DisplaySetMode (UINT32 BoardRevision,
  CONST RPI_DISPLAY_TIMING *Timing, EFI_PHYSICAL_ADDRESS FbBase, UINTN FbSize, UINTN Pitch);
#endif
