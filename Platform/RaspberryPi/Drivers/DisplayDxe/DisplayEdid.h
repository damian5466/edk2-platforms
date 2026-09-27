/** @file
 *  EDID timing selection for the firmware console.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 **/

#ifndef DISPLAY_EDID_H_
#define DISPLAY_EDID_H_

#include <Uefi.h>
#include <Protocol/RpiFirmware.h>

BOOLEAN
DisplayEdidPreferredTiming (
  IN CONST UINT8          Edid[128],
  OUT RPI_DISPLAY_TIMING  *Timing
  );

BOOLEAN
DisplayEdidIsHdmi (
  IN CONST UINT8  Extension[128]
  );

#endif
