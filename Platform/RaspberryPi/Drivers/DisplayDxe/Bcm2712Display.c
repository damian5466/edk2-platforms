/** @file
 * BCM2712 D0 console mode programming. The VideoCore framebuffer mailbox on
 * Pi5 returns the boot allocation; it does not implement arbitrary modes.
 * Keep that reserved allocation and its actual pitch, and program the native
 * HDMI, pixel-valve and HVS registers in the same order as vc4's encoder.
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 **/
#include "DisplayDxe.h"
#include "Bcm2712Display.h"

enum NativeField
{
  PvControl,
  PvVertical,
  PvEvenDelay,
  PvHa,
  PvHb,
  PvVa,
  PvVb,
  PvMux,
  PvPipe,
  PhyReset,
  PhyPower,
  Lane0,
  Lane1,
  Lane2,
  LaneClock,
  RefClock,
  PostDivider,
  VcoDivider,
  PllConfig,
  TmdsWord,
  Misc0,
  Misc1,
  Misc2,
  Misc3,
  Misc4,
  Misc5,
  Misc6,
  Misc7,
  Misc8,
  PllReset,
  PllPower,
  RmOffset,
  Fifo,
  PacketConfig,
  Scheduler,
  Ha,
  Hb,
  Va0,
  Vb0,
  Va1,
  Vb1,
  MiscControl,
  DeepColor,
  GcpConfig,
  GcpWord,
  Scrambler,
  ClockStop,
  VectorConfig,
  VectorCrossbar,
  CscControl,
  Csc11,
  Csc13,
  Csc21,
  Csc23,
  Csc31,
  Csc33,
  CscChannel,
  Avi0,
  Avi1,
  Avi2,
  Avi3,
  Avi4,
  Avi5,
  Avi6,
  Avi7,
  Avi8,
  VidControl,
  HvsControl1,
  NativeFieldCount
};

typedef struct
{
  UINT32 Region, Offset;
} NATIVE_REGISTER;

// Region 6 here means this output's HDMI core. VID_CTL is at 0x48 for HDMI1.
STATIC CONST NATIVE_REGISTER NativeRegisters[] = {
    {1, 0},     {1, 4},     {1, 8},     {1, 0xc},   {1, 0x10},  {1, 0x14},  {1, 0x18},  {1, 0x34},
    {1, 0x94},  {4, 0},     {4, 4},     {4, 8},     {4, 0xc},   {4, 0x10},  {4, 0x14},  {4, 0x1c},
    {4, 0x28},  {4, 0x2c},  {4, 0x44},  {4, 0x54},  {4, 0x60},  {4, 0x64},  {4, 0x68},  {4, 0x6c},
    {4, 0x70},  {4, 0x74},  {4, 0x78},  {4, 0x7c},  {4, 0x80},  {4, 0x190}, {4, 0x194}, {5, 0x18},
    {6, 0x7c},  {6, 0xc4},  {6, 0xe8},  {6, 0xec},  {6, 0xf0},  {6, 0xf4},  {6, 0xf8},  {6, 0x100},
    {6, 0x104}, {6, 0x114}, {6, 0x18c}, {6, 0x194}, {6, 0x198}, {6, 0x1e4}, {9, 0xbc},  {9, 0xf0},
    {9, 0xf4},  {11, 0},    {11, 4},    {11, 8},    {11, 0xc},  {11, 0x10}, {11, 0x14}, {11, 0x18},
    {11, 0x2c}, {10, 0x48}, {10, 0x4c}, {10, 0x50}, {10, 0x54}, {10, 0x58}, {10, 0x5c}, {10, 0x60},
    {10, 0x64}, {10, 0x68}, {8, 0x44},  {0, 0x104}};
STATIC_ASSERT (sizeof (NativeRegisters) / sizeof (*NativeRegisters) == NativeFieldCount,
               "register snapshot layout");

typedef struct
{
  UINT32 Value[NativeFieldCount];
} NATIVE_STATE;

