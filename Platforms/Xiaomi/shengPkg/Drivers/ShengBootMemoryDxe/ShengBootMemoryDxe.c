/** @file
  Memory and RAM disk from the device tree the previous stage handed over.

  The previous stage (a Linux-style boot loader) passes the physical address
  of a flattened device tree in x0, which SEC keeps in a HOB. Its /memory
  nodes list the real DRAM and its /reserved-memory nodes list what the
  secure side owns. Without the vendor memory manager the firmware only knows
  the few regions of its own memory map, so:

  * DRAM that no firmware region claims and no reserved-memory node covers is
    added to the GCD as system memory, up to a fixed amount;
  * the initrd the loader placed in memory (/chosen linux,initrd-start/-end)
    is first pinned as reserved memory and then published as a read-only block
    device, so a FAT image in the boot image's ramdisk shows up as a normal
    volume without the DXE allocator ever owning its pages.

  Nothing here depends on a hardware address; every range comes from the DT.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#define MAX_RANGES        64
#define MIN_PIECE_SIZE    SIZE_16MB
#define ADD_LIMIT         SIZE_1GB
#define ADD_BELOW         SIZE_4GB     // stay inside what the MMU setup maps
#define MAX_FDT_SIZE      SIZE_16MB
#define MAX_MAPPED_PIECES 8
//
// Shared with UniXNULib/Config.c. A fixed-width structure avoids depending on
// the variable driver while the menu is being configured.
//
#define SHENG_BOOT_MEMORY_STATUS_SIGNATURE  SIGNATURE_64 ('S', 'H', 'M', 'E', 'M', 'S', 'T', '1')

typedef struct {
  UINT64      Signature;
  UINT32      Version;
  UINT32      HobFound;
  UINT64      Dtb;
  EFI_STATUS  FdtStatus;
  EFI_STATUS  InitrdStatus;
  EFI_STATUS  MapStatus;
  EFI_STATUS  BlockStatus;
  UINT64      InitrdStart;
  UINT64      InitrdEnd;
  UINT64      MapBase;
  UINT64      MapEnd;
  UINT32      MapPieces;
} SHENG_BOOT_MEMORY_STATUS;

STATIC EFI_GUID  mStatusGuid = {
  0x8E8F1B43, 0x70A4, 0x4F8D, { 0x9B, 0x63, 0x24, 0x1E, 0xC6, 0x91, 0x52, 0xA7 }
};

STATIC
VOID
PublishStatus (
  IN UINT32      HobFound,
  IN UINT64      Dtb,
  IN EFI_STATUS  Fdt,
  IN EFI_STATUS  Initrd,
  IN EFI_STATUS  Map,
  IN EFI_STATUS  Block,
  IN UINT64      InitrdStart,
  IN UINT64      InitrdEnd,
  IN UINT64      MapBase,
  IN UINT64      MapEnd,
  IN UINT32      MapPieces
  )
{
  STATIC SHENG_BOOT_MEMORY_STATUS  Status;

  Status.Signature    = SHENG_BOOT_MEMORY_STATUS_SIGNATURE;
  Status.Version      = 1;
  Status.HobFound     = HobFound;
  Status.Dtb          = Dtb;
  Status.FdtStatus    = Fdt;
  Status.InitrdStatus = Initrd;
  Status.MapStatus    = Map;
  Status.BlockStatus  = Block;
  Status.InitrdStart  = InitrdStart;
  Status.InitrdEnd    = InitrdEnd;
  Status.MapBase      = MapBase;
  Status.MapEnd       = MapEnd;
  Status.MapPieces    = MapPieces;
  gBS->InstallConfigurationTable (&mStatusGuid, &Status);
}

typedef struct {
  UINT64  Base;
  UINT64  End;       // exclusive
} RANGE;

STATIC
EFI_STATUS
MapRangePiece (
  IN  EFI_PHYSICAL_ADDRESS  Base,
  IN  UINTN                 Length,
  IN  EFI_GCD_MEMORY_TYPE   GcdMemoryType,
  OUT BOOLEAN               *Added
  )
{
  EFI_STATUS  Status;

  *Added = FALSE;

  Status = gDS->AddMemorySpace (
                  GcdMemoryType,
                  Base,
                  Length,
                  EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: add mapping 0x%lx-0x%lx %r\n", Base, Base + Length - 1, Status));
    return Status;
  }

  *Added = TRUE;
  Status = gDS->SetMemorySpaceAttributes (Base, Length, EFI_MEMORY_WB);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: map attributes 0x%lx-0x%lx %r\n", Base, Base + Length - 1, Status));
    gDS->RemoveMemorySpace (Base, Length);
    *Added = FALSE;
  }

  return Status;
}

/**
  Temporarily map physical memory which the bootloader used for its device tree.

  SEC can read the bootloader's DTB while its identity map is active, but the
  DXE page tables only cover the platform's declared memory regions. The DTB
  usually sits in a DRAM hole that ShengBootMemoryDxe itself has not published
  yet, so it must be mapped before libfdt reads its header.
**/
STATIC
EFI_STATUS
MapFdtRange (
  IN  EFI_PHYSICAL_ADDRESS  Base,
  IN  UINTN                 Length,
  IN  EFI_GCD_MEMORY_TYPE   AddType,
  IN  EFI_GCD_MEMORY_TYPE   ExistingType,
  OUT EFI_PHYSICAL_ADDRESS  *MappedBase,
  OUT UINTN                 *MappedLength,
  OUT UINTN                 *MappedCount,
  OUT BOOLEAN               *Added
  )
{
  EFI_STATUS                       Status;
  EFI_STATUS                       PieceStatus;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  EFI_PHYSICAL_ADDRESS             Cursor;
  EFI_PHYSICAL_ADDRESS             End;
  EFI_PHYSICAL_ADDRESS             PieceEnd;
  UINTN                            Index;

  *MappedCount = 0;
  *Added       = FALSE;
  if (Length == 0) {
    return EFI_SUCCESS;
  }

  if (((Base & EFI_PAGE_MASK) != 0) || ((Length & EFI_PAGE_MASK) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  End    = Base + Length;
  Cursor = Base;
  Status = EFI_SUCCESS;
  while (Cursor < End) {
    PieceStatus = gDS->GetMemorySpaceDescriptor (Cursor, &Descriptor);
    if (EFI_ERROR (PieceStatus)) {
      Status = PieceStatus;
      break;
    }

    PieceEnd = MIN (End, Descriptor.BaseAddress + Descriptor.Length);
    if (PieceEnd <= Cursor) {
      Status = EFI_DEVICE_ERROR;
      break;
    }

    if (Descriptor.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
      PieceStatus = MapRangePiece (Cursor, (UINTN)(PieceEnd - Cursor), AddType, Added);
      if (EFI_ERROR (PieceStatus)) {
        Status = PieceStatus;
        break;
      }

      if (*Added) {
        if (*MappedCount >= MAX_MAPPED_PIECES) {
          gDS->RemoveMemorySpace (Cursor, (UINTN)(PieceEnd - Cursor));
          Status = EFI_OUT_OF_RESOURCES;
          break;
        }

        MappedBase[*MappedCount]   = Cursor;
        MappedLength[*MappedCount] = (UINTN)(PieceEnd - Cursor);
        (*MappedCount)++;
      }
    } else {
      //
      // An existing mapping is only acceptable when its owner and type match
      // the caller's contract. The DTB accepts any identity mapping; initrd
      // pages must already be unowned reserved RAM.
      //
      if (((ExistingType != EfiGcdMemoryTypeNonExistent) &&
           (Descriptor.GcdMemoryType != ExistingType)) ||
          ((ExistingType != EfiGcdMemoryTypeNonExistent) &&
           (Descriptor.ImageHandle != NULL)))
      {
        DEBUG ((
          DEBUG_ERROR,
          "SHENGMEM: range 0x%lx-0x%lx has incompatible GCD type %u owner %p\n",
          Cursor,
          PieceEnd - 1,
          Descriptor.GcdMemoryType,
          Descriptor.ImageHandle
          ));
        Status = EFI_ACCESS_DENIED;
        break;
      }

      if (ExistingType != EfiGcdMemoryTypeNonExistent) {
        PieceStatus = gDS->SetMemorySpaceAttributes (Cursor, (UINTN)(PieceEnd - Cursor), EFI_MEMORY_WB);
        if (EFI_ERROR (PieceStatus)) {
          DEBUG ((DEBUG_ERROR, "SHENGMEM: reuse mapping 0x%lx-0x%lx %r\n",
                  Cursor, PieceEnd - 1, PieceStatus));
          Status = PieceStatus;
          break;
        }
      }
    }

    Cursor = PieceEnd;
  }

  if (EFI_ERROR (Status)) {
    for (Index = 0; Index < *MappedCount; Index++) {
      gDS->RemoveMemorySpace (MappedBase[Index], MappedLength[Index]);
    }

    *MappedCount = 0;
    *Added       = FALSE;
  } else {
    *Added = *MappedCount != 0;
  }

  return Status;
}

STATIC
VOID
ReleaseMappedPieces (
  IN EFI_PHYSICAL_ADDRESS  *MappedBase,
  IN UINTN                 *MappedLength,
  IN OUT UINTN             *MappedCount
  )
{
  UINTN       Index;

  for (Index = 0; Index < *MappedCount; Index++) {
    if (EFI_ERROR (gDS->RemoveMemorySpace (MappedBase[Index], MappedLength[Index]))) {
      DEBUG ((DEBUG_ERROR, "SHENGMEM: failed to release mapping 0x%lx-0x%lx\n",
              MappedBase[Index], MappedBase[Index] + MappedLength[Index] - 1));
    }
  }

  *MappedCount = 0;
}

/**
  Temporarily map the whole bootloader FDT and validate it.

  Pool memory is not yet a safe place for the DTB: this driver runs before the
  platform has published the full DRAM map, so AllocatePool() can fail even for
  a small blob. Keep the FDT in place instead, map only its exact extent, and
  release that mapping after parsing.
**/
STATIC
EFI_STATUS
MapBootFdt (
  IN  UINT64                Dtb,
  OUT UINTN                 *FdtSize,
  OUT EFI_PHYSICAL_ADDRESS  *ProbeBase,
  OUT UINTN                 *ProbeSize,
  OUT EFI_PHYSICAL_ADDRESS  *ProbeMappedBase,
  OUT UINTN                 *ProbeMappedLength,
  OUT UINTN                 *ProbeMappedCount,
  OUT BOOLEAN               *ProbeAdded,
  OUT EFI_PHYSICAL_ADDRESS  *TailBase,
  OUT UINTN                 *TailSize,
  OUT EFI_PHYSICAL_ADDRESS  *TailMappedBase,
  OUT UINTN                 *TailMappedLength,
  OUT UINTN                 *TailMappedCount,
  OUT BOOLEAN               *TailAdded
  )
{
  EFI_STATUS  Status;
  EFI_PHYSICAL_ADDRESS  Base;
  EFI_PHYSICAL_ADDRESS  ProbeEnd;
  EFI_PHYSICAL_ADDRESS  FullEnd;
  UINTN       TotalSize;

  *FdtSize    = 0;
  *ProbeBase  = 0;
  *ProbeSize  = 0;
  *ProbeMappedCount = 0;
  *ProbeAdded = FALSE;
  *TailBase   = 0;
  *TailSize   = 0;
  *TailMappedCount = 0;
  *TailAdded  = FALSE;
  if ((Dtb == 0) || ((Dtb & 7) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Base      = Dtb & ~((UINT64)SIZE_4KB - 1);
  ProbeEnd  = ALIGN_VALUE (Dtb + SIZE_4KB, SIZE_4KB);
  *ProbeBase = Base;
  *ProbeSize = (UINTN)(ProbeEnd - Base);

  Status = MapFdtRange (
             Base,
             *ProbeSize,
             EfiGcdMemoryTypeMemoryMappedIo,
             EfiGcdMemoryTypeNonExistent,
             ProbeMappedBase,
             ProbeMappedLength,
             ProbeMappedCount,
             ProbeAdded
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (FdtCheckHeader ((VOID *)(UINTN)Dtb) != 0) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  TotalSize = (UINTN)FdtTotalSize ((VOID *)(UINTN)Dtb);
  if ((TotalSize < sizeof (UINT64)) || (TotalSize > MAX_FDT_SIZE)) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  FullEnd = ALIGN_VALUE (Dtb + TotalSize, SIZE_4KB);
  if (FullEnd > ProbeEnd) {
    *TailBase = ProbeEnd;
    *TailSize = (UINTN)(FullEnd - ProbeEnd);
    Status    = MapFdtRange (
                  *TailBase,
                  *TailSize,
                  EfiGcdMemoryTypeMemoryMappedIo,
                  EfiGcdMemoryTypeNonExistent,
                  TailMappedBase,
                  TailMappedLength,
                  TailMappedCount,
                  TailAdded
                  );
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  if (FdtCheckHeader ((VOID *)(UINTN)Dtb) != 0) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  *FdtSize = TotalSize;
  return EFI_SUCCESS;

Done:
  ReleaseMappedPieces (TailMappedBase, TailMappedLength, TailMappedCount);
  ReleaseMappedPieces (ProbeMappedBase, ProbeMappedLength, ProbeMappedCount);
  *FdtSize    = 0;
  *ProbeSize  = 0;
  *ProbeMappedCount = 0;
  *ProbeAdded = FALSE;
  *TailBase   = 0;
  *TailSize   = 0;
  *TailMappedCount = 0;
  *TailAdded  = FALSE;
  return Status;
}

STATIC
UINT64
ReadCells (
  IN CONST VOID  *Data,
  IN UINT32      Cells
  )
{
  if (Cells == 2) {
    return Fdt64ToCpu (ReadUnaligned64 (Data));
  }

  return Fdt32ToCpu (ReadUnaligned32 (Data));
}

STATIC
UINT32
GetCells (
  IN CONST VOID  *Fdt,
  IN INT32       Node,
  IN CONST CHAR8 *Name,
  IN UINT32      Default
  )
{
  CONST VOID  *Prop;
  INT32       Length;

  Prop = FdtGetProp (Fdt, Node, Name, &Length);
  if ((Prop == NULL) || (Length != sizeof (UINT32))) {
    return Default;
  }

  return Fdt32ToCpu (ReadUnaligned32 (Prop));
}

STATIC
UINTN
CollectReg (
  IN     CONST VOID  *Fdt,
  IN     INT32       Node,
  IN     UINT32      AddressCells,
  IN     UINT32      SizeCells,
  IN OUT RANGE       *Ranges,
  IN     UINTN       Count
  )
{
  CONST UINT8  *Reg;
  INT32        Length;
  UINT32       Entry;
  INT32        Offset;
  UINT64       Base;
  UINT64       Size;

  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if (Reg == NULL) {
    return Count;
  }

  Entry = (AddressCells + SizeCells) * sizeof (UINT32);
  for (Offset = 0; (Offset + (INT32)Entry <= Length) && (Count < MAX_RANGES); Offset += Entry) {
    Base = ReadCells (Reg + Offset, AddressCells);
    Size = ReadCells (Reg + Offset + AddressCells * sizeof (UINT32), SizeCells);
    if ((Size != 0) && (Base + Size > Base)) {
      Ranges[Count].Base = Base;
      Ranges[Count].End  = Base + Size;
      Count++;
    }
  }

  return Count;
}

/**
  Remove every part of Pieces[] that lies inside Cut.
**/
STATIC
UINTN
Subtract (
  IN OUT RANGE  *Pieces,
  IN     UINTN  Count,
  IN     RANGE  Cut
  )
{
  UINTN  Index;
  UINTN  Result;
  RANGE  Out[MAX_RANGES];

  Result = 0;
  for (Index = 0; Index < Count; Index++) {
    if ((Cut.End <= Pieces[Index].Base) || (Cut.Base >= Pieces[Index].End)) {
      if (Result < MAX_RANGES) {
        Out[Result++] = Pieces[Index];
      }

      continue;
    }

    if ((Cut.Base > Pieces[Index].Base) && (Result < MAX_RANGES)) {
      Out[Result].Base  = Pieces[Index].Base;
      Out[Result].End   = Cut.Base;
      Result++;
    }

    if ((Cut.End < Pieces[Index].End) && (Result < MAX_RANGES)) {
      Out[Result].Base  = Cut.End;
      Out[Result].End   = Pieces[Index].End;
      Result++;
    }
  }

  CopyMem (Pieces, Out, Result * sizeof (RANGE));
  return Result;
}

STATIC
VOID
AddMemory (
  IN CONST VOID  *Fdt,
  IN UINT64      InitrdBase,
  IN UINT64      InitrdEnd
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  *Map;
  UINTN                            MapCount;
  INT32                            Root;
  INT32                            Node;
  INT32                            Reserved;
  INT32                            Length;
  CONST CHAR8                      *Type;
  UINT32                           AddressCells;
  UINT32                           SizeCells;
  RANGE                            Banks[MAX_RANGES];
  RANGE                            Holes[MAX_RANGES];
  RANGE                            Pieces[MAX_RANGES];
  UINTN                            BankCount;
  UINTN                            HoleCount;
  UINTN                            PieceCount;
  UINTN                            ReservedCount;
  RANGE                            ReservedRanges[MAX_RANGES];
  UINTN                            Index;
  UINTN                            Other;
  UINT64                           Added;
  UINT64                           Base;
  UINT64                           End;
  RANGE                            Initrd;

  Root         = FdtPathOffset (Fdt, "/");
  AddressCells = GetCells (Fdt, Root, "#address-cells", 2);
  SizeCells    = GetCells (Fdt, Root, "#size-cells", 2);

  BankCount = 0;
  for (Node = FdtFirstSubnode (Fdt, Root); Node >= 0; Node = FdtNextSubnode (Fdt, Node)) {
    Type = FdtGetProp (Fdt, Node, "device_type", &Length);
    if ((Type != NULL) && (AsciiStrCmp (Type, "memory") == 0)) {
      BankCount = CollectReg (Fdt, Node, AddressCells, SizeCells, Banks, BankCount);
    }
  }

  ReservedCount = 0;
  Reserved      = FdtPathOffset (Fdt, "/reserved-memory");
  if (Reserved >= 0) {
    UINT32  ResAddress = GetCells (Fdt, Reserved, "#address-cells", AddressCells);
    UINT32  ResSize    = GetCells (Fdt, Reserved, "#size-cells", SizeCells);

    for (Node = FdtFirstSubnode (Fdt, Reserved); Node >= 0; Node = FdtNextSubnode (Fdt, Node)) {
      ReservedCount = CollectReg (Fdt, Node, ResAddress, ResSize, ReservedRanges, ReservedCount);
    }
  }

  DEBUG ((DEBUG_ERROR, "SHENGMEM: DT has %u memory ranges and %u reserved ranges\n", (UINT32)BankCount, (UINT32)ReservedCount));
  if (BankCount == 0) {
    return;
  }

  Status = gDS->GetMemorySpaceMap (&MapCount, &Map);
  if (EFI_ERROR (Status)) {
    return;
  }

  //
  // Holes: DRAM the GCD does not know yet.
  //
  HoleCount = 0;
  for (Index = 0; Index < BankCount; Index++) {
    for (Other = 0; Other < MapCount; Other++) {
      if (Map[Other].GcdMemoryType != EfiGcdMemoryTypeNonExistent) {
        continue;
      }

      Base = MAX (Banks[Index].Base, Map[Other].BaseAddress);
      End  = MIN (Banks[Index].End, Map[Other].BaseAddress + Map[Other].Length);
      if ((Base < End) && (HoleCount < MAX_RANGES)) {
        Holes[HoleCount].Base = Base;
        Holes[HoleCount].End  = End;
        HoleCount++;
      }
    }
  }

  FreePool (Map);

  //
  // Minus everything the DT reserves; what is left is added right away.
  //
  Added = 0;
  for (Index = 0; (Index < HoleCount) && (Added < ADD_LIMIT); Index++) {
    Pieces[0]  = Holes[Index];
    PieceCount = 1;
    for (Other = 0; Other < ReservedCount; Other++) {
      PieceCount = Subtract (Pieces, PieceCount, ReservedRanges[Other]);
    }

    //
    // The initrd is handled before general DRAM, but subtract it here as well
    // so a failed ramdisk setup can never hand its pages to the allocator.
    //
    if (InitrdEnd > InitrdBase) {
      Initrd.Base = InitrdBase;
      Initrd.End  = InitrdEnd;
      PieceCount  = Subtract (Pieces, PieceCount, Initrd);
    }

    for (Other = 0; (Other < PieceCount) && (Added < ADD_LIMIT); Other++) {
      Base = ALIGN_VALUE (Pieces[Other].Base, SIZE_2MB);
      End  = Pieces[Other].End & ~((UINT64)SIZE_2MB - 1);
      if ((End <= Base) || (End - Base < MIN_PIECE_SIZE) || (End > ADD_BELOW)) {
        continue;
      }

      End = MIN (End, Base + (ADD_LIMIT - Added));

      //
      // Add and map before anything can allocate from it.
      //
      Status = gDS->AddMemorySpace (
                      EfiGcdMemoryTypeSystemMemory,
                      Base,
                      End - Base,
                      EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB
                      );
      if (!EFI_ERROR (Status)) {
        Status = gDS->SetMemorySpaceAttributes (Base, End - Base, EFI_MEMORY_WB);
        if (EFI_ERROR (Status)) {
          gDS->RemoveMemorySpace (Base, End - Base);
        } else {
          Added += End - Base;
        }
      }

      DEBUG ((DEBUG_ERROR, "SHENGMEM: system memory 0x%lx-0x%lx %r\n", Base, End - 1, Status));
    }
  }

  DEBUG ((DEBUG_ERROR, "SHENGMEM: added 0x%lx bytes of system memory\n", Added));
}

#define RAM_BLOCK_SIZE  512

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0xA9C6827C, 0x1BD8, 0x410C, { 0x88, 0x5A, 0xC4, 0x65, 0x00, 0xF7, 0xD8, 0x36 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8                    *mRamBase;
STATIC EFI_BLOCK_IO_MEDIA       mMedia;
STATIC BOOLEAN                  mFirstRead;

STATIC
VOID
LogFatHeader (
  IN CONST UINT8  *Sector
  )
{
  UINT16  Signature;

  CopyMem (&Signature, Sector + 510, sizeof (Signature));
  DEBUG ((
    DEBUG_ERROR,
    "SHENGMEM: fat boot sig=%04x bytes/sector=%u sectors/cluster=%u reserved=%u fats=%u root=%u total16=%u total32=%u\n",
    Signature,
    Sector[11] | (Sector[12] << 8),
    Sector[13],
    Sector[14] | (Sector[15] << 8),
    Sector[16],
    Sector[17] | (Sector[18] << 8),
    Sector[19] | (Sector[20] << 8),
    Sector[32] | (Sector[33] << 8) | (Sector[34] << 16) | ((UINT32)Sector[35] << 24)
    ));
}

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  EFI_STATUS  Status;

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize % RAM_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > mMedia.LastBlock) || ((BufferSize / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
  Status = EFI_SUCCESS;

  if (!mFirstRead) {
    mFirstRead = TRUE;
    DEBUG ((DEBUG_ERROR, "SHENGMEM: first block read LBA %lu size %lu buffer %p: %r\n",
            (UINT64)Lba, (UINT64)BufferSize, Buffer, Status));
    if ((Lba == 0) && (BufferSize >= RAM_BLOCK_SIZE)) {
      LogFatHeader (Buffer);
    }
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  RamBlockReset,
  RamBlockRead,
  RamBlockWrite,
  RamBlockFlush
};

STATIC
EFI_STATUS
PublishInitrd (
  IN  CONST VOID  *Fdt,
  OUT UINT64      *InitrdStart,
  OUT UINT64      *InitrdEnd,
  OUT EFI_STATUS  *MapStatus,
  OUT EFI_STATUS  *BlockStatus,
  OUT UINT64      *MappedBase,
  OUT UINT64      *MappedEnd,
  OUT UINT32      *MappedPieces
  )
{
  EFI_STATUS  Status;
  EFI_PHYSICAL_ADDRESS  MapBase;
  EFI_PHYSICAL_ADDRESS  MapEnd;
  INT32       Chosen;
  CONST VOID  *StartProp;
  CONST VOID  *EndProp;
  INT32       StartLength;
  INT32       EndLength;
  UINT64      Start;
  UINT64      End;
  EFI_HANDLE  Handle;
  BOOLEAN     Added;
  EFI_PHYSICAL_ADDRESS  PieceBase[MAX_MAPPED_PIECES];
  UINTN                 PieceLength[MAX_MAPPED_PIECES];
  UINTN                 PieceCount;

  *InitrdStart = 0;
  *InitrdEnd   = 0;
  *MapStatus   = EFI_NOT_FOUND;
  *BlockStatus = EFI_NOT_FOUND;
  *MappedBase  = 0;
  *MappedEnd   = 0;
  *MappedPieces = 0;
  PieceCount = 0;
  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: no /chosen\n"));
    return EFI_NOT_FOUND;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) || ((EndLength != 4) && (EndLength != 8)))
  {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: no initrd in /chosen\n"));
    return EFI_NOT_FOUND;
  }

  Start = ReadCells (StartProp, StartLength / 4);
  End   = ReadCells (EndProp, EndLength / 4);
  if (End <= Start) {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: invalid initrd 0x%lx-0x%lx\n", Start, End));
    return EFI_INVALID_PARAMETER;
  }

  //
  // uke's hardware-tested path rounds the extent down to a complete 512-byte
  // sector. Do not reject an otherwise valid initrd for trailing bytes.
  //
  End -= (End - Start) % RAM_BLOCK_SIZE;
  if (End <= Start) {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: initrd 0x%lx-0x%lx has no complete sector\n", Start, End));
    return EFI_INVALID_PARAMETER;
  }

  *InitrdStart = Start;
  *InitrdEnd   = End;

  //
  // The bootloader can place the ramdisk outside the DRAM described by /memory.
  // Keep a non-allocatable identity mapping alive for every later FAT read.
  //
  MapBase = Start & ~((UINT64)SIZE_4KB - 1);
  MapEnd  = ALIGN_VALUE (End, SIZE_4KB);
  if (MapEnd <= MapBase) {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: invalid initrd mapping 0x%lx-0x%lx\n", MapBase, MapEnd));
    return EFI_INVALID_PARAMETER;
  }

  Status = MapFdtRange (
             MapBase,
             (UINTN)(MapEnd - MapBase),
             EfiGcdMemoryTypeReserved,
             EfiGcdMemoryTypeReserved,
             PieceBase,
             PieceLength,
             &PieceCount,
             &Added
             );
  *MapStatus = Status;
  *MappedBase = MapBase;
  *MappedEnd = MapEnd;
  *MappedPieces = (UINT32)PieceCount;
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: failed to map initrd 0x%lx-0x%lx %r\n",
            MapBase, MapEnd - 1, Status));
    return Status;
  }

  DEBUG ((DEBUG_ERROR, "SHENGMEM: initrd 0x%lx-0x%lx mapped=%u\n", Start, End - 1, (UINT32)Added));

  mRamBase                = (UINT8 *)(UINTN)Start;
  mFirstRead              = FALSE;
  mMedia.MediaId          = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = RAM_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = (End - Start) / RAM_BLOCK_SIZE - 1;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  NULL
                  );
  *BlockStatus = Status;
  DEBUG ((DEBUG_ERROR, "SHENGMEM: initrd block device 0x%lx size 0x%lx last LBA 0x%lx %r\n",
          Start, End - Start, (UINT64)mMedia.LastBlock, Status));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ShengBootMemoryDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *Hob;
  UINT64             Dtb;
  VOID               *Fdt;
  EFI_STATUS         Status;
  EFI_STATUS         InitrdStatus;
  EFI_STATUS         MapStatus;
  EFI_STATUS         BlockStatus;
  UINT64             InitrdStart;
  UINT64             InitrdEnd;
  UINTN              FdtSize;
  EFI_PHYSICAL_ADDRESS  ProbeBase;
  UINTN              ProbeSize;
  EFI_PHYSICAL_ADDRESS  ProbeMappedBase[MAX_MAPPED_PIECES];
  UINTN              ProbeMappedLength[MAX_MAPPED_PIECES];
  UINTN              ProbeMappedCount;
  BOOLEAN            ProbeAdded;
  EFI_PHYSICAL_ADDRESS  TailBase;
  UINTN              TailSize;
  EFI_PHYSICAL_ADDRESS  TailMappedBase[MAX_MAPPED_PIECES];
  UINTN              TailMappedLength[MAX_MAPPED_PIECES];
  UINTN              TailMappedCount;
  BOOLEAN            TailAdded;
  UINT64             InitrdMapBase;
  UINT64             InitrdMapEnd;
  UINT32             InitrdMapPieces;

  Hob = GetFirstGuidHob (&gSiliciumEntryArgHobGuid);
  if (Hob == NULL) {
    PublishStatus (0, 0, EFI_NOT_FOUND, EFI_NOT_FOUND, EFI_NOT_FOUND, EFI_NOT_FOUND, 0, 0, 0, 0, 0);
    DEBUG ((DEBUG_ERROR, "SHENGMEM: no entry argument HOB\n"));
    return EFI_SUCCESS;
  }

  Dtb = *(UINT64 *)GET_GUID_HOB_DATA (Hob);
  Status = MapBootFdt (
             Dtb,
             &FdtSize,
             &ProbeBase,
             &ProbeSize,
             ProbeMappedBase,
             ProbeMappedLength,
             &ProbeMappedCount,
             &ProbeAdded,
             &TailBase,
             &TailSize,
             TailMappedBase,
             TailMappedLength,
             &TailMappedCount,
             &TailAdded
             );
  if (EFI_ERROR (Status)) {
    PublishStatus (1, Dtb, Status, EFI_NOT_FOUND, EFI_NOT_FOUND, EFI_NOT_FOUND, 0, 0, 0, 0, 0);
    DEBUG ((DEBUG_ERROR, "SHENGMEM: x0 0x%lx is not a device tree\n", Dtb));
    return EFI_SUCCESS;
  }

  Fdt = (VOID *)(UINTN)Dtb;
  DEBUG ((DEBUG_ERROR, "SHENGMEM: device tree at 0x%lx, %u bytes\n", Dtb, (UINT32)FdtSize));

  InitrdStatus = PublishInitrd (
                   Fdt,
                   &InitrdStart,
                   &InitrdEnd,
                   &MapStatus,
                   &BlockStatus,
                   &InitrdMapBase,
                   &InitrdMapEnd,
                   &InitrdMapPieces
                   );
  AddMemory (Fdt, InitrdMapBase, InitrdMapEnd);

  PublishStatus (
    1,
    Dtb,
    EFI_SUCCESS,
    InitrdStatus,
    MapStatus,
    BlockStatus,
    InitrdStart,
    InitrdEnd,
    InitrdMapBase,
    InitrdMapEnd,
    InitrdMapPieces
    );
  ReleaseMappedPieces (TailMappedBase, TailMappedLength, &TailMappedCount);
  ReleaseMappedPieces (ProbeMappedBase, ProbeMappedLength, &ProbeMappedCount);
  return EFI_SUCCESS;
}
