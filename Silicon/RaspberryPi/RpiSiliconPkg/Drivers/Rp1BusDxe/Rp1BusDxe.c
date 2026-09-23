/** @file
 *
 *  Copyright (c) 2023-2024, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Uefi.h>
#include <IndustryStandard/Pci.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Rp1.h>

#include "Rp1BusDxe.h"

// Retain ownership independently of public protocols. A failed teardown must
// remain reachable by Stop even after those protocols have been uninstalled.
STATIC RP1_BUS_DATA *mRp1Controllers;

STATIC RP1_BUS_DATA *
Rp1BusFindController (EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE Controller)
{
  RP1_BUS_DATA *Data;

  for (Data = mRp1Controllers; Data != NULL; Data = Data->Next) {
    if ((Data->DriverBinding == This) && (Data->ControllerHandle == Controller)) {
      return Data;
    }
  }
  return NULL;
}

STATIC
EFI_STATUS
Rp1BusUnregisterNonDiscoverableDevice (
  IN RP1_BUS_DATA *Rp1Data,
  IN UINTN Index
  );

STATIC
EFI_STATUS
EFIAPI
Rp1BusRegisterDwc3Controllers (
  IN RP1_BUS_DATA  *Rp1Data
  )
{
  EFI_STATUS            Status;
  UINTN                 Index;
  EFI_PHYSICAL_ADDRESS  FullBase;
  EFI_HANDLE            DeviceHandle;
  RP1_BUS_PROTOCOL      *Rp1Bus;

  EFI_PHYSICAL_ADDRESS  Dwc3Addresses[] = {
    RP1_USBHOST0_BASE, RP1_USBHOST1_BASE
  };

  for (Index = 0; Index < ARRAY_SIZE (Dwc3Addresses); Index++) {
    DeviceHandle = NULL;
    FullBase     = Rp1Data->PeripheralBase + Dwc3Addresses[Index];

    Status = RegisterNonDiscoverableMmioDevice (
               NonDiscoverableDeviceTypeXhci,
               NonDiscoverableDeviceDmaTypeNonCoherent,
               NULL,
               &DeviceHandle,
               1,
               FullBase,
               (UINTN)RP1_USBHOST_SIZE
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "RP1: Failed to register DWC3 controller at 0x%lx. Status=%r\n",
        FullBase,
        Status
        ));
      return Status;
    }

    Rp1Data->UsbChildren[Index] = DeviceHandle;

    Status = gBS->OpenProtocol (
                    Rp1Data->ControllerHandle,
                    &gRp1BusProtocolGuid,
                    (VOID **)&Rp1Bus,
                    Rp1Data->DriverBinding->DriverBindingHandle,
                    DeviceHandle,
                    EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "RP1: Failed to open DWC3 by controller. Status=%r\n",
        Status
        ));
      return Status;
    }
    Rp1Data->UsbChildOpened[Index] = TRUE;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Rp1BusRegisterDevices (
  IN RP1_BUS_DATA  *Rp1Data
  )
{
  return Rp1BusRegisterDwc3Controllers (Rp1Data);
}

STATIC
VOID
EFIAPI
Rp1BusEnableInterrupts (
  IN RP1_BUS_DATA  *Rp1Data
  )
{
  Rp1Data->UsbInterruptEnable[0] = MmioRead32 (
    Rp1Data->PeripheralBase + RP1_PCIE_REG_RW + RP1_PCIE_MSIX_CFG (RP1_INT_USBHOST0_0)
    ) & RP1_PCIE_MSIX_CFG_ENABLE;
  Rp1Data->UsbInterruptEnable[1] = MmioRead32 (
    Rp1Data->PeripheralBase + RP1_PCIE_REG_RW + RP1_PCIE_MSIX_CFG (RP1_INT_USBHOST1_0)
    ) & RP1_PCIE_MSIX_CFG_ENABLE;
  MmioWrite32 (
    Rp1Data->PeripheralBase + RP1_PCIE_REG_SET + RP1_PCIE_MSIX_CFG (RP1_INT_USBHOST0_0),
    RP1_PCIE_MSIX_CFG_ENABLE
    );
  MmioWrite32 (
    Rp1Data->PeripheralBase + RP1_PCIE_REG_SET + RP1_PCIE_MSIX_CFG (RP1_INT_USBHOST1_0),
    RP1_PCIE_MSIX_CFG_ENABLE
    );
}

STATIC
VOID
Rp1BusRestoreInterrupts (
  IN RP1_BUS_DATA *Rp1Data
  )
{
  UINTN Index;
  UINTN Interrupts[] = { RP1_INT_USBHOST0_0, RP1_INT_USBHOST1_0 };

  for (Index = 0; Index < ARRAY_SIZE (Interrupts); Index++) {
    MmioWrite32 (Rp1Data->PeripheralBase +
      (Rp1Data->UsbInterruptEnable[Index] ? RP1_PCIE_REG_SET : RP1_PCIE_REG_CLR) +
      RP1_PCIE_MSIX_CFG (Interrupts[Index]), RP1_PCIE_MSIX_CFG_ENABLE);
  }
}

STATIC
EFI_PHYSICAL_ADDRESS
EFIAPI
Rp1BusGetPeripheralBase (
  IN RP1_BUS_PROTOCOL  *This
  )
{
  ASSERT (This != NULL);

  return (RP1_BUS_DATA_FROM_THIS (This))->PeripheralBase;
}

STATIC BOOLEAN EFIAPI
Rp1BusIsFanReady (IN RP1_BUS_PROTOCOL *This)
{
  ASSERT (This != NULL);
  return RP1_BUS_DATA_FROM_THIS (This)->FanActive;
}

STATIC UINT32 EFIAPI
Rp1BusGetChipId (IN RP1_BUS_PROTOCOL *This)
{
  return RP1_BUS_DATA_FROM_THIS (This)->ChipId;
}

STATIC EFI_PHYSICAL_ADDRESS EFIAPI
Rp1BusGetSramBase (IN RP1_BUS_PROTOCOL *This)
{
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *Desc;
  EFI_PCI_IO_PROTOCOL *PciIo;
  EFI_PHYSICAL_ADDRESS Base;
  EFI_STATUS Status;

  PciIo = RP1_BUS_DATA_FROM_THIS (This)->PciIo;
  Desc = NULL;
  Base = 0;
  Status = PciIo->GetBarAttributes (PciIo, 2, NULL, (VOID **)&Desc);
  if (!EFI_ERROR (Status) && (Desc != NULL) &&
      (Desc->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR) &&
      (Desc->Len == sizeof (*Desc) - 3) &&
      (Desc->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM) &&
      (Desc->AddrLen >= SIZE_64KB) && (Desc->AddrRangeMin != 0) &&
      ((Desc->AddrRangeMin & (SIZE_64KB - 1)) == 0) &&
      (Desc->AddrRangeMin <= MAX_UINT64 - SIZE_64KB)) {
    Base = Desc->AddrRangeMin;
  }
  if (Desc != NULL) {
    FreePool (Desc);
  }
  return Base;
}

STATIC EFI_STATUS
Rp1BusReleaseController (RP1_BUS_DATA *Data)
{
  EFI_STATUS Status;
  RP1_BUS_DATA **Link;

  if (Data->BusInstalled) {
    Status = gBS->UninstallMultipleProtocolInterfaces (
      Data->ControllerHandle, &gRp1BusProtocolGuid, &Data->Rp1Bus,
      &gRp1IoProtocolGuid, &Data->Rp1Io, NULL);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Data->BusInstalled = FALSE;
  }
  // Restore MMIO state while PCI memory decoding and driver ownership remain
  // valid. Never repeat MMIO once the original PCI attributes are restored.
  if (Data->InterruptsSaved) {
    Rp1EthernetRestore (Data);
    Rp1FanRestore (Data);
    Rp1BusRestoreInterrupts (Data);
    Data->InterruptsSaved = FALSE;
  }
  if (Data->AttributesSaved) {
    Status = Data->PciIo->Attributes (Data->PciIo,
      EfiPciIoAttributeOperationSet, Data->OriginalAttributes, NULL);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Data->AttributesSaved = FALSE;
  }
  Status = gBS->CloseProtocol (Data->ControllerHandle, &gEfiPciIoProtocolGuid,
    Data->DriverBinding->DriverBindingHandle, Data->ControllerHandle);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  for (Link = &mRp1Controllers; *Link != Data; Link = &(*Link)->Next) {
    ASSERT (*Link != NULL);
  }
  *Link = Data->Next;
  FreePool (Data);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Rp1BusDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT32               PciId;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint32,
                        PCI_VENDOR_ID_OFFSET,
                        1,
                        &PciId
                        );

  if (EFI_ERROR (Status)) {
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }

  if (((PciId & 0xffff) != PCI_VENDOR_ID_RPILTD) ||
      ((PciId >> 16) != PCI_DEVICE_ID_RP1))
  {
    Status = EFI_UNSUPPORTED;
  }

Exit:
  gBS->CloseProtocol (
         ControllerHandle,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  return Status;
}

EFI_STATUS
EFIAPI
Rp1BusDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                         Status;
  EFI_PCI_IO_PROTOCOL                *PciIo;
  UINT64                             Supports;
  RP1_BUS_DATA                       *Rp1Data;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *PeripheralDesc;
  UINTN                              Index;
  EFI_STATUS                         CleanupStatus;

  PeripheralDesc = NULL;

  if (Rp1BusFindController (This, ControllerHandle) != NULL) {
    return EFI_ALREADY_STARTED;
  }
  // Allocate before opening PCI so an allocation failure cannot strand an
  // open protocol with no context available for cleanup.
  Rp1Data = AllocateZeroPool (sizeof (RP1_BUS_DATA));
  if (Rp1Data == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Rp1Data);
    return Status;
  }

  Rp1Data->Signature = RP1_BUS_DATA_SIGNATURE;
  Rp1Data->ControllerHandle = ControllerHandle;
  Rp1Data->DriverBinding = This;
  Rp1Data->PciIo = PciIo;
  Rp1Data->Next = mRp1Controllers;
  mRp1Controllers = Rp1Data;
  Rp1Data->Rp1Bus.GetPeripheralBase = Rp1BusGetPeripheralBase;
  Rp1Data->Rp1Bus.IsFanReady = Rp1BusIsFanReady;
  Rp1Data->Rp1Bus.GetSramBase = Rp1BusGetSramBase;
  Rp1Data->Rp1Bus.GetChipId = Rp1BusGetChipId;
  Rp1IoInitialize (Rp1Data);

  Status = PciIo->Attributes (PciIo, EfiPciIoAttributeOperationGet, 0,
                             &Rp1Data->OriginalAttributes);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }
  Rp1Data->AttributesSaved = TRUE;

  Status = PciIo->Attributes (
                    PciIo,
                    EfiPciIoAttributeOperationSupported,
                    0,
                    &Supports
                    );
  if (!EFI_ERROR (Status)) {
    if ((Supports & (EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)) !=
        (EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)) {
      Status = EFI_UNSUPPORTED;
      goto Fail;
    }
    Supports &= (UINT64)EFI_PCI_DEVICE_ENABLE;
    Status    = PciIo->Attributes (
                         PciIo,
                         EfiPciIoAttributeOperationEnable,
                         Supports,
                         NULL
                         );
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RP1: Failed to enable PCI device. Status=%r\n", Status));
    goto Fail;
  }

  Status = PciIo->GetBarAttributes (
                    PciIo,
                    RP1_PERIPHERAL_BAR_INDEX,
                    NULL,
                    (VOID **)&PeripheralDesc
                    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RP1: Failed to get BAR attributes. Status=%r\n", Status));
    goto Fail;
  }

  // All MMIO below must fit BAR1. Do not touch a malformed or truncated BAR.
  if ((PeripheralDesc == NULL) ||
      (PeripheralDesc->Desc != ACPI_ADDRESS_SPACE_DESCRIPTOR) ||
      (PeripheralDesc->Len != sizeof (*PeripheralDesc) - 3) ||
      (PeripheralDesc->ResType != ACPI_ADDRESS_SPACE_TYPE_MEM) ||
      (PeripheralDesc->AddrLen < RP1_USBHOST1_BASE + RP1_USBHOST_SIZE) ||
      (PeripheralDesc->AddrRangeMin == 0) ||
      ((PeripheralDesc->AddrRangeMin & 3) != 0) ||
      (PeripheralDesc->AddrRangeMin > MAX_UINTN - PeripheralDesc->AddrLen)) {
    Status = EFI_UNSUPPORTED;
    goto Fail;
  }

  Rp1Data->PeripheralBase = PeripheralDesc->AddrRangeMin;
  FreePool (PeripheralDesc);
  PeripheralDesc = NULL;

  Rp1Data->ChipId = MmioRead32 (Rp1Data->PeripheralBase + RP1_SYSINFO_BASE);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ControllerHandle,
                  &gRp1BusProtocolGuid,
                  &Rp1Data->Rp1Bus,
                  &gRp1IoProtocolGuid,
                  &Rp1Data->Rp1Io,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RP1: Failed to install bus protocol. Status=%r\n", Status));
    goto Fail;
  }
  Rp1Data->BusInstalled = TRUE;

  DEBUG ((
    DEBUG_INFO,
    "RP1: chip id %x, peripheral base at CPU address 0x%lx\n",
    Rp1Data->ChipId,
    Rp1Data->PeripheralBase
    ));

  Rp1BusEnableInterrupts (Rp1Data);
  Rp1Data->InterruptsSaved = TRUE;
  Status = Rp1BusRegisterDevices (Rp1Data);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Rp1FanStart (Rp1Data);
  Rp1EthernetStart (Rp1Data);
  return EFI_SUCCESS;

Fail:
  if (PeripheralDesc != NULL) {
    FreePool (PeripheralDesc);
  }
  if (Rp1Data->BusInstalled) {
    for (Index = 0; Index < ARRAY_SIZE (Rp1Data->UsbChildren); Index++) {
      if (Rp1Data->UsbChildren[Index] != NULL) {
        CleanupStatus = Rp1BusUnregisterNonDiscoverableDevice (Rp1Data, Index);
        if (EFI_ERROR (CleanupStatus)) {
          // A consumer still owns a published child. Keep the parent alive;
          // DriverBinding.Stop can finish the teardown after it disconnects.
          DEBUG ((DEBUG_ERROR, "RP1: Partial start (%r), child cleanup blocked (%r)\n", Status, CleanupStatus));
          return EFI_SUCCESS;
        }
      }
    }
  }
  CleanupStatus = Rp1BusReleaseController (Rp1Data);
  if (EFI_ERROR (CleanupStatus)) {
    // PCI ownership remains held. Report a partial start so the driver model
    // can call Stop again; no public interface points to freed storage.
    DEBUG ((DEBUG_ERROR, "RP1: Partial start (%r), cleanup pending (%r)\n", Status, CleanupStatus));
    return EFI_SUCCESS;
  }

  return Status;
}

STATIC
EFI_STATUS
Rp1BusUnregisterNonDiscoverableDevice (
  IN RP1_BUS_DATA *Rp1Data,
  IN UINTN Index
  )
{
  EFI_STATUS                Status;
  NON_DISCOVERABLE_DEVICE   *NonDiscoverableDevice;
  EFI_DEVICE_PATH_PROTOCOL  *NonDiscoverableDevicePath;
  RP1_BUS_PROTOCOL          *Rp1Bus;
  EFI_HANDLE                DeviceHandle;
  BOOLEAN                   WasOpened;

  DeviceHandle = Rp1Data->UsbChildren[Index];
  WasOpened = Rp1Data->UsbChildOpened[Index];

  Status = gBS->OpenProtocol (
                  DeviceHandle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  (VOID **)&NonDiscoverableDevice,
                  Rp1Data->DriverBinding->DriverBindingHandle,
                  Rp1Data->ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    ASSERT_EFI_ERROR (Status);
    return Status;
  }

  Status = gBS->OpenProtocol (
                  DeviceHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&NonDiscoverableDevicePath,
                  Rp1Data->DriverBinding->DriverBindingHandle,
                  Rp1Data->ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    ASSERT_EFI_ERROR (Status);
    return Status;
  }

  if (WasOpened) {
    Status = gBS->CloseProtocol (
                  Rp1Data->ControllerHandle,
                  &gRp1BusProtocolGuid,
                  Rp1Data->DriverBinding->DriverBindingHandle,
                  DeviceHandle
                  );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Rp1Data->UsbChildOpened[Index] = FALSE;
  }

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  DeviceHandle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  NonDiscoverableDevice,
                  &gEfiDevicePathProtocolGuid,
                  NonDiscoverableDevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    if (WasOpened) {
      Rp1Data->UsbChildOpened[Index] = !EFI_ERROR (gBS->OpenProtocol (
           Rp1Data->ControllerHandle,
           &gRp1BusProtocolGuid,
           (VOID **)&Rp1Bus,
           Rp1Data->DriverBinding->DriverBindingHandle,
           DeviceHandle,
           EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
           ));
    }
    return Status;
  }

  FreePool (NonDiscoverableDevice);
  FreePool (NonDiscoverableDevicePath);
  Rp1Data->UsbChildren[Index] = NULL;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Rp1BusDriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *DeviceHandleBuffer
  )
{
  EFI_STATUS        Status;
  UINTN             Index;
  BOOLEAN           AllChildrenStopped;
  RP1_BUS_DATA      *Rp1Data;
  UINTN             ChildIndex;

  if ((NumberOfChildren != 0) && (DeviceHandleBuffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Rp1Data = Rp1BusFindController (This, ControllerHandle);
  if (Rp1Data == NULL) {
    return EFI_NOT_FOUND;
  }

  if (NumberOfChildren == 0) {
    DEBUG ((DEBUG_INFO, "RP1: Stop bus at %p\n", ControllerHandle));

    for (Index = 0; Index < ARRAY_SIZE (Rp1Data->UsbChildren); Index++) {
      if (Rp1Data->UsbChildren[Index] != NULL) {
        return EFI_DEVICE_ERROR;
      }
    }

    return Rp1BusReleaseController (Rp1Data);
  }

  AllChildrenStopped = TRUE;

  for (Index = 0; Index < NumberOfChildren; Index++) {
    Status = EFI_NOT_FOUND;
    for (ChildIndex = 0; ChildIndex < ARRAY_SIZE (Rp1Data->UsbChildren); ChildIndex++) {
      if ((DeviceHandleBuffer[Index] != NULL) &&
          (DeviceHandleBuffer[Index] == Rp1Data->UsbChildren[ChildIndex])) {
        Status = Rp1BusUnregisterNonDiscoverableDevice (Rp1Data, ChildIndex);
        break;
      }
    }
    if (EFI_ERROR (Status)) {
      AllChildrenStopped = FALSE;
      continue;
    }
  }

  if (!AllChildrenStopped) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_DRIVER_BINDING_PROTOCOL  mRp1BusDriverBinding = {
  Rp1BusDriverBindingSupported,
  Rp1BusDriverBindingStart,
  Rp1BusDriverBindingStop,
  0x10,
  NULL,
  NULL
};

EFI_STATUS
EFIAPI
Rp1BusDxeEntryPoint (
  IN  EFI_HANDLE        ImageHandle,
  IN  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &mRp1BusDriverBinding,
           ImageHandle,
           &mRp1BusComponentName,
           &mRp1BusComponentName2
           );
}
