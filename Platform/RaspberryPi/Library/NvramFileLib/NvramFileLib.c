/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/NvramFileLib.h>
typedef char NvHeaderSizeCheck[sizeof (NV_FILE_HEADER) == NV_FILE_HEADER_SIZE ? 1 : -1];
typedef char NvImageSizeCheck[sizeof (NV_FILE_IMAGE) == NV_FILE_SIZE ? 1 : -1];

static unsigned int Crc (const void *Data, unsigned int Size, int Header) {
  const unsigned char *Bytes = Data;
  unsigned int Value = ~0U, I, Bit;
  for (I = 0; I < Size; I++) {
    Value ^= Header && I >= 52 && I < 56 ? 0 : Bytes[I];
    for (Bit = 0; Bit < 8; Bit++) Value = (Value >> 1) ^ ((0U - (Value & 1U)) & 0xedb88320U);
  }
  return ~Value;
}
unsigned int NvFileCrc (const void *Data, unsigned int Size) { return Crc (Data, Size, 0); }
static int Equal (const void *A, const void *B, unsigned int Size) {
  const unsigned char *X = A, *Y = B;
  unsigned int I;
  for (I = 0; I < Size; I++) if (X[I] != Y[I]) return 0;
  return 1;
}
static unsigned int Read32 (const unsigned char *P) {
  return (unsigned int)P[0] | ((unsigned int)P[1] << 8) |
         ((unsigned int)P[2] << 16) | ((unsigned int)P[3] << 24);
}
int NvFileHeaderValid (const NV_FILE_HEADER *H) {
  unsigned int I, Nonzero = 0, Component = 0, End = 128;
  if (!H || !Equal (H->Magic, "RPINV001", 8) || H->Version != NV_FILE_VERSION ||
      H->HeaderSize != NV_FILE_HEADER_SIZE || H->DataSize != NV_FILE_DATA_SIZE ||
      (H->Flags & ~NV_FILE_PREFIX_FLAG) || !H->Sequence || H->Sequence == ~0ULL ||
      H->FirmwareSize > 0x1000000U || H->FirmwareOffset < 0x1000 ||
      (H->FirmwareOffset & 4095) || H->FirmwareOffset > H->FirmwareSize ||
      H->FirmwareSize - H->FirmwareOffset != NV_FILE_DATA_SIZE ||
      Crc (H, NV_FILE_HEADER_SIZE, 1) != H->HeaderCrc) return 0;
  for (I = 0; I < 16; I++) Nonzero |= H->StoreId[I];
  if (!Nonzero) return 0;
  for (I = 0; I < sizeof H->Reserved; I++) if (H->Reserved[I]) return 0;
  /* Relative ASCII path on this volume only; no traversal or device paths. */
  for (I = 0; I < sizeof H->FirmwarePath; I++) {
    unsigned char C = (unsigned char)H->FirmwarePath[I];
    if (!C) { End = I; break; }
    if (C < 0x20 || C > 0x7e || C == ':' || C == '*' || C == '?' || C == '"' || C == '<' || C == '>' || C == '|') return 0;
    if (C == '/' || C == '\\') {
      if (I == Component || (I - Component == 1 && H->FirmwarePath[Component] == '.') ||
          (I - Component == 2 && H->FirmwarePath[Component] == '.' && H->FirmwarePath[Component+1] == '.')) return 0;
      Component = I + 1;
    }
  }
  if (End == 128 || End - Component < 4) return 0;
  if (H->FirmwarePath[End-3] != '.' || (H->FirmwarePath[End-2] | 32) != 'f' ||
      (H->FirmwarePath[End-1] | 32) != 'd') return 0;
  for (I = End; I < 128; I++) if (H->FirmwarePath[I]) return 0;
  Component = 0; End = 128;
  for (I = 0; I < 128; I++) {
    unsigned char C = (unsigned char)H->NvramPrefix[I];
    if (!C) { End = I; break; }
    if (C < 0x20 || C > 0x7e || C == ':' || C == '*' || C == '?' || C == '"' || C == '<' || C == '>' || C == '|') return 0;
    if (C == '/' || C == '\\') {
      if (I == Component || (I - Component == 1 && H->NvramPrefix[Component] == '.') ||
          (I - Component == 2 && H->NvramPrefix[Component] == '.' && H->NvramPrefix[Component+1] == '.')) return 0;
      Component = I + 1;
    }
  }
  if (End == 128 || (End - Component == 1 && H->NvramPrefix[Component] == '.') ||
      (End - Component == 2 && H->NvramPrefix[Component] == '.' && H->NvramPrefix[Component+1] == '.')) return 0;
  if (!!End != !!(H->Flags & NV_FILE_PREFIX_FLAG)) return 0;
  for (I = End; I < 128; I++) if (H->NvramPrefix[I]) return 0;
  return 1;
}
int NvFileVolumeValid (const void *Data, unsigned int Size) {
  const unsigned char *P = Data;
  const unsigned char FvGuid[16] = {0x8d,0x2b,0xf1,0xff,0x96,0x76,0x8b,0x4c,0xa9,0x85,0x27,0x47,0x07,0x5b,0x4f,0x50};
  unsigned int I, Sum = 0;
  if (!P || Size != NV_FILE_DATA_SIZE || !Equal (P + 16, FvGuid, 16) ||
      Read32 (P+32) != NV_FILE_DATA_SIZE || Read32 (P+36) ||
      Read32 (P+40) != 0x4856465f || P[48] != 0x48 || P[49] ||
      P[52] || P[53] || P[54] || P[55] != 2 ||
      Read32 (P+56) != 32 || Read32 (P+60) != 4096 || Read32 (P+64) || Read32 (P+68)) return 0;
  for (I = 0; I < 16; I++) if (P[I]) return 0;
  for (I = 0; I < 0x48; I += 2) Sum += P[I] | ((unsigned int)P[I+1] << 8);
  return (Sum & 0xffffU) == 0;
}
int NvFileValid (const void *Image, unsigned int Size) {
  const NV_FILE_IMAGE *I = Image;
  return I && Size == NV_FILE_SIZE && NvFileHeaderValid (&I->Header) &&
    NvFileVolumeValid (I->Data, NV_FILE_DATA_SIZE) &&
    NvFileCrc (I->Data, NV_FILE_DATA_SIZE) == I->Header.DataCrc;
}
int NvFileSameStore (const NV_FILE_HEADER *A, const NV_FILE_HEADER *B) {
  return Equal (A->StoreId, B->StoreId, 16) && A->FirmwareSize == B->FirmwareSize &&
    A->FirmwareOffset == B->FirmwareOffset && Equal (A->FirmwarePath, B->FirmwarePath, 128) &&
    A->Flags == B->Flags && Equal (A->NvramPrefix, B->NvramPrefix, 128);
}
int NvFileName (const NV_FILE_HEADER *H, unsigned int Slot, char Path[NV_FILE_PATH_SIZE]) {
  const char *Name = Slot ? "RPI_NV1.bin" : "RPI_NV0.bin";
  unsigned int I = 0, J = 0;
  if (Slot > 1 || !NvFileHeaderValid (H)) return 0;
  while (H->NvramPrefix[I]) { Path[I] = H->NvramPrefix[I]; I++; }
  do { Path[I++] = Name[J]; } while (Name[J++]);
  return 1;
}
int NvFileSelect (const NV_FILE_IMAGE *A, const NV_FILE_IMAGE *B) {
  int Av = NvFileValid (A, NV_FILE_SIZE), Bv = NvFileValid (B, NV_FILE_SIZE);
  if (!Av && !Bv) return -1;
  if (!Av) return 1;
  if (!Bv) return 0;
  if (!NvFileSameStore (&A->Header, &B->Header)) return -1;
  /* Equal sequence with different content is not an ordering we can resolve. */
  if (A->Header.Sequence == B->Header.Sequence && !Equal (A, B, NV_FILE_SIZE)) return -1;
  return B->Header.Sequence > A->Header.Sequence ? 1 : 0;
}
void NvFileSeal (NV_FILE_IMAGE *Image) {
  Image->Header.DataCrc = NvFileCrc (Image->Data, NV_FILE_DATA_SIZE);
  Image->Header.HeaderCrc = Crc (&Image->Header, NV_FILE_HEADER_SIZE, 1);
}
