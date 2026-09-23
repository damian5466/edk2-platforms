/** @file
 * Prepare the RP1 PCI hierarchy for enumeration by a device-tree OS.
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 **/

#include "Rp1Handoff.h"

#include <IndustryStandard/Bcm2712.h>
#include <IndustryStandard/Bcm2712Pcie.h>
#include <IndustryStandard/Pci.h>
#include <Library/IoLib.h>
#include <Rp1.h>

VOID
Rp1PrepareForFdtBoot (
  VOID
  )
{
  UINTN   PcieBase;
  UINTN   ConfigBase;
  UINT32  BusNumbers;
  UINT32  ConfigIndex;
  UINT32  LinkMask;
  UINT32  LinkStatus;
  UINTN   Index;

  PcieBase = BCM2712_BRCMSTB_PCIE2_BASE;
  LinkMask = PCIE_MISC_PCIE_STATUS_PCIE_DL_ACTIVE_MASK |
             PCIE_MISC_PCIE_STATUS_PCIE_PHYLINKUP_MASK;
  LinkStatus = MmioRead32 (PcieBase + PCIE_MISC_PCIE_STATUS);
  if ((LinkStatus == MAX_UINT32) || ((LinkStatus & LinkMask) != LinkMask)) {
    return;
  }

  BusNumbers = MmioRead32 (PcieBase + OFFSET_OF (PCI_TYPE01, Bridge.PrimaryBus));
  if ((BusNumbers == MAX_UINT32) || ((BusNumbers & 0xFF00) == 0)) {
    return;
  }

  // The fixed RP1 endpoint is device 0, function 0 on the secondary bus.
  // Direct MMIO avoids allocating or using boot-service protocol interfaces
  // during ExitBootServices. All firmware PCI consumers have stopped by now.
  ConfigIndex = MmioRead32 (PcieBase + PCIE_EXT_CFG_INDEX);
  MmioWrite32 (PcieBase + PCIE_EXT_CFG_INDEX, (BusNumbers & 0xFF00) << 12);
  ConfigBase = PcieBase + PCIE_EXT_CFG_DATA;
  if (MmioRead32 (ConfigBase) !=
      ((PCI_DEVICE_ID_RP1 << 16) | PCI_VENDOR_ID_RPILTD)) {
    MmioWrite32 (PcieBase + PCIE_EXT_CFG_INDEX, ConfigIndex);
    return;
  }

  // Keep the link and RP1 configuration alive while releasing UEFI addresses.
  // RP1's class and BAR geometry are initialized by the platform firmware;
  // this handoff must not request a fundamental reset of the endpoint.
  // Disable DMA and address decoding BEFORE changing any endpoint BAR.
  MmioAnd16 (
    ConfigBase + OFFSET_OF (PCI_TYPE00, Hdr.Command),
    (UINT16)~(EFI_PCI_COMMAND_IO_SPACE |
              EFI_PCI_COMMAND_MEMORY_SPACE |
              EFI_PCI_COMMAND_BUS_MASTER)
    );
  for (Index = 0; Index < 3; Index++) {
    // RP1 has three 32-bit memory BARs (16 KiB, 4 MiB and 64 KiB).
    MmioWrite32 (ConfigBase + OFFSET_OF (PCI_TYPE00, Device.Bar) +
                 Index * sizeof (UINT32), 0);
  }

  // UEFI and the supplied DT use different PCI bus address translations.
  // Close the bridge windows so Linux assigns the complete hierarchy again.
  MmioAnd16 (
    PcieBase + OFFSET_OF (PCI_TYPE01, Hdr.Command),
    (UINT16)~(EFI_PCI_COMMAND_IO_SPACE |
              EFI_PCI_COMMAND_MEMORY_SPACE |
              EFI_PCI_COMMAND_BUS_MASTER)
    );
  MmioWrite16 (PcieBase + OFFSET_OF (PCI_TYPE01, Bridge.MemoryBase), 0xFFF0);
  MmioWrite16 (PcieBase + OFFSET_OF (PCI_TYPE01, Bridge.MemoryLimit), 0);
  MmioWrite16 (PcieBase + OFFSET_OF (PCI_TYPE01, Bridge.PrefetchableMemoryBase), 0xFFF0);
  MmioWrite16 (PcieBase + OFFSET_OF (PCI_TYPE01, Bridge.PrefetchableMemoryLimit), 0);
  MmioWrite32 (PcieBase + OFFSET_OF (PCI_TYPE01, Bridge.PrefetchableBaseUpper32), 0);
  MmioWrite32 (PcieBase + OFFSET_OF (PCI_TYPE01, Bridge.PrefetchableLimitUpper32), 0);
  MmioWrite8 (PcieBase + OFFSET_OF (PCI_TYPE01, Bridge.IoBase), 0xF0);
  MmioWrite8 (PcieBase + OFFSET_OF (PCI_TYPE01, Bridge.IoLimit), 0);
  MmioWrite16 (PcieBase + OFFSET_OF (PCI_TYPE01, Bridge.IoBaseUpper16), 0);
  MmioWrite16 (PcieBase + OFFSET_OF (PCI_TYPE01, Bridge.IoLimitUpper16), 0);
  // Drop bus routing last, after all endpoint accesses have completed.
  MmioWrite32 (PcieBase + OFFSET_OF (PCI_TYPE01, Bridge.PrimaryBus),
               BusNumbers & 0xFF000000);
  MmioWrite32 (PcieBase + PCIE_EXT_CFG_INDEX, ConfigIndex);
}
