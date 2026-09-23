/** @file
  Keep a firmware-detected Raspberry Pi 5 cooling fan running during UEFI.
  This is a fixed full-speed boot policy, not an OS thermal controller.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/FdtPlatformLib.h>
#include <Library/IoLib.h>
#include <Rp1.h>
#include "Rp1BusDxe.h"

STATIC BOOLEAN
Rp1FanCompatible (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Compatible)
{
  CONST CHAR8 *Value;
  INT32 Length;

  Value = FdtGetProp (Fdt, Node, "compatible", &Length);
  return (Value != NULL) && (Length > 0) &&
         FdtStringListContains (Value, Length, Compatible);
}

STATIC BOOLEAN
Rp1FanNodeEnabled (CONST VOID *Fdt, INT32 Node)
{
  CONST CHAR8 *Value;
  INT32 Length;

  Value = FdtGetProp (Fdt, Node, "status", &Length);
  // Require positive firmware detection; missing status is not evidence of a fan.
  return (Value != NULL) &&
    (((Length == 5) && (CompareMem (Value, "okay", 5) == 0)) ||
     ((Length == 3) && (CompareMem (Value, "ok", 3) == 0)));
}

STATIC BOOLEAN
Rp1FanDetected (VOID)
{
  CONST VOID *Fdt;
  CONST UINT32 *Cells;
  INT32 Fan, Pwm, Pin, Length;
  CONST CHAR8 *String;

  Fdt = FdtPlatformGetBase ();
  if ((Fdt == NULL) || !Rp1FanCompatible (Fdt, 0, "raspberrypi,5-model-b")) {
    return FALSE;
  }
  Fan = FdtPathOffset (Fdt, "/cooling_fan");
  if ((Fan < 0) || !Rp1FanNodeEnabled (Fdt, Fan) ||
      !Rp1FanCompatible (Fdt, Fan, "pwm-fan")) {
    return FALSE;
  }
  Cells = FdtGetProp (Fdt, Fan, "pwms", &Length);
  if ((Cells == NULL) || (Length != 16) ||
      (Fdt32ToCpu (Cells[1]) != 3) || (Fdt32ToCpu (Cells[2]) != 41566) ||
      (Fdt32ToCpu (Cells[3]) != 1)) {
    return FALSE;
  }
  Pwm = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Cells[0]));
  if ((Pwm < 0) || !Rp1FanNodeEnabled (Fdt, Pwm) ||
      !Rp1FanCompatible (Fdt, Pwm, "raspberrypi,rp1-pwm")) {
    return FALSE;
  }
  Cells = FdtGetProp (Fdt, Pwm, "#pwm-cells", &Length);
  if ((Cells == NULL) || (Length != 4) || (Fdt32ToCpu (*Cells) != 3)) {
    return FALSE;
  }
  Cells = FdtGetProp (Fdt, Pwm, "reg", &Length);
  if (!((Cells != NULL) && (Length == 16) &&
    (Fdt32ToCpu (Cells[0]) == 0xc0) && (Fdt32ToCpu (Cells[1]) == 0x4009c000) &&
    (Fdt32ToCpu (Cells[2]) == 0) && (Fdt32ToCpu (Cells[3]) == 0x100))) {
    return FALSE;
  }
  // The bootloader can leave GPIO45 as an ordinary GPIO even though its live
  // DT positively detects the fan and assigns this PWM pin. Honor that binding
  // explicitly; do not repurpose a pin selected by a different overlay.
  Cells = FdtGetProp (Fdt, Pwm, "pinctrl-0", &Length);
  if ((Cells == NULL) || (Length != 4)) { return FALSE; }
  Pin = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Cells));
  if (Pin < 0) { return FALSE; }
  String = FdtGetProp (Fdt, Pin, "function", &Length);
  if ((String == NULL) || (Length != 5) || (CompareMem (String, "pwm1", 5) != 0)) {
    return FALSE;
  }
  String = FdtGetProp (Fdt, Pin, "pins", &Length);
  return (String != NULL) && (Length == 7) && (CompareMem (String, "gpio45", 7) == 0);
}

// GPIO45 is bank 2, local pin 11. Only its direction/latch bits are restored.
#define FAN_GPIO_CTRL (RP1_IO_BANK0_BASE + 0x8000 + 11 * 8 + 4)
#define FAN_GPIO_PAD  (RP1_PADS_BANK0_BASE + 0x8000 + 11 * 4 + 4)
#define FAN_GPIO_RIO  (RP1_SYS_RIO0_BASE + 0x8000)
#define FAN_GPIO_BIT  BIT11

STATIC VOID
Rp1FanWrite (UINTN Address, UINT32 Value)
{
  MmioWrite32 (Address, Value);
  MmioRead32 (Address);
}

VOID
Rp1FanRestore (IN RP1_BUS_DATA *Data)
{
  UINTN Pwm, Clock;

  if (!Data->FanActive) {
    return;
  }
  Pwm = Data->PeripheralBase + RP1_PWM1_BASE;
  Clock = Data->PeripheralBase + RP1_CLOCKS_MAIN_BASE + 0x84;
  if (Data->FanPinChanged) {
    UINTN Base = Data->PeripheralBase;
    Rp1FanWrite (Base + FAN_GPIO_CTRL,
      (MmioRead32 (Base + FAN_GPIO_CTRL) & ~(3U << 14)) | (2U << 14));
    Rp1FanWrite (Base + FAN_GPIO_RIO + (Data->FanRegisters[10] ? 0x2000 : 0x3000), FAN_GPIO_BIT);
    Rp1FanWrite (Base + FAN_GPIO_RIO + 4 + (Data->FanRegisters[11] ? 0x2000 : 0x3000), FAN_GPIO_BIT);
    Rp1FanWrite (Base + FAN_GPIO_PAD, Data->FanRegisters[9]);
    Rp1FanWrite (Base + FAN_GPIO_CTRL, Data->FanRegisters[8]);
    Data->FanPinChanged = FALSE;
  }
  Rp1FanWrite (Pwm, (Data->FanRegisters[0] & ~BIT3) | BIT31);
  Rp1FanWrite (Clock, MmioRead32 (Clock) & ~BIT11);
  Rp1FanWrite (Clock + 4, Data->FanRegisters[5]);
  Rp1FanWrite (Clock + 8, Data->FanRegisters[6]);
  Rp1FanWrite (Clock, Data->FanRegisters[4]);
  Rp1FanWrite (Pwm + 0x44, Data->FanRegisters[1]);
  Rp1FanWrite (Pwm + 0x48, Data->FanRegisters[2]);
  Rp1FanWrite (Pwm + 0x50, Data->FanRegisters[3]);
  Rp1FanWrite (Pwm + 0x4c, Data->FanRegisters[7]);
  Rp1FanWrite (Pwm, Data->FanRegisters[0] | BIT31);
  Data->FanActive = FALSE;
}

VOID
Rp1FanStart (IN RP1_BUS_DATA *Data)
{
  RP1_GPIO_CONFIG Config;
  UINTN Pwm, Clock;
  EFI_STATUS Status;

  if (Data->FanActive || !Rp1FanDetected ()) {
    return;
  }
  Status = Data->Rp1Io.GetGpioConfig (&Data->Rp1Io, 45, &Config);
  // Accept the bootloader's GPIO state only with the validated DT assignment.
  // Preserve other alternate-function owners even if the DT is inconsistent.
  if (EFI_ERROR (Status) || ((Config.Function != 0) && (Config.Function != RP1_GPIO_FUNCTION))) {
    DEBUG ((DEBUG_WARN, "RP1 fan: firmware pin assignment unavailable\n"));
    return;
  }
  Pwm = Data->PeripheralBase + RP1_PWM1_BASE;
  Clock = Data->PeripheralBase + RP1_CLOCKS_MAIN_BASE + 0x84;
  Data->FanRegisters[0] = MmioRead32 (Pwm);
  if ((Data->FanRegisters[0] & 7) != 0) {
    DEBUG ((DEBUG_WARN, "RP1 fan: PWM1 has other active users\n"));
    return;
  }
  Data->FanRegisters[1] = MmioRead32 (Pwm + 0x44);
  Data->FanRegisters[2] = MmioRead32 (Pwm + 0x48);
  Data->FanRegisters[3] = MmioRead32 (Pwm + 0x50);
  Data->FanRegisters[4] = MmioRead32 (Clock);
  Data->FanRegisters[5] = MmioRead32 (Clock + 4);
  Data->FanRegisters[6] = MmioRead32 (Clock + 8);
  Data->FanRegisters[7] = MmioRead32 (Pwm + 0x4c);
  Data->FanRegisters[8] = MmioRead32 (Data->PeripheralBase + FAN_GPIO_CTRL);
  Data->FanRegisters[9] = MmioRead32 (Data->PeripheralBase + FAN_GPIO_PAD);
  Data->FanRegisters[10] = MmioRead32 (Data->PeripheralBase + FAN_GPIO_RIO) & FAN_GPIO_BIT;
  Data->FanRegisters[11] = MmioRead32 (Data->PeripheralBase + FAN_GPIO_RIO + 4) & FAN_GPIO_BIT;
  // Only the detected fan uses this clock. Disable its old channel before
  // selecting XOSC, with the full-speed setting applied before re-enabling it.
  Status = Data->Rp1Io.ConfigurePwm (&Data->Rp1Io, 1, 3, 0, 0, TRUE, FALSE);
  if (!EFI_ERROR (Status)) {
    Rp1FanWrite (Pwm + 0x4c, 0);
    Status = Data->Rp1Io.ConfigurePwm (&Data->Rp1Io, 1, 3, 41566, 41566, TRUE, TRUE);
  }
  if (!EFI_ERROR (Status) && (Config.Function != 0)) {
    Config.Function = 0;
    Config.Pull = RP1_GPIO_PULL_DOWN;
    Config.Output = FALSE;
    Config.Value = FALSE;
    Data->FanPinChanged = TRUE;
    Status = Data->Rp1Io.ConfigureGpio (&Data->Rp1Io, 45, &Config);
  }
  if (!EFI_ERROR (Status)) {
    Status = Data->Rp1Io.GetGpioConfig (&Data->Rp1Io, 45, &Config);
    if (!EFI_ERROR (Status) &&
        ((Config.Function != 0) || ((MmioRead32 (Pwm) & 0x0f) != BIT3) ||
         (MmioRead32 (Pwm + 0x44) != 0x109) ||
         (MmioRead32 (Pwm + 0x48) != 2078) ||
         (MmioRead32 (Pwm + 0x4c) != 0) ||
         (MmioRead32 (Pwm + 0x50) != 2078) ||
         ((MmioRead32 (Clock) & 0xbe0) != 0x840) ||
         (MmioRead32 (Clock + 4) != 1) || (MmioRead32 (Clock + 8) != 0) ||
         (MmioRead32 (Clock + 12) == 0))) {
      Status = EFI_DEVICE_ERROR;
    }
  }
  Data->FanActive = TRUE;
  if (EFI_ERROR (Status)) {
    Rp1FanRestore (Data);
    DEBUG ((DEBUG_WARN, "RP1 fan: configuration failed: %r\n", Status));
    return;
  }
  DEBUG ((DEBUG_INFO, "RP1 fan: detected, full-speed UEFI cooling enabled\n"));
  // Keep cooling through ExitBootServices. The OS takes over these registers
  // with its own PWM driver. DriverBinding.Stop restores the firmware state.
}
