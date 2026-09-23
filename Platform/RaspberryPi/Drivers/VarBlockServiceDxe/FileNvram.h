/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef RPI_FILE_NVRAM_H
#define RPI_FILE_NVRAM_H
VOID FileNvramInitialize (VOID);
VOID FileNvramChanged (VOID);
VOID FileNvramVirtualAddressChange (VOID);
EFI_STATUS FileNvramSave (VOID);
#endif