STATIC CONST UINTN Bases[] = {0x107c580000ULL, 0x107c410000ULL, 0x107c411000ULL, 0x107c502000ULL,
                              0x107c701d00ULL, 0x107c702000ULL, 0x107c701400ULL, 0x107c706400ULL,
                              0x107c720000ULL, 0x107c701000ULL, 0x107c703800ULL, 0x107c700100ULL};

STATIC UINTN Address (UINT32 Port, UINT32 Unit, UINT32 Offset)
{
  UINTN Base = Bases[Unit];
  if (Port)
  {
    if (Unit == 1)
      Base = Bases[2];
    else if ((Unit == 4) || (Unit == 5) || (Unit == 9) || (Unit == 10))
      Base += 0x5000;
    else if (Unit == 11)
      Base += 0x80;
  }
  if ((Unit == 0) && (Offset >= 0x100) && (Offset <= 0x120))
    Offset += Port * 0x40;
  return Base + Offset;
}

STATIC UINT32 ReadReg (UINT32 Port, UINT32 Unit, UINT32 Offset)
{
  return MmioRead32 (Address (Port, Unit, Offset));
}

STATIC VOID WriteReg (UINT32 Port, UINT32 Unit, UINT32 Offset, UINT32 Value)
{
  MmioWrite32 (Address (Port, Unit, Offset), Value);
}

STATIC UINT32 ReadField (UINT32 Port, UINT32 Field)
{
  CONST NATIVE_REGISTER *R = &NativeRegisters[Field];
  return ReadReg (Port, R->Region == 6 ? 6 + Port : R->Region,
                  R->Offset + (R->Region == 8 ? Port * 4 : 0));
}

STATIC VOID WriteField (UINT32 Port, UINT32 Field, UINT32 Value)
{
  CONST NATIVE_REGISTER *R = &NativeRegisters[Field];
  WriteReg (Port, R->Region == 6 ? 6 + Port : R->Region,
            R->Offset + (R->Region == 8 ? Port * 4 : 0), Value);
}

STATIC BOOLEAN WaitBits (UINT32 Port, UINT32 Unit, UINT32 Offset, UINT32 Mask, UINT32 Value,
                         UINT32 Milliseconds)
{
  UINT32 I;
  for (I = 0; I <= Milliseconds; I++)
  {
    if ((ReadReg (Port, Unit, Offset) & Mask) == Value)
      return TRUE;
    MicroSecondDelay (1000);
  }
  return FALSE;
}

