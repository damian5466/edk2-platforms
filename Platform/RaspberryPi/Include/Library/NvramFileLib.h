/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef RPI_NVRAM_FILE_LIB_H
#define RPI_NVRAM_FILE_LIB_H
/* On-disk/transport ABI for runtime variable persistence. Little-endian. */
#define NV_FILE_HEADER_SIZE 4096U
#define NV_FILE_DATA_SIZE 0x20000U
#define NV_FILE_SIZE (NV_FILE_HEADER_SIZE + NV_FILE_DATA_SIZE)
#define NV_FILE_LOAD_ADDRESS 0x04000000ULL
#define NV_FILE_VERSION 1U
#define NV_FILE_PREFIX_FLAG 1U
#define NV_FILE_PATH_SIZE 140U
#define NV_FILE_GUID {0x6ece02d9,0x9314,0x4a74,{0x84,0xdc,0x8f,0x67,0x93,0xbc,0xc6,0x51}}
#define NV_FILE_META_NAME L"RpiNvramFileInfo"
#define NV_FILE_SNAPSHOT_NAME L"RpiNvramFileSnapshot"
#pragma pack(push, 1)
typedef struct {
  unsigned char Magic[8];
  unsigned int Version, HeaderSize, DataSize, Flags;
  unsigned long long Sequence;
  unsigned char StoreId[16];
  unsigned int DataCrc, HeaderCrc, FirmwareSize, FirmwareOffset;
  char FirmwarePath[128];
  /* Optional relative directory/filename prefix, matching bootloader os_prefix.
   * Older readers reject NV_FILE_PREFIX_FLAG. Empty/flags=0 retains root files. */
  char NvramPrefix[128];
  unsigned char Reserved[NV_FILE_HEADER_SIZE - 320];
} NV_FILE_HEADER;
typedef struct {
  NV_FILE_HEADER Header;
  unsigned char Data[NV_FILE_DATA_SIZE];
} NV_FILE_IMAGE;
typedef struct {
  unsigned int Size, Version, Active, Reserved;
  unsigned long long Sequence, BootSequence;
  unsigned char StoreId[16];
} NV_FILE_INFO;
#pragma pack(pop)
unsigned int NvFileCrc (const void *Data, unsigned int Size);
int NvFileHeaderValid (const NV_FILE_HEADER *Header);
int NvFileVolumeValid (const void *Data, unsigned int Size);
int NvFileValid (const void *Image, unsigned int Size);
int NvFileSameStore (const NV_FILE_HEADER *A, const NV_FILE_HEADER *B);
int NvFileName (const NV_FILE_HEADER *Header, unsigned int Slot, char Path[NV_FILE_PATH_SIZE]);
/* -1: no valid copy or ambiguous stores. Otherwise index of newest valid copy. */
int NvFileSelect (const NV_FILE_IMAGE *A, const NV_FILE_IMAGE *B);
void NvFileSeal (NV_FILE_IMAGE *Image);
#endif
