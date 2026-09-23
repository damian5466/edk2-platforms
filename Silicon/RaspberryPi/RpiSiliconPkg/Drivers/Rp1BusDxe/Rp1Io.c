/** @file
  RP1 GPIO and PWM register access.

  Register definitions are described by Raspberry Pi's RP1 peripherals manual
  and the RP1 pinctrl, clock and PWM bindings.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Rp1.h>
#include "Rp1BusDxe.h"

#define IO_SET              0x2000
#define IO_CLEAR            0x3000
#define GPIO_FUNCTION_MASK  0x1f
#define GPIO_OVERRIDE_MASK (0x3fU << 12)
#define GPIO_OE_DISABLE    (2U << 14)
#define PAD_PULL_MASK       0x0c
#define PAD_INPUT_ENABLE    BIT6
#define PAD_OUTPUT_DISABLE BIT7
#define PWM_UPDATE         BIT31
#define PWM_CHANNELS       0x0f
#define PWM_CLOCK_ENABLE   BIT11
#define PWM_CLOCK_AUX_MASK  0x3e0
#define PWM_CLOCK_XOSC    (2U << 5)

STATIC
UINTN
Rp1PinBank (
  IN UINTN Pin,
  OUT UINTN *LocalPin
  )
{
  UINTN Bank;

  Bank = (Pin < 28) ? 0 : ((Pin < 34) ? 1 : 2);
  *LocalPin = Pin - ((Bank == 0) ? 0 : ((Bank == 1) ? 28 : 34));
  return Bank * 0x4000;
}

// Reading the same device drains posted PCIe writes and paces successive writes
// across low-power link transitions. IoLib supplies architecture MMIO ordering.
STATIC
VOID
Rp1Write (
  IN UINTN Address,
  IN UINT32 Value,
  IN UINTN ReadbackAddress
  )
{
  MmioWrite32 (Address, Value);
  MmioRead32 (ReadbackAddress);
}

STATIC
EFI_STATUS
EFIAPI
Rp1GetGpioConfig (
  IN RP1_IO_PROTOCOL *This,
  IN UINTN Pin,
  OUT RP1_GPIO_CONFIG *Config
  )
{
  UINTN Base, Bank, LocalPin;
  EFI_TPL OldTpl;

  if ((This == NULL) || (Pin >= RP1_GPIO_COUNT) || (Config == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Base = (UINTN)RP1_BUS_DATA_FROM_IO (This)->PeripheralBase;
  Bank = Rp1PinBank (Pin, &LocalPin);
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Config->Function = MmioRead32 (Base + RP1_IO_BANK0_BASE + Bank + LocalPin * 8 + 4) & GPIO_FUNCTION_MASK;
  Config->Pull = (MmioRead32 (Base + RP1_PADS_BANK0_BASE + Bank + LocalPin * 4 + 4) & PAD_PULL_MASK) >> 2;
  Config->Output = (MmioRead32 (Base + RP1_SYS_RIO0_BASE + Bank + 4) & (1U << LocalPin)) != 0;
  Config->Value = (MmioRead32 (Base + RP1_SYS_RIO0_BASE + Bank) & (1U << LocalPin)) != 0;
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Rp1ConfigureGpio (
  IN RP1_IO_PROTOCOL *This,
  IN UINTN Pin,
  IN CONST RP1_GPIO_CONFIG *Config
  )
{
  UINTN Base, Bank, LocalPin, Control, Pad, Rio;
  UINT32 OldControl, PadValue, Mask;
  EFI_TPL OldTpl;

  if ((This == NULL) || (Pin >= RP1_GPIO_COUNT) || (Config == NULL) ||
      ((Config->Function > 8) && (Config->Function != RP1_GPIO_DISABLED)) ||
      (Config->Pull > RP1_GPIO_PULL_UP) || (Config->Output > TRUE) || (Config->Value > TRUE)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Pin == 45) && RP1_BUS_DATA_FROM_IO (This)->FanActive) {
    return EFI_ACCESS_DENIED;
  }
  if ((Pin == 32) && RP1_BUS_DATA_FROM_IO (This)->EthernetActive) {
    return EFI_ACCESS_DENIED;
  }

  Base = (UINTN)RP1_BUS_DATA_FROM_IO (This)->PeripheralBase;
  Bank = Rp1PinBank (Pin, &LocalPin);
  Control = Base + RP1_IO_BANK0_BASE + Bank + LocalPin * 8 + 4;
  Pad = Base + RP1_PADS_BANK0_BASE + Bank + LocalPin * 4 + 4;
  Rio = Base + RP1_SYS_RIO0_BASE + Bank;
  Mask = 1U << LocalPin;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);

  OldControl = MmioRead32 (Control);
  Rp1Write (Control, (OldControl & ~(3U << 14)) | GPIO_OE_DISABLE, Control);
  // Establish the latch before enabling output, including input -> output.
  Rp1Write (Rio + (Config->Value ? IO_SET : IO_CLEAR), Mask, Rio);
  Rp1Write (Rio + 4 + (Config->Output ? IO_SET : IO_CLEAR), Mask, Rio + 4);
  PadValue = MmioRead32 (Pad);
  PadValue = (PadValue & ~(PAD_PULL_MASK | PAD_OUTPUT_DISABLE)) |
             PAD_INPUT_ENABLE | (Config->Pull << 2);
  Rp1Write (Pad, PadValue, Pad);
  Rp1Write (Control, (OldControl & ~(GPIO_FUNCTION_MASK | GPIO_OVERRIDE_MASK)) |
            Config->Function, Control);
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Rp1ReadGpio (
  IN RP1_IO_PROTOCOL *This,
  IN UINTN Pin,
  OUT BOOLEAN *Value
  )
{
  UINTN Base, Bank, LocalPin;

  if ((This == NULL) || (Pin >= RP1_GPIO_COUNT) || (Value == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Base = (UINTN)RP1_BUS_DATA_FROM_IO (This)->PeripheralBase;
  Bank = Rp1PinBank (Pin, &LocalPin);
  *Value = (MmioRead32 (Base + RP1_SYS_RIO0_BASE + Bank + 8) & (1U << LocalPin)) != 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Rp1WriteGpio (
  IN RP1_IO_PROTOCOL *This,
  IN UINTN Pin,
  IN BOOLEAN Value
  )
{
  UINTN Base, Bank, LocalPin, Rio;

  if ((This == NULL) || (Pin >= RP1_GPIO_COUNT) || (Value > TRUE)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Pin == 45) && RP1_BUS_DATA_FROM_IO (This)->FanActive) {
    return EFI_ACCESS_DENIED;
  }
  if ((Pin == 32) && RP1_BUS_DATA_FROM_IO (This)->EthernetActive) {
    return EFI_ACCESS_DENIED;
  }

  Base = (UINTN)RP1_BUS_DATA_FROM_IO (This)->PeripheralBase;
  Bank = Rp1PinBank (Pin, &LocalPin);
  Rio = Base + RP1_SYS_RIO0_BASE + Bank;
  Rp1Write (Rio + (Value ? IO_SET : IO_CLEAR), 1U << LocalPin, Rio);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
Rp1PwmClock (
  IN UINTN Base,
  IN UINTN Controller,
  IN UINT32 ActiveChannels
  )
{
  UINTN Clock, Retry;
  UINT32 Control, Divider, Fraction;

  Clock = Base + RP1_CLOCKS_MAIN_BASE + 0x74 + Controller * 0x10;
  Control = MmioRead32 (Clock);
  Divider = MmioRead32 (Clock + 4);
  Fraction = MmioRead32 (Clock + 8);
  if (((Control & (PWM_CLOCK_ENABLE | PWM_CLOCK_AUX_MASK)) ==
       (PWM_CLOCK_ENABLE | PWM_CLOCK_XOSC)) && (Divider == 1) && (Fraction == 0)) {
    return (MmioRead32 (Clock + 12) != 0) ? EFI_SUCCESS : EFI_NOT_READY;
  }

  if (ActiveChannels != 0) {
    return EFI_UNSUPPORTED;
  }

  Rp1Write (Clock, Control & ~PWM_CLOCK_ENABLE, Clock);
  Rp1Write (Clock + 4, 1, Clock + 4);
  Rp1Write (Clock + 8, 0, Clock + 8);
  Rp1Write (Clock, (Control & ~PWM_CLOCK_AUX_MASK) | PWM_CLOCK_XOSC | PWM_CLOCK_ENABLE, Clock);
  for (Retry = 0; Retry < 1000; Retry++) {
    if (MmioRead32 (Clock + 12) != 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
  }

  // Restore the idle clock on failure. No PWM output has been enabled yet.
  Rp1Write (Clock, Control & ~PWM_CLOCK_ENABLE, Clock);
  Rp1Write (Clock + 4, Divider, Clock + 4);
  Rp1Write (Clock + 8, Fraction, Clock + 8);
  Rp1Write (Clock, Control, Clock);
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
EFIAPI
Rp1ConfigurePwm (
  IN RP1_IO_PROTOCOL *This,
  IN UINTN Controller,
  IN UINTN Channel,
  IN UINT64 PeriodNs,
  IN UINT64 DutyNs,
  IN BOOLEAN Inverted,
  IN BOOLEAN Enable
  )
{
  UINTN Base, Pwm;
  UINT64 Period, Duty;
  UINT32 Global;
  EFI_TPL OldTpl;
  EFI_STATUS Status;

  if ((This == NULL) || (Controller > 1) || (Channel > 3) ||
      (Inverted > TRUE) || (Enable > TRUE)) {
    return EFI_INVALID_PARAMETER;
  }

  Period = PeriodNs / 20 + ((PeriodNs % 20) >= 10);
  Duty = DutyNs / 20 + ((DutyNs % 20) >= 10);
  if (Enable && ((DutyNs > PeriodNs) || (Period == 0) || (Period > MAX_UINT32))) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Controller == 1) && RP1_BUS_DATA_FROM_IO (This)->FanActive) {
    return EFI_ACCESS_DENIED;
  }

  Base = (UINTN)RP1_BUS_DATA_FROM_IO (This)->PeripheralBase;
  Pwm = Base + ((Controller == 0) ? RP1_PWM0_BASE : RP1_PWM1_BASE);
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Global = MmioRead32 (Pwm);
  if (!Enable) {
    Rp1Write (Pwm, (Global & ~(1U << Channel)) | PWM_UPDATE, Pwm);
    gBS->RestoreTPL (OldTpl);
    return EFI_SUCCESS;
  }

  Status = Rp1PwmClock (Base, Controller, Global & PWM_CHANNELS);
  if (!EFI_ERROR (Status)) {
    // Buffered settings latch together on UPDATE. Use trailing-edge mark/space
    // mode, with FIFO disabled for this channel and its polarity explicit.
    Rp1Write (Pwm + 0x14 + Channel * 16, BIT8 | BIT0 | (Inverted ? BIT3 : 0), Pwm);
    Rp1Write (Pwm + 0x18 + Channel * 16, (UINT32)Period, Pwm);
    Rp1Write (Pwm + 0x20 + Channel * 16, (UINT32)Duty, Pwm);
    Rp1Write (Pwm, Global | (1U << Channel) | PWM_UPDATE, Pwm);
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

VOID
Rp1IoInitialize (
  IN RP1_BUS_DATA *Rp1Data
  )
{
  Rp1Data->Rp1Io.Revision = 1;
  Rp1Data->Rp1Io.GetGpioConfig = Rp1GetGpioConfig;
  Rp1Data->Rp1Io.ConfigureGpio = Rp1ConfigureGpio;
  Rp1Data->Rp1Io.ReadGpio = Rp1ReadGpio;
  Rp1Data->Rp1Io.WriteGpio = Rp1WriteGpio;
  Rp1Data->Rp1Io.ConfigurePwm = Rp1ConfigurePwm;
}
