/** @file
 * One-way OS ownership transfer of the VideoCore mailbox.
 * Keep flags on separate cache lines; AML writes Request, firmware writes
 * Active/Fault. Firmware must publish Active before sampling Request.
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 **/
#ifndef RPI_MAILBOX_HANDOFF_H
#define RPI_MAILBOX_HANDOFF_H

typedef struct {
  volatile UINT32 Request;
  UINT8 Reserved0[60];
  volatile UINT32 Active;
  UINT8 Reserved1[60];
  volatile UINT32 Fault;
} RPI_MAILBOX_HANDOFF;

STATIC_ASSERT (OFFSET_OF (RPI_MAILBOX_HANDOFF, Active) == 64, "AML Active offset");
STATIC_ASSERT (OFFSET_OF (RPI_MAILBOX_HANDOFF, Fault) == 128, "AML Fault offset");
#endif
