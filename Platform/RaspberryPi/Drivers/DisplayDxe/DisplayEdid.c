/** @file
 *  Select a progressive EDID detailed timing supported by the Pi HDMI pipeline.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 **/

#include "DisplayEdid.h"
#include <Library/BaseMemoryLib.h>

STATIC
BOOLEAN
EdidChecksumValid (
  IN CONST UINT8  Block[128]
  )
{
  UINTN  Index;
  UINT8  Sum;

  Sum = 0;
  for (Index = 0; Index < 128; Index++) {
    Sum = (UINT8)(Sum + Block[Index]);
  }

  return Sum == 0;
}

BOOLEAN
DisplayEdidPreferredTiming (
  IN CONST UINT8          Edid[128],
  OUT RPI_DISPLAY_TIMING  *Timing
  )
{
  STATIC CONST UINT8  Signature[8] = { 0, 255, 255, 255, 255, 255, 255, 0 };
  CONST UINT8        *Dtd;
  UINTN              Offset;
  UINT32             HBlank;
  UINT32             VBlank;
  UINT32             HFront;
  UINT32             HSync;
  UINT32             VFront;
  UINT32             VSync;
  UINT32             Total;

  ZeroMem (Timing, sizeof (*Timing));
  if (!EdidChecksumValid (Edid) ||
      (CompareMem (Edid, Signature, sizeof (Signature)) != 0) ||
      (Edid[18] != 1) || ((Edid[20] & BIT7) == 0)) {
    return FALSE;
  }

  // EDID orders base-block detailed timings by preference. Do not guess a
  // physical mode from the framebuffer size or turn an interlaced DTD into a
  // progressive one. The bootloader's existing mode remains the fallback.
  for (Offset = 54; Offset + 18 <= 126; Offset += 18) {
    Dtd = Edid + Offset;
    ZeroMem (Timing, sizeof (*Timing));
    Timing->Clock = (Dtd[0] | ((UINT32)Dtd[1] << 8)) * 10;
    if ((Timing->Clock == 0) || (Timing->Clock > 297000) ||
        ((Dtd[17] & (BIT7 | BIT4 | BIT3)) != (BIT4 | BIT3)) ||
        ((Dtd[17] & (BIT6 | BIT5 | BIT0)) != 0)) {
      continue;
    }

    Timing->HDisplay = Dtd[2] | ((Dtd[4] & 0xf0) << 4);
    HBlank = Dtd[3] | ((Dtd[4] & 0x0f) << 8);
    Timing->VDisplay = Dtd[5] | ((Dtd[7] & 0xf0) << 4);
    VBlank = Dtd[6] | ((Dtd[7] & 0x0f) << 8);
    HFront = Dtd[8] | ((Dtd[11] & 0xc0) << 2);
    HSync = Dtd[9] | ((Dtd[11] & 0x30) << 4);
    VFront = (Dtd[10] >> 4) | ((Dtd[11] & 0x0c) << 2);
    VSync = (Dtd[10] & 0x0f) | ((Dtd[11] & 0x03) << 4);
    if ((Timing->HDisplay < 640) || (Timing->HDisplay > 3840) ||
        (Timing->VDisplay < 480) || (Timing->VDisplay > 2160) ||
        (HFront == 0) || (HSync == 0) || (HFront + HSync >= HBlank) ||
        (VFront == 0) || (VSync == 0) || (VFront + VSync >= VBlank) ||
        (((Timing->HDisplay | HBlank | HFront | HSync) & 1) != 0)) {
      continue;
    }

    Timing->HSyncStart = Timing->HDisplay + HFront;
    Timing->HSyncEnd = Timing->HSyncStart + HSync;
    Timing->HTotal = Timing->HDisplay + HBlank;
    Timing->VSyncStart = Timing->VDisplay + VFront;
    Timing->VSyncEnd = Timing->VSyncStart + VSync;
    Timing->VTotal = Timing->VDisplay + VBlank;
    Total = (UINT32)Timing->HTotal * Timing->VTotal;
    Timing->VRefresh = (Timing->Clock * 1000 + Total / 2) / Total;
    if ((Timing->VRefresh < 24) || (Timing->VRefresh > 85)) {
      continue;
    }

    Timing->Flags = ((Dtd[17] & BIT1) ? BIT0 : 0) |
                    ((Dtd[17] & BIT2) ? BIT1 : 0);
    if (Timing->HDisplay * 3 == Timing->VDisplay * 4) {
      Timing->Flags |= (1 << 4);
    } else if (Timing->HDisplay * 9 == Timing->VDisplay * 16) {
      Timing->Flags |= (2 << 4);
    }

    return TRUE;
  }

  ZeroMem (Timing, sizeof (*Timing));
  return FALSE;
}

BOOLEAN
DisplayEdidIsHdmi (
  IN CONST UINT8  Extension[128]
  )
{
  UINTN  Offset;
  UINTN  Length;
  UINTN  End;

  if (!EdidChecksumValid (Extension) || (Extension[0] != 2)) {
    return FALSE;
  }

  End = Extension[2];
  if ((End < 4) || (End > 127)) {
    return FALSE;
  }

  for (Offset = 4; Offset < End; Offset += Length + 1) {
    Length = Extension[Offset] & 31;
    if (Offset + Length + 1 > End) {
      return FALSE;
    }

    if (((Extension[Offset] >> 5) == 3) && (Length >= 3) &&
        (((Extension[Offset + 1] == 0x03) &&
          (Extension[Offset + 2] == 0x0c) && (Extension[Offset + 3] == 0)) ||
         ((Extension[Offset + 1] == 0xd8) &&
          (Extension[Offset + 2] == 0x5d) && (Extension[Offset + 3] == 0xc4)))) {
      return TRUE;
    }
  }

  return FALSE;
}
