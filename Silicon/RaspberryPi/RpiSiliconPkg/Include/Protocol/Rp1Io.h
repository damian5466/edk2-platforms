/** @file
  RP1 boot-services GPIO and PWM interface. Calls require TPL <= TPL_NOTIFY.

  No pins or clocks are changed merely by installing this protocol. Consumers
  own the pins/channels they configure and must coordinate with other drivers.
  Interrupt GPIO, capture, DMA and runtime access are not implemented.
  When the board fan policy is active, writes to GPIO45 and PWM1 return
  EFI_ACCESS_DENIED. PWM0 and read-only queries remain available.
  The Ethernet board policy similarly reserves GPIO32 while active.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef RP1_IO_PROTOCOL_H_
#define RP1_IO_PROTOCOL_H_

#include <Uefi.h>

#define RP1_IO_PROTOCOL_GUID \
  { 0xa177e77a, 0x4b9a, 0x487e, { 0xa6, 0xa1, 0x53, 0xea, 0xf6, 0x04, 0x23, 0xb8 } }

#define RP1_GPIO_COUNT       54
#define RP1_GPIO_FUNCTION    5
#define RP1_GPIO_DISABLED    31
#define RP1_GPIO_PULL_NONE   0
#define RP1_GPIO_PULL_DOWN   1
#define RP1_GPIO_PULL_UP     2

typedef struct _RP1_IO_PROTOCOL RP1_IO_PROTOCOL;

typedef struct {
  UINT32   Function;  // Hardware function selector 0..8 or RP1_GPIO_DISABLED.
  UINT32   Pull;
  BOOLEAN  Output;    // RIO direction; relevant when Function is GPIO.
  BOOLEAN  Value;     // RIO output latch, not the sampled input level.
} RP1_GPIO_CONFIG;

typedef EFI_STATUS (EFIAPI *RP1_GPIO_GET_CONFIG)(
  IN RP1_IO_PROTOCOL *This, IN UINTN Pin, OUT RP1_GPIO_CONFIG *Config
  );
typedef EFI_STATUS (EFIAPI *RP1_GPIO_CONFIGURE)(
  IN RP1_IO_PROTOCOL *This, IN UINTN Pin, IN CONST RP1_GPIO_CONFIG *Config
  );
typedef EFI_STATUS (EFIAPI *RP1_GPIO_READ)(
  IN RP1_IO_PROTOCOL *This, IN UINTN Pin, OUT BOOLEAN *Value
  );
typedef EFI_STATUS (EFIAPI *RP1_GPIO_WRITE)(
  IN RP1_IO_PROTOCOL *This, IN UINTN Pin, IN BOOLEAN Value
  );
/**
  Configure one of four channels of PWM controller 0 or 1. Period and duty are
  nanoseconds, rounded to the nearest 20 ns. An enabled channel needs a nonzero
  representable period and DutyNs <= PeriodNs. When disabling, timing is ignored.
  The clock is 50 MHz XOSC / 1. An incompatible clock already driving active
  channels is left untouched and EFI_UNSUPPORTED is returned. Pin mux is separate.
**/
typedef EFI_STATUS (EFIAPI *RP1_PWM_CONFIGURE)(
  IN RP1_IO_PROTOCOL *This, IN UINTN Controller, IN UINTN Channel,
  IN UINT64 PeriodNs, IN UINT64 DutyNs, IN BOOLEAN Inverted, IN BOOLEAN Enable
  );

struct _RP1_IO_PROTOCOL {
  UINT64                Revision;
  RP1_GPIO_GET_CONFIG   GetGpioConfig;
  RP1_GPIO_CONFIGURE    ConfigureGpio;
  RP1_GPIO_READ         ReadGpio;
  RP1_GPIO_WRITE        WriteGpio;
  RP1_PWM_CONFIGURE     ConfigurePwm;
};

extern EFI_GUID gRp1IoProtocolGuid;
#endif
