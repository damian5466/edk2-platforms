/** @file
 *
 *  Copyright (c) 2024, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#ifndef __RP1_BUS_DXE_H__
#define __RP1_BUS_DXE_H__

#include <Protocol/DriverBinding.h>
#include <Protocol/PciIo.h>
#include <Protocol/Rp1Bus.h>
#include <Protocol/Rp1Io.h>
#include <Protocol/ComponentName.h>
#include <Protocol/ComponentName2.h>

#define RP1_BUS_DATA_SIGNATURE  SIGNATURE_32 ('R','P','1','b')

typedef struct RP1_BUS_DATA {
  UINT32                         Signature;
  EFI_HANDLE                     ControllerHandle;
  EFI_DRIVER_BINDING_PROTOCOL    *DriverBinding;
  EFI_PCI_IO_PROTOCOL            *PciIo;
  RP1_BUS_PROTOCOL               Rp1Bus;
  RP1_IO_PROTOCOL                Rp1Io;
  EFI_PHYSICAL_ADDRESS           PeripheralBase;
  UINT32                         ChipId;
  UINT64                         OriginalAttributes;
  EFI_HANDLE                     UsbChildren[2];
  BOOLEAN                        UsbChildOpened[2];
  UINT32                         UsbInterruptEnable[2];
  BOOLEAN                        FanActive;
  BOOLEAN                        FanPinChanged;
  UINT32                         FanRegisters[12];
  BOOLEAN                        EthernetActive;
  UINT32                         EthernetRegisters[10];
  struct RP1_BUS_DATA            *Next;
  BOOLEAN                        BusInstalled;
  BOOLEAN                        AttributesSaved;
  BOOLEAN                        InterruptsSaved;
} RP1_BUS_DATA;

#define RP1_BUS_DATA_FROM_THIS(a)  (CR (a, RP1_BUS_DATA, Rp1Bus, RP1_BUS_DATA_SIGNATURE))
#define RP1_BUS_DATA_FROM_IO(a)  (CR (a, RP1_BUS_DATA, Rp1Io, RP1_BUS_DATA_SIGNATURE))

VOID Rp1IoInitialize (IN RP1_BUS_DATA *Rp1Data);
VOID Rp1FanStart (IN RP1_BUS_DATA *Rp1Data);
VOID Rp1FanRestore (IN RP1_BUS_DATA *Rp1Data);
VOID Rp1EthernetStart (IN RP1_BUS_DATA *Rp1Data);
VOID Rp1EthernetRestore (IN RP1_BUS_DATA *Rp1Data);

extern EFI_DRIVER_BINDING_PROTOCOL   mRp1BusDriverBinding;
extern EFI_COMPONENT_NAME_PROTOCOL   mRp1BusComponentName;
extern EFI_COMPONENT_NAME2_PROTOCOL  mRp1BusComponentName2;

#endif // __RP1_BUS_DXE_H__