STATIC BOOLEAN BuildMode (CONST RPI_DISPLAY_TIMING *T, NATIVE_STATE *S)
{
  UINT32 *V = S->Value;
  UINT64 BitRate = (UINT64)T->Clock * 10000;
  UINT32 Lo, Hi, Div, I, W, Hf, Hs, HBack, Vf, Vs, Vb, Pixels, Aspect, Sum;
  BOOLEAN Hdmi;
  UINT8 Avi[35] = {0x82, 2, 13, 0, 0x10, 8, 0, 0, 0};
  STATIC CONST UINT32 Pll[] = {0x810c6000, 0x00b8c451, 0x46402e31, 0x00b8c005, 0x42410261,
                               0xcc021001, 0xc8301c80, 0xb0804444, 0xf80f8000};
  if (!T->Clock || T->Clock > 222000 || T->HDisplay < 640 || T->HDisplay > 3840 ||
      T->VDisplay < 480 || T->VDisplay > 2160 || T->HSyncStart <= T->HDisplay ||
      T->HSyncEnd <= T->HSyncStart || T->HTotal <= T->HSyncEnd || T->VSyncStart <= T->VDisplay ||
      T->VSyncEnd <= T->VSyncStart || T->VTotal <= T->VSyncEnd ||
      ((T->HDisplay | T->HSyncStart | T->HSyncEnd | T->HTotal) & 1) ||
      T->HSyncStart - T->HDisplay > 8191 || T->HTotal - T->HSyncEnd > 2047 ||
      T->HSyncEnd - T->HSyncStart > 2047 || T->VTotal - T->VSyncEnd > 511 ||
      T->VSyncEnd - T->VSyncStart > 31 || T->VSyncStart - T->VDisplay > 127)
    return FALSE;
  Lo = (UINT32)((8000000000ULL + BitRate - 1) / BitRate);
  Hi = (UINT32)(11999999999ULL / BitRate);
  if (Lo > Hi || Hi > 1023)
    return FALSE;
  Div = Lo + (Hi - Lo) / 2;
  V[RmOffset] = 0x80000000U | (UINT32)((BitRate * Div * (1ULL << 21)) / 54000000);
  V[VcoDivider] = 0x400 | Div;
  V[RefClock] = 0x2036;
  V[PostDivider] = 9;
  V[PllConfig] = 0;
  V[PhyReset] = 0x7f;
  V[PhyPower] = 0x1cf;
  V[PllPower] = 1;
  V[PllReset] |= 1;
  V[Lane0] = V[Lane1] = V[Lane2] = V[LaneClock] = 0x80828700;
  V[TmdsWord] = 0;
  for (I = 0; I < ARRAY_SIZE (Pll); I++)
    V[Misc0 + I] = Pll[I];
  Hf = T->HSyncStart - T->HDisplay;
  Hs = T->HSyncEnd - T->HSyncStart;
  HBack = T->HTotal - T->HSyncEnd;
  Vf = T->VSyncStart - T->VDisplay;
  Vs = T->VSyncEnd - T->VSyncStart;
  Vb = T->VTotal - T->VSyncEnd;
  Pixels = (V[PvVertical] & 0x20000000) ? 1 : 2;
  V[PvHa] = ((HBack / Pixels) << 16) | (Hs / Pixels);
  V[PvHb] = ((Hf / Pixels) << 16) | (T->HDisplay / Pixels);
  V[PvVa] = (Vb << 16) | Vs;
  V[PvVb] = (Vf << 16) | T->VDisplay;
  V[Ha] = (Hf << 16) | ((T->Flags & 3) << 14) | T->HDisplay;
  V[Hb] = (HBack << 16) | Hs;
  V[Va0] = V[Va1] = (Vs << 24) | (Vf << 16) | T->VDisplay;
  V[Vb0] = V[Vb1] = Vb;
  Hdmi = !(T->Flags & BIT9);
  V[Scheduler] = (V[Scheduler] & ~3U) | (Hdmi ? 1U : 0U);
  Aspect = (T->Flags >> 4) & 15;
  if ((Aspect == 1) || (Aspect == 2))
    Avi[5] |= (UINT8)(Aspect << 4);
  // The console uses full-range RGB and a detailed timing (no forced CTA VIC).
  Avi[6] = 8;
  Sum = 0;
  for (I = 0; I < 17; I++)
    Sum += Avi[I];
  Avi[3] = (UINT8)(0U - Sum);
  for (I = 0; I < 9; I++)
    V[Avi0 + I] = 0;
  for (I = 0, W = 0; I < 21; I += 7, W += 2)
  {
    V[Avi0 + W] = Avi[I] | ((UINT32)Avi[I + 1] << 8) | ((UINT32)Avi[I + 2] << 16);
    V[Avi0 + W + 1] = Avi[I + 3] | ((UINT32)Avi[I + 4] << 8) | ((UINT32)Avi[I + 5] << 16) |
                      ((UINT32)Avi[I + 6] << 24);
  }
  V[PacketConfig] = 0x10000 | (Hdmi ? 4U : 0U);
  V[VidControl] = (V[VidControl] & ~0x18040000U) | 0x80010000U |
                  ((T->Flags & 1) ? 0U : 0x08000000U) | ((T->Flags & 2) ? 0U : 0x10000000U);
  return TRUE;
}

