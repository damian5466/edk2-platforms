/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiPei.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/FdtPlatformLib.h>
#include <Library/HobLib.h>
#include <Library/NvramFileLib.h>
#include <Library/PcdLib.h>

STATIC UINT64 InitrdAddress (CONST VOID *Fdt, INT32 Chosen, CONST CHAR8 *Name) {
  INT32 Size;
  CONST UINT8 *Value = FdtGetProp (Fdt, Chosen, Name, &Size);
  UINT64 Address = 0;
  INT32 I;
  if (!Value || (Size != 4 && Size != 8)) return 0;
  for (I = 0; I < Size; I++) Address = (Address << 8) | Value[I];
  return Address;
}

/* Called in SEC before DXE or variable services allocate/cache the store.
 * The bootloader loads both fixed-size files as one initramfs blob. The low
 * 64 MiB staging address is outside both the FD and SEC's top-of-RAM arena.
 * Consume it now; no permanent extra memory reservation is necessary. */
VOID NvramFileEarlyLoad (VOID) {
  CONST VOID *Fdt;
  CONST NV_FILE_IMAGE *Slots;
  INT32 Chosen;
  INTN Selected;
  if (!FeaturePcdGet (PcdNvramFileEnable)) return;
  Fdt = FdtPlatformGetBase ();
  if (!Fdt) return;
  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0 || InitrdAddress (Fdt, Chosen, "linux,initrd-start") != NV_FILE_LOAD_ADDRESS ||
      InitrdAddress (Fdt, Chosen, "linux,initrd-end") != NV_FILE_LOAD_ADDRESS + 2 * NV_FILE_SIZE)
    return;
  Slots = (CONST NV_FILE_IMAGE *)(UINTN)NV_FILE_LOAD_ADDRESS;
  Selected = NvFileSelect (&Slots[0], &Slots[1]);
  if (Selected < 0 || Slots[Selected].Header.FirmwareSize != FixedPcdGet32 (PcdFdSize) ||
      Slots[Selected].Header.FirmwareOffset != FixedPcdGet32 (PcdNvStorageVariableBase)) return;
  /* Publish the identity first, so an allocation failure cannot load a store
   * whose persistence bridge would be inactive. */
  if (!BuildGuidDataHob (&gRpiNvramFileGuid, (VOID *)&Slots[Selected].Header, sizeof (NV_FILE_HEADER))) return;
  CopyMem ((VOID *)(UINTN)FixedPcdGet32 (PcdNvStorageVariableBase),
           Slots[Selected].Data, NV_FILE_DATA_SIZE);
}
