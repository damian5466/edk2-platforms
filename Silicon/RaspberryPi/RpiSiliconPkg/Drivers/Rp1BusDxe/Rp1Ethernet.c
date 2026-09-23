/** @file
  Pi 5 Ethernet prerequisites for ACPI operating systems.
  GPIO32 releases the board PHY; the network driver owns GEM/MDIO/DMA.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/FdtPlatformLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Rp1.h>
#include "Rp1BusDxe.h"

#define ETH_RESET_CTRL  (RP1_IO_BANK0_BASE + 0x4000 + 4 * 8 + 4)
#define ETH_RESET_PAD   (RP1_PADS_BANK0_BASE + 0x4000 + 4 * 4 + 4)
#define ETH_RESET_RIO   (RP1_SYS_RIO0_BASE + 0x4000)
#define ETH_CLOCK      (RP1_CLOCKS_MAIN_BASE + 0x64)
#define ETH_TSU_CLOCK  (RP1_CLOCKS_MAIN_BASE + 0x134)
#define ETH_PLL        (RP1_CLOCKS_MAIN_BASE + 0x8000)
#define ETH_MAC_LOW    (RP1_ETH_BASE + 0x88)
#define ETH_MAC_HIGH   (RP1_ETH_BASE + 0x8c)

STATIC CONST UINTN mEthernetSavedOffsets[] = {
  ETH_RESET_CTRL, ETH_RESET_PAD, ETH_RESET_RIO, ETH_RESET_RIO + 4,
  ETH_CLOCK, ETH_CLOCK + 4, ETH_TSU_CLOCK, ETH_TSU_CLOCK + 4,
  ETH_MAC_LOW, ETH_MAC_HIGH
};

STATIC BOOLEAN
EthernetCompatible (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Compatible)
{
  INT32 Length;
  CONST CHAR8 *Value = FdtGetProp (Fdt, Node, "compatible", &Length);
  return (Value != NULL) && (Length > 0) &&
         FdtStringListContains (Value, Length, Compatible);
}

STATIC INT32
EthernetNode (CONST VOID *Fdt)
{
  INT32 Node, Gpio, Length;
  CONST CHAR8 *Status;
  CONST UINT32 *Cells;

  if ((Fdt == NULL) || !EthernetCompatible (Fdt, 0, "raspberrypi,5-model-b")) return -1;
  Node = FdtNodeOffsetByCompatible (Fdt, -1, "raspberrypi,rp1-gem");
  if (Node < 0) return -1;
  Status = FdtGetProp (Fdt, Node, "status", &Length);
  if ((Status == NULL) || (Length != 5) || CompareMem (Status, "okay", 5)) return -1;
  Cells = FdtGetProp (Fdt, Node, "reg", &Length);
  if ((Cells == NULL) || (Length != 16) || Fdt32ToCpu (Cells[0]) != 0xc0 ||
      Fdt32ToCpu (Cells[1]) != 0x40100000 || Fdt32ToCpu (Cells[2]) != 0 ||
      Fdt32ToCpu (Cells[3]) != 0x4000) return -1;
  Cells = FdtGetProp (Fdt, Node, "phy-reset-gpios", &Length);
  if ((Cells == NULL) || (Length != 12) || Fdt32ToCpu (Cells[1]) != 32 ||
      Fdt32ToCpu (Cells[2]) != 1) return -1;
  Gpio = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Cells[0]));
  if ((Gpio < 0) || !EthernetCompatible (Fdt, Gpio, "raspberrypi,rp1-gpio")) return -1;
  return Node;
}

STATIC VOID
EthernetWrite (UINTN Base, UINTN Offset, UINT32 Value)
{
  MmioWrite32 (Base + Offset, Value);
  // NSR is harmless to read and drains writes, including GPIO atomic aliases.
  MmioRead32 (Base + RP1_ETH_BASE + 8);
}

VOID
Rp1EthernetRestore (IN RP1_BUS_DATA *Data)
{
  UINTN Base;
  if (!Data->EthernetActive) return;
  Base = (UINTN)Data->PeripheralBase;
  Data->EthernetActive = FALSE;
  EthernetWrite (Base, ETH_RESET_CTRL, (MmioRead32 (Base + ETH_RESET_CTRL) & ~(3U << 14)) | (2U << 14));
  EthernetWrite (Base, ETH_RESET_RIO + ((Data->EthernetRegisters[2] & BIT4) ? 0x2000 : 0x3000), BIT4);
  EthernetWrite (Base, ETH_RESET_RIO + 4 + ((Data->EthernetRegisters[3] & BIT4) ? 0x2000 : 0x3000), BIT4);
  EthernetWrite (Base, ETH_RESET_PAD, Data->EthernetRegisters[1]);
  EthernetWrite (Base, ETH_RESET_CTRL, Data->EthernetRegisters[0]);
  EthernetWrite (Base, ETH_CLOCK, MmioRead32 (Base + ETH_CLOCK) & ~BIT11);
  EthernetWrite (Base, ETH_CLOCK + 4, Data->EthernetRegisters[5]);
  EthernetWrite (Base, ETH_CLOCK, Data->EthernetRegisters[4]);
  EthernetWrite (Base, ETH_TSU_CLOCK, MmioRead32 (Base + ETH_TSU_CLOCK) & ~BIT11);
  EthernetWrite (Base, ETH_TSU_CLOCK + 4, Data->EthernetRegisters[7]);
  EthernetWrite (Base, ETH_TSU_CLOCK, Data->EthernetRegisters[6]);
  EthernetWrite (Base, ETH_MAC_LOW, Data->EthernetRegisters[8]);
  EthernetWrite (Base, ETH_MAC_HIGH, Data->EthernetRegisters[9]);
}

STATIC BOOLEAN
EthernetClock (UINTN Base, UINTN Offset)
{
  UINT32 Control = MmioRead32 (Base + Offset);
  UINTN Retry;
  EthernetWrite (Base, Offset, Control & ~BIT11);
  EthernetWrite (Base, Offset + 4, 1);
  // Both clocks use auxiliary source 0: SYS secondary / XOSC respectively.
  EthernetWrite (Base, Offset, (Control & ~0x3e0U) | BIT11);
  for (Retry = 0; Retry < 1000; Retry++) {
    if (MmioRead32 (Base + Offset + 12) != 0) return TRUE;
    MicroSecondDelay (1);
  }
  return FALSE;
}

VOID
Rp1EthernetStart (IN RP1_BUS_DATA *Data)
{
  CONST VOID *Fdt = FdtPlatformGetBase ();
  CONST UINT8 *Mac;
  INT32 Node, Length;
  UINTN Base, Index;
  UINT32 Cs, Power, Fb, Frac, Sec, Any;
  RP1_GPIO_CONFIG Pin = { RP1_GPIO_FUNCTION, RP1_GPIO_PULL_NONE, TRUE, FALSE };
  EFI_STATUS Status;

  if (Data->EthernetActive) return;
  Node = EthernetNode (Fdt);
  if (Node < 0) return;
  Base = (UINTN)Data->PeripheralBase;
  if (MmioRead32 (Base + RP1_ETH_BASE + 0xfc) != 0x00070109 ||
      (MmioRead32 (Base + RP1_ETH_BASE) & (BIT2 | BIT3))) {
    DEBUG ((DEBUG_WARN, "RP1 Ethernet: unknown or active GEM, leaving it alone\n"));
    return;
  }
  for (Index = 0; Index < ARRAY_SIZE (mEthernetSavedOffsets); Index++)
    Data->EthernetRegisters[Index] = MmioRead32 (Base + mEthernetSavedOffsets[Index]);
  DEBUG ((DEBUG_INFO, "RP1 Ethernet: GPIO32 ctrl=%x pad=%x out=%x oe=%x; clocks=%x/%x %x/%x\n",
    Data->EthernetRegisters[0], Data->EthernetRegisters[1], Data->EthernetRegisters[2],
    Data->EthernetRegisters[3], Data->EthernetRegisters[4], Data->EthernetRegisters[5],
    Data->EthernetRegisters[6], Data->EthernetRegisters[7]));
  Cs = MmioRead32 (Base + ETH_PLL);
  Power = MmioRead32 (Base + ETH_PLL + 4);
  Fb = MmioRead32 (Base + ETH_PLL + 8);
  Frac = MmioRead32 (Base + ETH_PLL + 12);
  Sec = MmioRead32 (Base + ETH_PLL + 20);
  DEBUG ((DEBUG_INFO, "RP1 Ethernet: PLL_SYS cs=%x pwr=%x fb=%x frac=%x sec=%x\n", Cs, Power, Fb, Frac, Sec));

  // Never reprogram the PLL shared with USB/system clocks. Only enable the
  // dedicated Ethernet clocks when firmware already supplies the known 125MHz.
  if (!(Cs & BIT31) || (Cs & 0x3f) != 1 || (Power & (BIT0 | BIT5)) ||
      Fb != 20 || Frac != 0 || ((Sec >> 8) & 0x1f) != 8 || (Sec & BIT16)) {
    DEBUG ((DEBUG_WARN, "RP1 Ethernet: expected 125MHz PLL source unavailable; skipped\n"));
    return;
  }
  // Configure the dedicated reset pin low before enabling clocks, then release
  // after the board's documented 5ms pulse. No GEM DMA or IRQ is enabled here.
  Status = Data->Rp1Io.ConfigureGpio (&Data->Rp1Io, 32, &Pin);
  Data->EthernetActive = TRUE;
  if (EFI_ERROR (Status) || !EthernetClock (Base, ETH_CLOCK) || !EthernetClock (Base, ETH_TSU_CLOCK)) {
    Rp1EthernetRestore (Data);
    DEBUG ((DEBUG_WARN, "RP1 Ethernet: reset/clock setup failed\n"));
    return;
  }
  MicroSecondDelay (5000);
  // Use our own atomic latch write while the pin is reserved by this policy.
  EthernetWrite (Base, ETH_RESET_RIO + 0x2000, BIT4);
  MicroSecondDelay (15000);
  Mac = FdtGetProp (Fdt, Node, "local-mac-address", &Length);
  if ((Mac == NULL) || Length != 6) Mac = FdtGetProp (Fdt, Node, "mac-address", &Length);
  Any = 0;
  if ((Mac != NULL) && Length == 6 && !(Mac[0] & 1)) {
    for (Index = 0; Index < 6; Index++) Any |= Mac[Index];
    if (Any != 0) {
      EthernetWrite (Base, ETH_MAC_LOW, Mac[0] | (Mac[1] << 8) | (Mac[2] << 16) | ((UINT32)Mac[3] << 24));
      EthernetWrite (Base, ETH_MAC_HIGH, Mac[4] | (Mac[5] << 8));
    }
  }
  DEBUG ((DEBUG_INFO, "RP1 Ethernet: clocks enabled, PHY reset released, MAC=%08x/%04x\n",
    MmioRead32 (Base + ETH_MAC_LOW), MmioRead32 (Base + ETH_MAC_HIGH)));
}