STATIC EFI_STATUS DisableOutput (UINT32 Port)
{
  WriteField (Port, PvVertical, ReadField (Port, PvVertical) & ~1U);
  if (!WaitBits (Port, 1, 4, 1, 0, 20))
    return EFI_TIMEOUT;
  MicroSecondDelay (20000);
  WriteField (Port, PacketConfig, 0x10000);
  // Never clear VID_CTL.ENABLE: BCM2712 can hang when that bit is cleared.
  WriteField (Port, VidControl, ReadField (Port, VidControl) | 0x00840000);
  MicroSecondDelay (1000);
  WriteField (Port, PvControl, ReadField (Port, PvControl) & ~1U);
  WriteField (Port, PvControl, ReadField (Port, PvControl) | 2U);
  WriteReg (Port, 0, 0x100, ReadReg (Port, 0, 0x100) | 0x40000000);
  WriteReg (Port, 0, 0x100, ReadReg (Port, 0, 0x100) & ~0x80000000U);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ApplyOutput (UINT32 Port, CONST NATIVE_STATE *State, UINT32 Control, UINT32 Head,
                               CONST UINT32 List[8])
{
  CONST UINT32 *V = State->Value;
  UINT32 I, FifoConfig, First, Frame;
  WriteField (Port, PhyReset, 0);
  WriteField (Port, PhyPower, 0);
  WriteField (Port, PostDivider, 0x10);
  for (I = Misc0; I <= Misc8; I++)
    WriteField (Port, I, V[I]);
  WriteField (Port, RefClock, V[RefClock]);
  WriteField (Port, PhyReset, V[PhyReset]);
  WriteField (Port, RmOffset, V[RmOffset]);
  WriteField (Port, VcoDivider, V[VcoDivider]);
  WriteField (Port, PllConfig, V[PllConfig]);
  WriteField (Port, PostDivider, V[PostDivider]);
  for (I = Lane0; I <= LaneClock; I++)
    WriteField (Port, I, V[I]);
  WriteField (Port, TmdsWord, V[TmdsWord]);
  WriteField (Port, PhyPower, V[PhyPower]);
  WriteField (Port, PllPower, V[PllPower]);
  WriteField (Port, PllReset, V[PllReset] & ~1U);
  WriteField (Port, PllReset, V[PllReset]);
  WriteField (Port, Scheduler, V[Scheduler] | 0x8020);
  for (I = Ha; I <= Scrambler; I++)
    WriteField (Port, I, V[I]);
  for (I = ClockStop; I <= CscChannel; I++)
    WriteField (Port, I, V[I]);
  WriteField (Port, PacketConfig, 0x10000);
  for (I = Avi0; I <= Avi8; I++)
    WriteField (Port, I, V[I]);
  WriteField (Port, PvVertical, V[PvVertical] & ~1U);
  for (I = PvEvenDelay; I <= PvPipe; I++)
    WriteField (Port, I, V[I]);
  WriteField (Port, PvControl, (V[PvControl] & ~1U) | 2U);
  WriteField (Port, HvsControl1, V[HvsControl1]);
  for (I = 0; I < 8; I++)
    WriteReg (Port, 0, 0x4000 + (Head + I) * 4, List[I]);
  WriteReg (Port, 0, 0x100, 0x40000000);
  MemoryFence ();
  WriteReg (Port, 0, 0x110, Head);
  WriteReg (Port, 0, 0x100, Control);
  WriteField (Port, PvControl, V[PvControl] | 1U);
  WriteField (Port, Fifo, 1);
  WriteField (Port, PvVertical, V[PvVertical] | 1U);
  WriteField (Port, VidControl, (V[VidControl] & ~0x00040000U) | 0x80000000U);
  WriteField (Port, Scheduler, V[Scheduler] | 0x8020);
  if (!WaitBits (Port, 6 + Port, 0xe8, 2, (V[Scheduler] & 1) ? 2 : 0, 250))
    return EFI_TIMEOUT;
  WriteField (Port, PacketConfig, V[PacketConfig]);
  FifoConfig = V[Fifo] & 0xefff;
  WriteField (Port, Fifo, FifoConfig & ~0x40U);
  WriteField (Port, Fifo, FifoConfig | 0x40);
  MicroSecondDelay (1000);
  WriteField (Port, Fifo, FifoConfig & ~0x40U);
  WriteField (Port, Fifo, FifoConfig | 0x40);
  if (!WaitBits (Port, 6 + Port, 0x7c, 0x4000, 0x4000, 50))
    return EFI_TIMEOUT;
  First = (ReadReg (Port, 0, 0x118) >> 16) & 63;
  for (I = 0; I < 250; I++)
  {
    Frame = (ReadReg (Port, 0, 0x118) >> 16) & 63;
    if (((Frame - First) & 63) >= 2 && (ReadReg (Port, 0, 0x11c) & 0xfff) == Head)
      return EFI_SUCCESS;
    MicroSecondDelay (1000);
  }
  return EFI_TIMEOUT;
}

EFI_STATUS Bcm2712DisplaySetMode (UINT32 BoardRevision, CONST RPI_DISPLAY_TIMING *Timing,
                                  EFI_PHYSICAL_ADDRESS FbBase, UINTN FbSize, UINTN Pitch)
{
  UINT32 Port, Head, Control, I, List[8], OldList[8], Handle;
  NATIVE_STATE Before, Desired;
  EFI_STATUS Status, Restored;
  if (((BoardRevision >> 12) & 15) != 4 || (Timing->Display != 2 && Timing->Display != 7))
    return EFI_UNSUPPORTED;
  Port = Timing->Display == 7 ? 1 : 0;
  if (ReadReg (Port, 0, 0) != 0x2454 || (ReadReg (Port, 1, 0) & 1) == 0 ||
      ReadReg (Port, 1, 4) != 3 || FbBase > MAX_UINT32 || Pitch < (UINTN)Timing->HDisplay * 4 ||
      (Pitch & 3) || Pitch * Timing->VDisplay > FbSize)
    return EFI_UNSUPPORTED;
  Head = ReadReg (Port, 0, 0x110) & 0xfff;
  Control = ReadReg (Port, 0, 0x100);
  if (Head >= 0x800 || (ReadReg (Port, 0, 0x11c) & 0xfff) != Head)
    return EFI_UNSUPPORTED;
  for (I = 0; I < 8; I++)
    List[I] = OldList[I] = ReadReg (Port, 0, 0x4000 + (Head + I) * 4);
  if (List[0] != 0x600cc007 || List[1] != 0 || List[2] != 0xfff0 || (List[5] & 15) != 0 ||
      List[6] != (UINT32)FbBase || List[7] != Pitch)
    return EFI_UNSUPPORTED;
  for (I = 0; I < NativeFieldCount; I++)
    Before.Value[I] = ReadField (Port, I);
  Desired = Before;
  if (!BuildMode (Timing, &Desired))
    return EFI_UNSUPPORTED;
  List[3] = ((UINT32)(Timing->VDisplay - 1) << 16) | (Timing->HDisplay - 1);
  // Retire the old prefetch handle when the visible raster width changes.
  Handle = (((List[5] >> 10) & 31) + 2) & 31;
  List[5] = (List[5] & ~(31U << 10)) | (Handle << 10);
  Status = DisableOutput (Port);
  if (!EFI_ERROR (Status))
    Status = ApplyOutput (
        Port, &Desired,
        0x80000000U | ((UINT32)(Timing->HDisplay - 1) << 16) | (Timing->VDisplay - 1), Head, List);
  DEBUG ((DEBUG_INFO, "Native console HDMI%u: %ux%u pitch %u head %x status %r PV %x/%x HVS %x\n",
          Port, Timing->HDisplay, Timing->VDisplay, Pitch, Head, Status, ReadReg (Port, 1, 0x10),
          ReadReg (Port, 1, 0x18), ReadReg (Port, 0, 0x100)));
  if (EFI_ERROR (Status))
  {
    DisableOutput (Port);
    Restored = ApplyOutput (Port, &Before, Control, Head, OldList);
    DEBUG ((DEBUG_ERROR, "Native console rollback: %r\n", Restored));
    if (EFI_ERROR (Restored))
      return EFI_DEVICE_ERROR;
  }
  return Status;
}
