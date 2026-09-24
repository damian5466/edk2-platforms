/** @file
 *
 *  Copyright (c) 2023-2024, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Guid/EventGroup.h>
#include <Guid/RpiPlatformFormSetGuid.h>
#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/SerialPortConsoleRedirectionTable.h>
#include <IndustryStandard/Pci.h>
#include <IndustryStandard/PeImage.h>
#include <Library/AcpiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/FdtPlatformLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PeCoffGetEntryPointLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Protocol/Rp1Bus.h>
#include <Protocol/RpiFirmware.h>
#include <RpiPlatformVarStoreData.h>
#include <Rpi5McfgTable.h>
#include <ConfigVars.h>

#include "ConfigTable.h"
#include "RpiPlatformDxe.h"
#include "Peripherals.h"
#include "PciMemory.h"
#include "Rp1Handoff.h"
#include "AcpiDeviceGraph.h"

//
// AcpiTables.inf
//
STATIC CONST EFI_GUID mAcpiTableFile = {
  0x7E374E25, 0x8E01, 0x4FEE, { 0x87, 0xf2, 0x39, 0x0C, 0x23, 0xC6, 0x06, 0xCD }
};

STATIC ACPI_SD_COMPAT_MODE_VARSTORE_DATA    AcpiSdCompatMode;
STATIC ACPI_SD_LIMIT_UHS_VARSTORE_DATA      AcpiSdLimitUhs;

STATIC ACPI_PCIE_ECAM_COMPAT_MODE_VARSTORE_DATA          AcpiPcieEcamCompatMode;
STATIC ACPI_PCIE_32_BIT_BAR_SPACE_SIZE_MB_VARSTORE_DATA  AcpiPcie32BitBarSpaceSizeMB;

STATIC BOOLEAN                      mIsAcpiEnabled;
STATIC BOOLEAN                      mIsFdtEnabled;
STATIC BOOLEAN                      mPrepareRp1OnExitBootServices;
STATIC EFI_ACPI_SDT_PROTOCOL        *mAcpiSdtProtocol;
STATIC EFI_ACPI_DESCRIPTION_HEADER  *mDsdtTable;

STATIC UINT64  mAcpiPciMem32Base;
STATIC UINT64  mAcpiPciMem32Size;

STATIC EFI_EXIT_BOOT_SERVICES  mOriginalExitBootServices;

typedef enum {
  AcpiOsUnknown = 0,
  AcpiOsWindows,
} ACPI_OS_BOOT_TYPE;

#define SDT_PATTERN_LEN  (AML_NAME_SEG_SIZE + 1)

//
// Simple NameOp integer patcher.
// Does not allocate memory and can be safely used at ExitBootServices.
//
STATIC
EFI_STATUS
EFIAPI
AcpiUpdateSdtNameInteger (
  IN  EFI_ACPI_DESCRIPTION_HEADER  *AcpiTable,
  IN  CHAR8                        Name[AML_NAME_SEG_SIZE],
  IN  UINTN                        Value
  )
{
  UINTN   Index;
  CHAR8   Pattern[SDT_PATTERN_LEN];
  UINT8   *SdtPtr;
  UINT32  DataSize;
  UINT32  ValueOffset;

  if (AcpiTable->Length <= SDT_PATTERN_LEN) {
    return EFI_INVALID_PARAMETER;
  }

  SdtPtr = (UINT8 *)AcpiTable;
  //
  // Do a single NameOp variable replacement. These are of the
  // form "08 XXXX SIZE VAL", where SIZE is: 0A=byte, 0B=word, 0C=dword,
  // XXXX is the name and VAL is the value.
  //
  Pattern[0] = AML_NAME_OP;
  CopyMem (Pattern + 1, Name, AML_NAME_SEG_SIZE);

  ValueOffset = SDT_PATTERN_LEN + 1;

  for (Index = 0; Index < (AcpiTable->Length - SDT_PATTERN_LEN); Index++) {
    if (CompareMem (SdtPtr + Index, Pattern, SDT_PATTERN_LEN) == 0) {
      switch (SdtPtr[Index + SDT_PATTERN_LEN]) {
        case AML_QWORD_PREFIX:
          DataSize = sizeof (UINT64);
          break;
        case AML_DWORD_PREFIX:
          DataSize = sizeof (UINT32);
          break;
        case AML_WORD_PREFIX:
          DataSize = sizeof (UINT16);
          break;
        case AML_ONE_OP:
        case AML_ZERO_OP:
          ValueOffset--;
        // Fallthrough
        case AML_BYTE_PREFIX:
          DataSize = sizeof (UINT8);
          break;
        default:
          return EFI_UNSUPPORTED;
      }

      CopyMem (SdtPtr + Index + ValueOffset, &Value, DataSize);
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}

STATIC
VOID
EFIAPI
DsdtFixupStatus (
  IN EFI_ACPI_SDT_PROTOCOL    *AcpiSdtProtocol,
  IN EFI_ACPI_HANDLE          TableHandle
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  struct {
    CHAR8    *ObjectPath;
    BOOLEAN  Enabled;
  } DevStatus[] = {
    { "\\_SB.PCI0._STA", FALSE },                             // Not exposed
    { "\\_SB.PCI1._STA", mPciePlatform.Settings[1].Enabled }, // Configurable
    { "\\_SB.PCI2._STA", FALSE },                             // Reserved by RP1
  };

  for (Index = 0; Index < ARRAY_SIZE (DevStatus); Index++) {
    if (DevStatus[Index].Enabled == FALSE) {
      Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
                  DevStatus[Index].ObjectPath, 0x0);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a: Failed to patch %a. Status=%r\n",
                __func__, DevStatus[Index].ObjectPath, Status));
      }
    }
  }
}

STATIC
VOID
EFIAPI
DsdtFixupSoc (
  IN EFI_ACPI_SDT_PROTOCOL    *AcpiSdtProtocol,
  IN EFI_ACPI_HANDLE          TableHandle
  )
{
  VOID        *Fdt;
  UINTN       Revision;
  EFI_STATUS  Status;
  INT32       Node;
  INT32       Length;
  CONST UINT32 *Interrupts;
  UINT32      UartInterrupt;
  UINT32      AonServices;
  UINTN       Index;
  CONST UINT32 *Regs;
  CONST CHAR8 *DeviceStatus;
  CONST CHAR8 *AonPaths[] = {
    "/soc/pwm@7d517a80", "/soc/intc@7d517b00",
    "/soc@107c000000/pwm@7d517a80", "/soc@107c000000/intc@7d517b00"
  };
  UINTN       TableIndex;
  UINTN       TableKey;
  EFI_ACPI_SERIAL_PORT_CONSOLE_REDIRECTION_TABLE *Spcr;

  Revision = 0xAB;
  Fdt = FdtPlatformGetBase ();
  if (Fdt == NULL) {
    goto FixupUart;
  }

  if ((FdtNodeOffsetByCompatible (Fdt, -1, "brcm,bcm2712d0-pinctrl") >= 0) &&
      (FdtNodeOffsetByCompatible (Fdt, -1, "brcm,bcm2712d0-aon-pinctrl") >= 0)) {
    Revision = 1;
  } else if (((FdtNodeOffsetByCompatible (Fdt, -1, "brcm,bcm2712-pinctrl") >= 0) &&
              (FdtNodeOffsetByCompatible (Fdt, -1, "brcm,bcm2712-aon-pinctrl") >= 0)) ||
             ((FdtNodeOffsetByCompatible (Fdt, -1, "brcm,bcm2712c0-pinctrl") >= 0) &&
              (FdtNodeOffsetByCompatible (Fdt, -1, "brcm,bcm2712c0-aon-pinctrl") >= 0))) {
    Revision = 0;
  } else {
    DEBUG ((DEBUG_WARN, "%a: Unknown pinctrl layout; ACPI GPIO devices hidden.\n", __func__));
  }

  Status = AcpiAmlObjectUpdateInteger (
             AcpiSdtProtocol,
             TableHandle,
             "\\_SB.SREV",
             Revision
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch SREV. Status=%r\n", __func__, Status));
  }

FixupUart:
  AonServices = 0;
  if ((Fdt != NULL) && (Revision <= 1)) {
    for (Index = 0; Index < ARRAY_SIZE (AonPaths); Index++) {
      Node = FdtPathOffset (Fdt, AonPaths[Index]);
      if ((Node < 0) || (FdtStringListSearch (Fdt, Node, "compatible",
          (Index & 1) ? "brcm,bcm7271-l2-intc" : "brcm,bcm7038-pwm") < 0)) {
        continue;
      }
      DeviceStatus = FdtGetProp (Fdt, Node, "status", &Length);
      if ((DeviceStatus != NULL) &&
          !((Length == 5 && CompareMem (DeviceStatus, "okay", 5) == 0) ||
            (Length == 3 && CompareMem (DeviceStatus, "ok", 3) == 0))) {
        continue;
      }
      Regs = FdtGetProp (Fdt, Node, "reg", &Length);
      if ((Regs == NULL) || (Length != 2 * sizeof (UINT32)) ||
          (Fdt32ToCpu (Regs[0]) != ((Index & 1) ? 0x7D517B00 : 0x7D517A80)) ||
          (Fdt32ToCpu (Regs[1]) != ((Index & 1) ? 0x10 : 0x28))) {
        continue;
      }
      if (Index & 1) {
        Interrupts = FdtGetProp (Fdt, Node, "interrupts", &Length);
        if ((Interrupts == NULL) || (Length != 3 * sizeof (UINT32)) ||
            (Fdt32ToCpu (Interrupts[0]) != 0) ||
            (Fdt32ToCpu (Interrupts[1]) != 243) ||
            (Fdt32ToCpu (Interrupts[2]) != 4)) {
          continue;
        }
      }
      AonServices |= 1U << (Index & 1);
    }
  }
  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
             "\\_SB.AONS", AonServices);
  ASSERT_EFI_ERROR (Status);
  UartInterrupt = (Revision == 1) ? 152 : 153;
  if (Fdt != NULL) {
    Node = FdtPathOffset (Fdt, "/soc@107c000000/serial@7d001000");
    if (Node < 0) {
      Node = FdtPathOffset (Fdt, "/soc/serial@7d001000");
    }
    Interrupts = FdtGetProp (Fdt, Node, "interrupts", &Length);
    if ((Interrupts != NULL) && (Length == 3 * sizeof (UINT32)) &&
        (Fdt32ToCpu (Interrupts[0]) == 0) &&
        (Fdt32ToCpu (Interrupts[2]) == 4) &&
        (Fdt32ToCpu (Interrupts[1]) < 988)) {
      UartInterrupt = Fdt32ToCpu (Interrupts[1]) + 32;
    }
  }
  // Use a DWORD patch slot: a future DT can select a GSI above 255.
  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
             "\\_SB.SOCB.URT0.UINR", UartInterrupt);
  ASSERT_EFI_ERROR (Status);
  TableIndex = 0;
  Status = AcpiLocateTableBySignature (AcpiSdtProtocol,
             EFI_ACPI_6_4_SERIAL_PORT_CONSOLE_REDIRECTION_TABLE_SIGNATURE,
             &TableIndex, (EFI_ACPI_DESCRIPTION_HEADER **)&Spcr, &TableKey);
  if (!EFI_ERROR (Status)) {
    Spcr->GlobalSystemInterrupt = UartInterrupt;
    AcpiUpdateChecksum ((UINT8 *)Spcr, Spcr->Header.Length);
  }
}

STATIC VOID
DsdtFixupBluetooth (EFI_ACPI_SDT_PROTOCOL *AcpiSdtProtocol, EFI_ACPI_HANDLE TableHandle)
{
  VOID *Fdt;
  CONST UINT32 *Property;
  CONST UINT8 *Address;
  INT32 Node;
  INT32 Radio;
  INT32 Length;
  UINT32 Clock;
  UINT64 BdAddress;
  EFI_STATUS Status;

  Clock = 0;
  BdAddress = 0;
  Fdt = FdtPlatformGetBase ();
  if (Fdt != NULL) {
    Node = FdtPathOffset (Fdt, "/soc@107c000000/serial@7d50c000");
    if (Node < 0) {
      Node = FdtPathOffset (Fdt, "/soc/serial@7d50c000");
    }
    if ((Node >= 0) &&
        (FdtStringListSearch (Fdt, Node, "compatible", "brcm,bcm7271-uart") >= 0)) {
      Property = FdtGetProp (Fdt, Node, "clock-frequency", &Length);
      if ((Property != NULL) && (Length == sizeof (UINT32))) {
        Clock = Fdt32ToCpu (*Property);
        if ((Clock < 1000000) || (Clock > 500000000)) {
          Clock = 0;
        }
      }
      Radio = FdtSubnodeOffset (Fdt, Node, "bluetooth");
      if (Radio >= 0) {
        Address = FdtGetProp (Fdt, Radio, "local-bd-address", &Length);
        if ((Address != NULL) && (Length == 6)) {
          // The DT and HCI both use least-significant address byte first.
          CopyMem (&BdAddress, Address, 6);
          if (BdAddress == 0xFFFFFFFFFFFFULL) {
            BdAddress = 0;
          }
        }
      }
    }
  }
  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
             "\\_SB.BTU0.UCKH", Clock);
  ASSERT_EFI_ERROR (Status);
  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
             "\\_SB.BTH0.BADR", BdAddress);
  ASSERT_EFI_ERROR (Status);
  DEBUG ((DEBUG_INFO, "%a: UART clock %u Hz, Bluetooth identity %a.\n",
          __func__, Clock, (BdAddress != 0) ? "present" : "unavailable"));
}

STATIC
VOID
EFIAPI
DsdtFixupSd (
  IN EFI_ACPI_SDT_PROTOCOL    *AcpiSdtProtocol,
  IN EFI_ACPI_HANDLE          TableHandle
  )
{
  EFI_STATUS Status;
  VOID *Fdt;
  CONST UINT32 *Regs;
  INT32 Node;
  INT32 Length;
  UINTN Pads;

  Pads = 0;
  Fdt = FdtPlatformGetBase ();
  if (Fdt != NULL) {
    Node = FdtPathOffset (Fdt, "/axi/mmc@fff000");
    Regs = FdtGetProp (Fdt, Node, "reg", &Length);
    if ((Regs != NULL) && (Length == 16 * sizeof (UINT32)) &&
        (Fdt32ToCpu (Regs[8]) == 0x10) && (Fdt32ToCpu (Regs[9]) == 0x015040B0) &&
        (Fdt32ToCpu (Regs[10]) == 0) && (Fdt32ToCpu (Regs[11]) == 4) &&
        (Fdt32ToCpu (Regs[12]) == 0x10) && (Fdt32ToCpu (Regs[13]) == 0x015200F0) &&
        (Fdt32ToCpu (Regs[14]) == 0) && (Fdt32ToCpu (Regs[15]) == 0x24)) {
      Pads = 1;
    }
  }
  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle, "\\_SB.SDX0.SDPD", Pads);
  ASSERT_EFI_ERROR (Status);

  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
                "\\_SB.SDCM", AcpiSdCompatMode.Value);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch AcpiSdCompatMode.\n", __func__));
  }

  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
                "\\_SB.SDLU", AcpiSdLimitUhs.Value);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch AcpiSdLimitUhs.\n", __func__));
  }
}

STATIC VOID
DsdtFixupMailbox (EFI_ACPI_SDT_PROTOCOL *AcpiSdtProtocol, EFI_ACPI_HANDLE TableHandle)
{
  RASPBERRY_PI_FIRMWARE_PROTOCOL *Firmware;
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (&gRaspberryPiFirmwareProtocolGuid, NULL, (VOID **)&Firmware);
  if (!EFI_ERROR (Status)) {
    Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
               "\\_SB.SOCB.MBX0.MBST", Firmware->GetMailboxHandoff ());
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Mailbox handoff unavailable: %r\n", __func__, Status));
  }
}

STATIC VOID
DsdtFixupDma (EFI_ACPI_SDT_PROTOCOL *AcpiSdtProtocol, EFI_ACPI_HANDLE TableHandle)
{
  VOID *Fdt;
  CONST UINT32 *Property;
  INT32 Node;
  INT32 Length;
  UINTN Index;
  UINT32 Mask;
  UINT32 AllChannels;
  EFI_STATUS Status;
  CONST CHAR8 *Paths[] = { "/axi/dma@10000", "/axi/dma@10600" };
  CHAR8 *Names[] = { "\\_SB.DMA0.DM32", "\\_SB.DMA0.DM40" };

  Fdt = FdtPlatformGetBase ();
  AllChannels = 0;
  for (Index = 0; Index < ARRAY_SIZE (Paths); Index++) {
    Mask = 0; // No usable channel unless the firmware DT assigns it to the OS.
    if (Fdt != NULL) {
      Node = FdtPathOffset (Fdt, Paths[Index]);
      Property = FdtGetProp (Fdt, Node, "brcm,dma-channel-mask", &Length);
      if ((Property != NULL) && (Length == sizeof (UINT32))) {
        Mask = Fdt32ToCpu (*Property) & ((Index == 0) ? 0x3F : 0xFC0);
      }
    }
    Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle, Names[Index], Mask);
    ASSERT_EFI_ERROR (Status);
    AllChannels |= Mask;
  }
  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle, "\\_SB.DMA0.DMAL", AllChannels);
  ASSERT_EFI_ERROR (Status);
}

STATIC
VOID
EFIAPI
DsdtFixupRp1 (
  IN EFI_ACPI_SDT_PROTOCOL    *AcpiSdtProtocol,
  IN EFI_ACPI_HANDLE          TableHandle
  )
{
  EFI_STATUS        Status;
  RP1_BUS_PROTOCOL  *Rp1Bus;
  UINTN             HandleCount;
  EFI_HANDLE        *Handles;

  HandleCount = 0;
  Handles = NULL;
  Rp1Bus = NULL;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gRp1BusProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: Failed to locate RP1 instance! Status=%r\n",
      __func__,
      Status
      ));
    return;
  }

  if (HandleCount > 1) {
    DEBUG ((DEBUG_WARN, "%a: Only one RP1 instance is supported!\n", __func__));
  }

  Status = gBS->HandleProtocol (
                  Handles[0],
                  &gRp1BusProtocolGuid,
                  (VOID **)&Rp1Bus
                  );
  FreePool (Handles);

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: Failed to get RP1 bus protocol! Status=%r\n",
      __func__,
      Status
      ));
    return;
  }

  Status = AcpiAmlObjectUpdateInteger (
             AcpiSdtProtocol,
             TableHandle,
             "\\_SB.RP1B.PBAR",
             Rp1Bus->GetPeripheralBase (Rp1Bus)
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch PBAR. Status=%r\n", __func__, Status));
  }
  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
             "\\_SB.RP1B.SBAR", Rp1Bus->GetSramBase (Rp1Bus));
  ASSERT_EFI_ERROR (Status);
  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
             "\\_SB.RP1B.CHIP", Rp1Bus->GetChipId (Rp1Bus));
  ASSERT_EFI_ERROR (Status);
  Status = AcpiAmlObjectUpdateInteger (
             AcpiSdtProtocol,
             TableHandle,
             "\\_SB.RP1B.PWM1.FNRD",
             Rp1Bus->IsFanReady (Rp1Bus) ? 1 : 0
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch FNRD. Status=%r\n", __func__, Status));
  }
}

STATIC
VOID
EFIAPI
DsdtFixupPcie (
  IN EFI_ACPI_SDT_PROTOCOL    *AcpiSdtProtocol,
  IN EFI_ACPI_HANDLE          TableHandle
  )
{
  EFI_STATUS Status;

  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
                "\\_SB.BB32", mAcpiPciMem32Base);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch BB32.\n", __func__));
  }

  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
                "\\_SB.MS32", mAcpiPciMem32Size);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch MS32.\n", __func__));
  }
}

STATIC
EFI_STATUS
EFIAPI
AcpiFixupPcieEcam (
  IN ACPI_OS_BOOT_TYPE  OsType
  )
{
  EFI_STATUS                    Status;
  UINTN                         Index;
  RPI5_MCFG_TABLE               *McfgTable;
  EFI_ACPI_DESCRIPTION_HEADER   *FadtTable;
  UINTN                         TableKey;
  UINT32                        PcieEcamMode;
  UINT8                         PcieBusMax;

  Index = 0;
  Status = AcpiLocateTableBySignature (
             mAcpiSdtProtocol,
             EFI_ACPI_6_4_PCI_EXPRESS_MEMORY_MAPPED_CONFIGURATION_SPACE_BASE_ADDRESS_DESCRIPTION_TABLE_SIGNATURE,
             &Index,
             (EFI_ACPI_DESCRIPTION_HEADER **)&McfgTable,
             &TableKey);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Couldn't locate ACPI MCFG table! Status=%r\n",
            __func__, Status));
    return Status;
  }

  PcieEcamMode = AcpiPcieEcamCompatMode.Value;

  if (PcieEcamMode == ACPI_PCIE_ECAM_COMPAT_MODE_NXPMX6_DEN0115 ||
      PcieEcamMode == ACPI_PCIE_ECAM_COMPAT_MODE_NXPMX6_GRAVITON) {
    if (OsType == AcpiOsWindows) {
      PcieEcamMode = ACPI_PCIE_ECAM_COMPAT_MODE_NXPMX6;
    } else {
      PcieEcamMode &= ~ACPI_PCIE_ECAM_COMPAT_MODE_NXPMX6;
    }
  }

  switch (PcieEcamMode) {
    case ACPI_PCIE_ECAM_COMPAT_MODE_NXPMX6:
      PcieBusMax = 0;

      Index = 0;
      Status = AcpiLocateTableBySignature (
                mAcpiSdtProtocol,
                EFI_ACPI_6_3_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE,
                &Index,
                &FadtTable,
                &TableKey);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a: Couldn't locate ACPI FADT table! Status=%r\n",
                __func__, Status));
        return Status;
      }

      CopyMem (FadtTable->OemId, "NXPMX6", sizeof (FadtTable->OemId));
      AcpiUpdateChecksum ((UINT8 *)FadtTable, FadtTable->Length);
      break;

    case ACPI_PCIE_ECAM_COMPAT_MODE_GRAVITON:
      PcieBusMax = 0;

      CopyMem (McfgTable->Header.Header.OemId, "AMAZON", sizeof (McfgTable->Header.Header.OemId));
      McfgTable->Header.Header.OemTableId = SIGNATURE_64 ('G','R','A','V','I','T','O','N');
      McfgTable->Header.Header.OemRevision = 0;

      //
      // The ECAM window of the single function exposed on bus 0 is obtained
      // from the "AMZN0001" device in DSDT.
      // This causes a conflict with the region described in MCFG, but since
      // the latter is superfluous, we can simply point it to a bogus region
      // way above the register space.
      //
      for (Index = 0; Index < ARRAY_SIZE (McfgTable->Entries); Index++) {
        McfgTable->Entries[Index].BaseAddress = BASE_1TB + (Index * SIZE_1MB);
      }
      break;

    default: // ACPI_PCIE_ECAM_COMPAT_MODE_DEN0115
      PcieBusMax = PCI_MAX_BUS;

      // MCFG must be hidden.
      McfgTable->Header.Header.Signature = 0;
      break;
  }

  AcpiUpdateChecksum ((UINT8 *)McfgTable, McfgTable->Header.Header.Length);

  AcpiUpdateSdtNameInteger (mDsdtTable, "PBMA", PcieBusMax);

  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
AcpiOsBootHandler (
  IN ACPI_OS_BOOT_TYPE  OsType
  )
{
  if ((mAcpiSdtProtocol == NULL) || (mDsdtTable == NULL)) {
    ASSERT (FALSE);
    return;
  }

  AcpiFixupPcieEcam (OsType);

  AcpiUpdateChecksum ((UINT8 *)mDsdtTable, mDsdtTable->Length);
}

STATIC
UINTN
EFIAPI
FindPeImageBase (
  EFI_PHYSICAL_ADDRESS  Base
  )
{
  EFI_IMAGE_DOS_HEADER                 *DosHdr;
  EFI_IMAGE_OPTIONAL_HEADER_PTR_UNION  Hdr;

  Base &= ~(EFI_PAGE_SIZE - 1);

  while (Base != 0) {
    DosHdr = (EFI_IMAGE_DOS_HEADER *)Base;
    if (DosHdr->e_magic == EFI_IMAGE_DOS_SIGNATURE) {
      Hdr.Pe32 = (EFI_IMAGE_NT_HEADERS32 *)(Base + DosHdr->e_lfanew);
      if (Hdr.Pe32->Signature == EFI_IMAGE_NT_SIGNATURE) {
        break;
      }
    }

    Base -= EFI_PAGE_SIZE;
  }

  return Base;
}

STATIC CHAR8 mWinLoadNameStr[] = "winload";
#define PDB_NAME_MAX_LENGTH   256

//
// Run after the TPL_NOTIFY xHCI shutdown callbacks have stopped using RP1.
//
STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mPrepareRp1OnExitBootServices) {
    Rp1PrepareForFdtBoot ();
  }
}

STATIC
BOOLEAN
EFIAPI
IsPeImageWinLoader (
  IN VOID *PeImage
 )
{
  CHAR8  *PdbStr;
  UINTN  WinLoadNameStrLen;
  UINTN  Index;

  PdbStr = (CHAR8 *)PeCoffLoaderGetPdbPointer (PeImage);
  if (PdbStr == NULL) {
    return FALSE;
  }

  WinLoadNameStrLen = sizeof (mWinLoadNameStr) - sizeof (CHAR8);

  for (Index = 0; Index < PDB_NAME_MAX_LENGTH && PdbStr[Index] != '\0'; Index++) {
    if (AsciiStrnCmp (PdbStr + Index, mWinLoadNameStr, WinLoadNameStrLen) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
EFI_STATUS
EFIAPI
AcpiExitBootServicesHook (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       MapKey
  )
{
  UINTN               ReturnAddress;
  UINTN               OsLoaderAddress;
  ACPI_OS_BOOT_TYPE   OsType;

  ReturnAddress = (UINTN)RETURN_ADDRESS (0);

  gBS->ExitBootServices = mOriginalExitBootServices;

  OsType = AcpiOsUnknown;

  OsLoaderAddress = FindPeImageBase (ReturnAddress);
  if (OsLoaderAddress > 0) {
    if (IsPeImageWinLoader ((VOID *)OsLoaderAddress)) {
      OsType = AcpiOsWindows;
    }
  }

  if (mIsAcpiEnabled) {
    AcpiOsBootHandler (OsType);
  }

  if (mIsFdtEnabled) {
    mPrepareRp1OnExitBootServices = (OsType != AcpiOsWindows);
  }

  return gBS->ExitBootServices (ImageHandle, MapKey);
}

STATIC
EFI_STATUS
EFIAPI
InstallAcpiTables (
  VOID
  )
{
  EFI_STATUS       Status;
  UINTN            TableKey;
  UINTN            TableIndex;
  EFI_ACPI_HANDLE  TableHandle;

  Status = gBS->LocateProtocol (
                  &gEfiAcpiSdtProtocolGuid,
                  NULL,
                  (VOID **)&mAcpiSdtProtocol
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Couldn't locate gEfiAcpiSdtProtocolGuid!\n", __func__));
    return Status;
  }

  Status = LocateAndInstallAcpiFromFvConditional (&mAcpiTableFile, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to install ACPI tables!\n"));
    return Status;
  }

  TableIndex = 0;
  Status = AcpiLocateTableBySignature (
             mAcpiSdtProtocol,
             EFI_ACPI_6_3_DIFFERENTIATED_SYSTEM_DESCRIPTION_TABLE_SIGNATURE,
             &TableIndex,
             &mDsdtTable,
             &TableKey
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Couldn't locate ACPI DSDT table!\n", __func__));
    return Status;
  }

  Status = mAcpiSdtProtocol->OpenSdt (TableKey, &TableHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Couldn't open ACPI DSDT table!\n", __func__));
    mAcpiSdtProtocol->Close (TableHandle);
    return Status;
  }

  DsdtFixupStatus (mAcpiSdtProtocol, TableHandle);
  DsdtFixupSoc (mAcpiSdtProtocol, TableHandle);
  DsdtFixupBluetooth (mAcpiSdtProtocol, TableHandle);
  DsdtFixupMailbox (mAcpiSdtProtocol, TableHandle);
  DsdtFixupDma (mAcpiSdtProtocol, TableHandle);
  DsdtFixupSd (mAcpiSdtProtocol, TableHandle);
  DsdtFixupRp1 (mAcpiSdtProtocol, TableHandle);
  DsdtFixupPcie (mAcpiSdtProtocol, TableHandle);

  mAcpiSdtProtocol->Close (TableHandle);

  // This SSDT retains the complete boot DT wiring graph as ACPI data. _CRS
  // remains authoritative for MMIO, interrupts and the firmware DMA mapping.
  Status = InstallAcpiDeviceGraph ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Device graph installation failed: %r\n", __func__, Status));
    return Status;
  }

  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->CloseEvent (Event);

  AdjustPciReservedMemory (
    mIsAcpiEnabled,
    AcpiPcie32BitBarSpaceSizeMB.Value,
    mSystemMemorySize,
    &mAcpiPciMem32Base,
    &mAcpiPciMem32Size
    );

  if (mIsAcpiEnabled) {
    InstallAcpiTables ();
  } else {
    // FDT installation is done by FdtDxe.
  }
}

VOID
EFIAPI
ApplyConfigTableVariables (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  mIsAcpiEnabled = PcdGet32 (PcdSystemTableMode) == SYSTEM_TABLE_MODE_ACPI ||
                   PcdGet32 (PcdSystemTableMode) == SYSTEM_TABLE_MODE_BOTH;
  mIsFdtEnabled = PcdGet32 (PcdSystemTableMode) == SYSTEM_TABLE_MODE_DT ||
                  PcdGet32 (PcdSystemTableMode) == SYSTEM_TABLE_MODE_BOTH;
  mPrepareRp1OnExitBootServices = PcdGet32 (PcdSystemTableMode) == SYSTEM_TABLE_MODE_DT;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &Event
                  );
  ASSERT_EFI_ERROR (Status);

  if (mIsFdtEnabled) {
    Status = gBS->CreateEventEx (
                    EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    OnExitBootServices,
                    NULL,
                    &gEfiEventExitBootServicesGuid,
                    &Event
                    );
    ASSERT_EFI_ERROR (Status);
  }

  if (mIsAcpiEnabled) {
    mOriginalExitBootServices = gBS->ExitBootServices;
    gBS->ExitBootServices = AcpiExitBootServicesHook;
  }
}

VOID
EFIAPI
SetupConfigTableVariables (
  VOID
  )
{
  EFI_STATUS    Status;
  UINTN         Size;
  UINT32        Var32;

  AcpiSdCompatMode.Value = ACPI_SD_COMPAT_MODE_DEFAULT;
  AcpiSdLimitUhs.Value = ACPI_SD_LIMIT_UHS_DEFAULT;
  AcpiPcieEcamCompatMode.Value = ACPI_PCIE_ECAM_COMPAT_MODE_DEFAULT;
  AcpiPcie32BitBarSpaceSizeMB.Value = ACPI_PCIE_32_BIT_BAR_SPACE_SIZE_MB_DEFAULT;

  Size = sizeof (ACPI_SD_COMPAT_MODE_VARSTORE_DATA);
  Status = gRT->GetVariable (L"AcpiSdCompatMode",
                  &gRpiPlatformFormSetGuid,
                  NULL, &Size, &AcpiSdCompatMode);
  if (EFI_ERROR (Status)) {
    Status = gRT->SetVariable (
                    L"AcpiSdCompatMode",
                    &gRpiPlatformFormSetGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                    Size,
                    &AcpiSdCompatMode);
    ASSERT_EFI_ERROR (Status);
  }

  Size = sizeof (ACPI_SD_LIMIT_UHS_VARSTORE_DATA);
  Status = gRT->GetVariable (L"AcpiSdLimitUhs",
                  &gRpiPlatformFormSetGuid,
                  NULL, &Size, &AcpiSdLimitUhs);
  if (EFI_ERROR (Status)) {
    Status = gRT->SetVariable (
                    L"AcpiSdLimitUhs",
                    &gRpiPlatformFormSetGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                    Size,
                    &AcpiSdLimitUhs);
    ASSERT_EFI_ERROR (Status);
  }

  Size = sizeof (ACPI_PCIE_ECAM_COMPAT_MODE_VARSTORE_DATA);
  Status = gRT->GetVariable (L"AcpiPcieEcamCompatMode",
                  &gRpiPlatformFormSetGuid,
                  NULL, &Size, &AcpiPcieEcamCompatMode);
  if (EFI_ERROR (Status)) {
    Status = gRT->SetVariable (
                    L"AcpiPcieEcamCompatMode",
                    &gRpiPlatformFormSetGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                    Size,
                    &AcpiPcieEcamCompatMode);
    ASSERT_EFI_ERROR (Status);
  }

  Size = sizeof (ACPI_PCIE_32_BIT_BAR_SPACE_SIZE_MB_VARSTORE_DATA);
  Status = gRT->GetVariable (L"AcpiPcie32BitBarSpaceSizeMB",
                  &gRpiPlatformFormSetGuid,
                  NULL, &Size, &AcpiPcie32BitBarSpaceSizeMB);
  if (EFI_ERROR (Status)) {
    Status = gRT->SetVariable (
                    L"AcpiPcie32BitBarSpaceSizeMB",
                    &gRpiPlatformFormSetGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                    Size,
                    &AcpiPcie32BitBarSpaceSizeMB);
    ASSERT_EFI_ERROR (Status);
  }

  Size = sizeof (UINT32);
  Status = gRT->GetVariable (L"SystemTableMode",
                  &gRpiPlatformFormSetGuid,
                  NULL, &Size, &Var32);
  if (EFI_ERROR (Status)) {
    Status = PcdSet32S (PcdSystemTableMode, PcdGet32 (PcdSystemTableMode));
    ASSERT_EFI_ERROR (Status);
  }
}
