/*===================================================================*/
/*                                                                   */
/*                Mapper 20 : Famicom Disk System                    */
/*                                                                   */
/*  Phase 1/2 : load path and memory mapping.                        */
/*  Phase 3   : disk controller state machine ($4020-$4033).         */
/*                                                                   */
/*  Not done yet: gap / CRC modelling beyond the two-byte rewind      */
/*  below (Phase 4), disk side switching (Phase 5), writing back to   */
/*  the disk (Phase 5) and the wavetable sound unit (Phase 6).        */
/*                                                                   */
/*===================================================================*/

/*-------------------------------------------------------------------*/
/*  Memory layout inside DRAM[]                                      */
/*                                                                   */
/*  DRAM is 0xA000 bytes and already exists (InfoNES_Mapper.cpp:24)  */
/*  for mapper 235, so the FDS needs no extra RAM at all:            */
/*                                                                   */
/*    DRAM[0x0000..0x5FFF]  -> $8000-$DFFF   PRG RAM   (24 KB)       */
/*    DRAM[0x6000..0x7FFF]  -> $E000-$FFFF   BIOS      ( 8 KB)       */
/*    DRAM[0x8000..0x9FFF]  -> disk write journal      ( 8 KB)       */
/*                                                                   */
/*  $6000-$7FFF uses the existing SRAM[] array, and CHR RAM uses     */
/*  PPURAM[] like any cartridge without VROM.                        */
/*-------------------------------------------------------------------*/

#define FDS_PRGRAM_OFFSET 0x0000
#define FDS_PRGRAM_SIZE 0x6000
#define FDS_BIOS_OFFSET 0x6000

static_assert(FDS_BIOS_OFFSET + FDS_BIOS_SIZE <= DRAM_SIZE,
              "FDS needs 32 KB of DRAM for $8000-$FFFF");

/*-------------------------------------------------------------------*/
/*  Disk timing                                                      */
/*                                                                   */
/*  Real hardware moves one byte every ~149 CPU cycles, but the only  */
/*  clock this port has is MapperHSync(), once per scanline, i.e.     */
/*  every STEP_PER_SCANLINE (114) cycles. So the byte counter is      */
/*  decremented in scanline-sized steps and the remainder is carried  */
/*  over on each re-arm, which keeps the long-run average at 149      */
/*  with at most one scanline of jitter.                             */
/*                                                                   */
/*  Transfers are demand driven: a byte becomes available, the CPU    */
/*  reads $4031 (or writes $4024), and only then is the next one      */
/*  scheduled. That cannot drop bytes the way real hardware does if   */
/*  the CPU is late, which is the forgiving direction to err in.      */
/*-------------------------------------------------------------------*/

/*  Counters run in thirds of a CPU cycle. A scanline is 341 PPU dots =
 *  113.667 CPU cycles, and STEP_PER_SCANLINE rounds that to 114; over a
 *  timer period of ~210 scanlines the rounding alone is worth two thirds
 *  of a scanline, which is enough to move a raster split. Working in
 *  thirds makes the per-scanline step exactly 341 and removes it.        */
#define FDS_THIRDS 3
#define FDS_SCANLINE_THIRDS 341 /* 341 PPU dots = 113.667 CPU cycles */

#define FDS_BYTE_CYCLES (149 * FDS_THIRDS) /* real hardware byte period */
#define FDS_SEEK_CYCLES (200 * FDS_THIRDS) /* delay after a (re)start   */

/* $4025 bits */
#define FDS_CTRL_MOTOR 0x01
#define FDS_CTRL_XFER_RESET 0x02
#define FDS_CTRL_READ_MODE 0x04
#define FDS_CTRL_MIRRORING 0x08
#define FDS_CTRL_CRC 0x10
#define FDS_CTRL_XFER_START 0x40
#define FDS_CTRL_IRQ_ENABLE 0x80

/* $4030 bits */
#define FDS_STATUS_TIMER_IRQ 0x01
#define FDS_STATUS_BYTE_XFER 0x02
#define FDS_STATUS_CRC_ERROR 0x10

/* $4032 bits (all active high, i.e. 1 means "no") */
#define FDS_DRIVE_NO_DISK 0x01
#define FDS_DRIVE_NOT_READY 0x02
#define FDS_DRIVE_PROTECTED 0x04

/*-------------------------------------------------------------------*/
/*  Disk write journal (Phase 5)                                     */
/*                                                                   */
/*  The disk image itself lives in flash and cannot be modified in    */
/*  place, and a whole side (65500 bytes) will not fit in RAM. What   */
/*  games actually write back is one or two small blocks, so the      */
/*  writes are journalled instead: each write transfer becomes an     */
/*  entry, and reads consult the journal before the flash image.      */
/*                                                                   */
/*  The journal is sized to exactly one existing NVRAM slot, so the   */
/*  slot allocation in main.cpp needs no changes at all -- it just    */
/*  stores this instead of SRAM for an FDS image.                     */
/*                                                                   */
/*    magic          4 bytes                                          */
/*    entry count    4 bytes                                          */
/*    fingerprint    4 bytes                                          */
/*    reserved       4 bytes                                          */
/*    entries        8 x 16 bytes                                     */
/*    payload        the rest                                         */
/*                                                                   */
/*  Serialised byte by byte, so nothing depends on DRAM's alignment.  */
/*                                                                   */
/*  The fingerprint identifies the disk the journal belongs to. NVRAM */
/*  slots are handed out by index, so burning a different game reuses */
/*  slot 0 and would otherwise overlay the previous game's writes on  */
/*  top of the new disk -- which corrupts it badly enough that the    */
/*  BIOS cannot read the disk at all.                                 */
/*-------------------------------------------------------------------*/

#define FDS_SAVE_OFFSET 0x8000
#define FDS_SAVE_MAGIC 0x31534446UL /* "FDS1" */
#define FDS_SAVE_MAX_ENTRIES 8
#define FDS_SAVE_ENTRY_BYTES 16
#define FDS_SAVE_HEADER_BYTES (16 + FDS_SAVE_MAX_ENTRIES * FDS_SAVE_ENTRY_BYTES)
#define FDS_SAVE_DATA_BYTES (FDS_SAVE_SIZE - FDS_SAVE_HEADER_BYTES)

static_assert(FDS_SAVE_OFFSET + FDS_SAVE_SIZE <= DRAM_SIZE,
              "the FDS write journal does not fit in DRAM");

/*-------------------------------------------------------------------*/
/*  State                                                            */
/*-------------------------------------------------------------------*/

/* The BIOS is copied here before the emulator starts (see main.cpp).
   It is not part of this repository -- disksys.rom is Nintendo code
   and has to be supplied by the user on the SD card. */
static bool FDS_BiosOk = false;

/* The .fds image in flash: side 0 data, side 1 data, ... */
static const BYTE *FDS_DiskImage = NULL;
static int FDS_DiskSides = 0;
static int FDS_CurrentSide = 0;
static bool FDS_DiskInserted = false;

/* Byte offset of the head within the current side */
static int FDS_DiskPos = 0;

/* $4020-$4026, indexed by address & 7 */
static BYTE FDS_Regs[8];

/* $4020-$4022 interval timer */
static WORD FDS_IrqLatch = 0;
static int FDS_IrqCounter = 0;
static BYTE FDS_IrqEnable = 0; /* bit0 = repeat, bit1 = running */

/* $4030 latched flags, cleared by reading $4030 */
static bool FDS_TimerIrqPending = false;
static bool FDS_XferIrqPending = false;

/* Byte transfer scheduling */
static bool FDS_XferArmed = false; /* a byte is on its way */
static int FDS_SeekCycles = 0;

/* Side switching. Swapping the data under a running BIOS makes it hang,
   so the drive has to go empty for a while first (Phase 5). */
static int FDS_EjectCountdown = 0; /* scanlines left with the drive empty */
static int FDS_PendingSide = 0;
#define FDS_EJECT_SCANLINES (262 * 20) /* about a third of a second */

/* Write journal, held in DRAM[FDS_SAVE_OFFSET..] */
struct FDS_SaveEntry
{
  DWORD dwOffset;  /* byte offset within the side  */
  DWORD dwLength;  /* bytes written                */
  DWORD dwDataOfs; /* offset within the payload    */
  DWORD dwSide;
};
static struct FDS_SaveEntry FDS_SaveIndex[FDS_SAVE_MAX_ENTRIES];
static int FDS_SaveCount = 0;
static DWORD FDS_SaveUsed = 0; /* payload bytes allocated */
static bool FDS_SaveDirty = false;

/* The entry currently being filled by a write transfer, or -1 */
static int FDS_WriteEntry = -1;

/* Temporary instrumentation: reports once a second over the USB serial so
   the IRQ rates can be compared against what the game should be seeing.
   Off now that both the audio and the scrolling behave -- see fds_plan.md
   7.10 for what is still unverified about why the scrolling recovered.
   The render/tempY/addrY fields are the ones to turn back on for that. */
#define FDS_DEBUG_COUNTERS 0

#if FDS_DEBUG_COUNTERS
#define FDS_DEBUG_PERIOD (262 * 60) /* scanlines in about one second */
static int FDS_DbgTick = 0;
static int FDS_DbgTimerIrq = 0;
static int FDS_DbgDiskIrq = 0;
static int FDS_DbgCtrlWrites = 0;
/* Ring of the scanlines the last few timer IRQs fired on. If the game is
   using the timer for a raster split, these should be the same number
   every frame; a creeping value is the split walking up the screen. */
static WORD FDS_DbgFireLine[8];
static int FDS_DbgFireIdx = 0;
/* And where the game re-arms it, which is what the fire line is relative to */
static WORD FDS_DbgArmLine = 0;
/* Sampled at SCAN_VBLANK_END, before InfoNES_HSync() reloads PPU_Addr:
   was rendering enabled (so does the reload happen at all), and what the
   vertical scroll looks like in PPU_Temp vs the accumulated PPU_Addr. */
static BYTE FDS_DbgRender[8];
static WORD FDS_DbgAddrY[8];
static WORD FDS_DbgTempY = 0;
static int FDS_DbgFrameIdx = 0;
#endif

/*-------------------------------------------------------------------*/
/*  Block-level transfer trace                                       */
/*                                                                   */
/*  For the Zelda side B "ERR. 24" report. Error 24 is the BIOS       */
/*  failing to recognise a file header block (block type $03) where   */
/*  it expected one; 22 and 23 are the same complaint about the disk  */
/*  info and file amount blocks. Reaching 24 therefore means blocks   */
/*  $01 and $02 were delivered correctly and alignment was lost       */
/*  somewhere after that -- possibly several good $03/$04 pairs in.   */
/*                                                                   */
/*  The prime suspect is the gap/CRC shortcut listed in fds_plan.md   */
/*  7.5: an .fds image carries no CRC bytes, so Map20_Apu() steps the */
/*  head back 2 on the falling edge of the start bit to compensate.   */
/*  That rewind is skipped when the CRC control bit is set in the     */
/*  same write, and any block that ends that way leaves the head 2    */
/*  bytes off for good -- which looks exactly like ERR 24.            */
/*                                                                   */
/*  This records one entry per read transfer so the two can be told   */
/*  apart. Run side A and side B and compare: on a healthy load every */
/*  entry's wStart equals the previous entry's wEnd minus the rewind, */
/*  and byFirst is a plausible block type. The entry where byFirst    */
/*  stops being a block type is where alignment went.                 */
/*                                                                   */
/*  Left at 1 on this branch because measuring is the only reason the  */
/*  branch exists. It must go back to 0 before any of this reaches     */
/*  main -- the per-entry printing is far too chatty to ship.          */
/*-------------------------------------------------------------------*/
#define FDS_XFER_TRACE 1

#if FDS_XFER_TRACE
#define FDS_TRACE_ENTRIES 20
#define FDS_TRACE_PERIOD (262 * 60 * 3) /* scanlines in about 3 seconds */

struct FDS_TraceRec
{
  WORD wStart;    /* head position when the transfer was armed        */
  WORD wEnd;      /* head position when it stopped, before the rewind */
  WORD wCount;    /* bytes the CPU actually moved                     */
  WORD wJOfs;     /* journal entry offset, writes only                */
  WORD wJLen;     /* journal entry length, writes only                */
  BYTE byFirst;   /* first byte moved -- block type on a good read    */
  BYTE byCtrl;    /* the $4025 value that stopped the transfer        */
  BYTE byRewound; /* whether the 2-byte CRC rewind fired              */
  BYTE byWrite;   /* 0 = read transfer, 1 = write transfer            */
  BYTE bySide;
};

static struct FDS_TraceRec FDS_Trace[FDS_TRACE_ENTRIES];
static int FDS_TraceIdx = 0;    /* next slot to write */
static int FDS_TraceFilled = 0;
static bool FDS_TraceOpen = false;
static int FDS_TraceTick = 0;

static void FDS_TraceStart(void)
{
  struct FDS_TraceRec *e = &FDS_Trace[FDS_TraceIdx];
  e->wStart = (WORD)FDS_DiskPos;
  e->wEnd = (WORD)FDS_DiskPos;
  e->wCount = 0;
  e->wJOfs = 0;
  e->wJLen = 0;
  e->byFirst = 0xff;
  e->byCtrl = 0;
  e->byRewound = 0;
  e->byWrite = (FDS_Regs[5] & FDS_CTRL_READ_MODE) ? 0 : 1;
  e->bySide = (BYTE)FDS_CurrentSide;
  FDS_TraceOpen = true;
}

static void FDS_TraceByte(BYTE byData)
{
  struct FDS_TraceRec *e;
  if (!FDS_TraceOpen)
  {
    return;
  }
  e = &FDS_Trace[FDS_TraceIdx];
  if (e->wCount == 0)
  {
    e->byFirst = byData;
  }
  e->wCount++;
}

static void FDS_TraceStop(BYTE byCtrl, bool bRewound)
{
  struct FDS_TraceRec *e;
  if (!FDS_TraceOpen)
  {
    return;
  }
  e = &FDS_Trace[FDS_TraceIdx];
  e->wEnd = (WORD)FDS_DiskPos;
  e->byCtrl = byCtrl;
  e->byRewound = bRewound ? 1 : 0;
  /* Captured before the caller clears FDS_WriteEntry. The journal entry is
     what a later read will overlay onto the image, so its offset and length
     are the thing to compare against how far the head actually moved. */
  if (FDS_WriteEntry >= 0)
  {
    e->wJOfs = (WORD)FDS_SaveIndex[FDS_WriteEntry].dwOffset;
    e->wJLen = (WORD)FDS_SaveIndex[FDS_WriteEntry].dwLength;
  }
  FDS_TraceOpen = false;
  FDS_TraceIdx = (FDS_TraceIdx + 1) % FDS_TRACE_ENTRIES;
  if (FDS_TraceFilled < FDS_TRACE_ENTRIES)
  {
    FDS_TraceFilled++;
  }
}

/* Reprints the whole ring every few seconds rather than printing each
   entry as it happens. Two reasons: a USB CDC re-enumeration leaves the
   host on a stale handle that reads nothing and reports no error
   (fds_plan.md 7.8), and once the BIOS gives up and shows ERR. 24 the
   disk stops, so the ring freezes holding the run-up to the failure and
   every later reprint carries it. Catching any one reprint is enough. */
static void FDS_TraceHsync(void)
{
  int k;

  if (++FDS_TraceTick < FDS_TRACE_PERIOD)
  {
    return;
  }
  FDS_TraceTick = 0;
  if (FDS_TraceFilled == 0)
  {
    return;
  }

  /* The journal is the other half of the picture: it survives a ROM
     re-burn (the NVRAM slot sits below NES_FILE_ADDR, outside the erased
     range), so a bad entry written once stays until something overwrites
     it. Each entry claims [ofs, ofs+len) and is overlaid on top of the
     image by Map20_SaveLookup(), so comparing len against how far the
     head actually moved during the matching W transfer is the test. */
  {
    int j;
    InfoNES_MessageBox("JOURNAL count=%d used=%u", FDS_SaveCount,
                       (unsigned)FDS_SaveUsed);
    for (j = 0; j < FDS_SaveCount; j++)
    {
      InfoNES_MessageBox("JOURNAL %d side=%u ofs=%5u len=%5u dataofs=%5u", j,
                         (unsigned)FDS_SaveIndex[j].dwSide,
                         (unsigned)FDS_SaveIndex[j].dwOffset,
                         (unsigned)FDS_SaveIndex[j].dwLength,
                         (unsigned)FDS_SaveIndex[j].dwDataOfs);
    }
  }

  InfoNES_MessageBox("XFER side=%d entries=%d (oldest first)",
                     FDS_CurrentSide, FDS_TraceFilled);
  for (k = 0; k < FDS_TraceFilled; k++)
  {
    /* Oldest first. FDS_TraceIdx points at the next slot to write, so it
       is also the oldest once the ring has wrapped. */
    int i = (FDS_TraceIdx + FDS_TRACE_ENTRIES - FDS_TraceFilled + k) %
            FDS_TRACE_ENTRIES;
    const struct FDS_TraceRec *e = &FDS_Trace[i];
    InfoNES_MessageBox(
        "XFER %2d s=%d %s st=%5u b=%02x n=%5u en=%5u c=%02x r=%d "
        "jofs=%5u jlen=%5u",
        k, (int)e->bySide, e->byWrite ? "W" : "R", (unsigned)e->wStart,
        (unsigned)e->byFirst, (unsigned)e->wCount, (unsigned)e->wEnd,
        (unsigned)e->byCtrl, (int)e->byRewound, (unsigned)e->wJOfs,
        (unsigned)e->wJLen);
  }
}
#endif /* FDS_XFER_TRACE */

/*-------------------------------------------------------------------*/
/*  Interface used by the loader (main.cpp)                          */
/*-------------------------------------------------------------------*/

/* Destination for the 8 KB BIOS image. Valid before Map20_Init() runs,
   because DRAM is a plain global -- Map20_Init() only clears the PRG
   RAM half and never touches the BIOS half. */
BYTE *FDS_GetBiosBuffer(void)
{
  return &DRAM[FDS_BIOS_OFFSET];
}

void FDS_SetBiosPresent(bool bPresent)
{
  FDS_BiosOk = bPresent;
}

bool FDS_IsBiosPresent(void)
{
  return FDS_BiosOk;
}

void FDS_SetDiskImage(const BYTE *pImage, int nSides)
{
  FDS_DiskImage = pImage;
  FDS_DiskSides = nSides;
  FDS_CurrentSide = 0;
}

bool FDS_IsDiskLoaded(void)
{
  return FDS_DiskImage != NULL && FDS_DiskSides > 0;
}

/* Reports the side that will be in the drive once a swap in progress
   finishes, which is what a caller wants to display. */
int FDS_GetSide(void)
{
  return (FDS_EjectCountdown > 0) ? FDS_PendingSide : FDS_CurrentSide;
}

int FDS_GetSideCount(void)
{
  return FDS_DiskSides;
}

/*-------------------------------------------------------------------*/
/*  Write journal                                                    */
/*-------------------------------------------------------------------*/

static BYTE *Map20_SaveBuf(void)
{
  return &DRAM[FDS_SAVE_OFFSET];
}

static BYTE *Map20_SavePayload(void)
{
  return &DRAM[FDS_SAVE_OFFSET + FDS_SAVE_HEADER_BYTES];
}

static void Map20_PutDword(BYTE *p, DWORD v)
{
  p[0] = (BYTE)v;
  p[1] = (BYTE)(v >> 8);
  p[2] = (BYTE)(v >> 16);
  p[3] = (BYTE)(v >> 24);
}

static DWORD Map20_GetDword(const BYTE *p)
{
  return (DWORD)p[0] | ((DWORD)p[1] << 8) |
         ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

/* Identifies the disk currently loaded. The disk info block carries the
   maker, game name and revision, so the first 64 bytes of side 0 plus the
   side count separate any two games that matter. */
static DWORD Map20_DiskFingerprint(void)
{
  DWORD dw = 0x811C9DC5UL; /* FNV-1a */
  int i;

  if (FDS_DiskImage == NULL)
  {
    return 0;
  }
  for (i = 0; i < 64; i++)
  {
    dw ^= FDS_DiskImage[i];
    dw *= 16777619UL;
  }
  dw ^= (DWORD)FDS_DiskSides;
  dw *= 16777619UL;
  return dw;
}

static void Map20_SaveReset(void)
{
  FDS_SaveCount = 0;
  FDS_SaveUsed = 0;
  FDS_WriteEntry = -1;
  FDS_SaveDirty = false;
  InfoNES_MemorySet(FDS_SaveIndex, 0, sizeof FDS_SaveIndex);
}

/* 8 KB buffer to hand to the NVRAM code in main.cpp */
BYTE *FDS_GetSaveBuffer(void)
{
  return Map20_SaveBuf();
}

bool FDS_IsSaveDirty(void)
{
  return FDS_SaveDirty;
}

/* Pack the in-RAM index into the buffer, ready to be written to flash. */
void FDS_SerializeSave(void)
{
  BYTE *p = Map20_SaveBuf();
  int i;

  Map20_PutDword(p + 0, FDS_SAVE_MAGIC);
  Map20_PutDword(p + 4, (DWORD)FDS_SaveCount);
  Map20_PutDword(p + 8, Map20_DiskFingerprint());
  Map20_PutDword(p + 12, 0);
  for (i = 0; i < FDS_SAVE_MAX_ENTRIES; i++)
  {
    BYTE *e = p + 16 + i * FDS_SAVE_ENTRY_BYTES;
    Map20_PutDword(e + 0, FDS_SaveIndex[i].dwOffset);
    Map20_PutDword(e + 4, FDS_SaveIndex[i].dwLength);
    Map20_PutDword(e + 8, FDS_SaveIndex[i].dwDataOfs);
    Map20_PutDword(e + 12, FDS_SaveIndex[i].dwSide);
  }
  FDS_SaveDirty = false;
}

/* Rebuild the index from whatever main.cpp loaded into the buffer.
   An erased flash slot reads as 0xFF and simply fails the magic check. */
void FDS_DeserializeSave(void)
{
  const BYTE *p = Map20_SaveBuf();
  int i;

  if (Map20_GetDword(p) != FDS_SAVE_MAGIC)
  {
    Map20_SaveReset();
    return;
  }

  if (Map20_GetDword(p + 8) != Map20_DiskFingerprint())
  {
    /* The slot belongs to a different disk -- almost certainly the game
       that was burned here before this one. Applying it would corrupt
       the image. */
    InfoNES_MessageBox("FDS save slot belongs to another disk, ignoring");
    Map20_SaveReset();
    return;
  }

  FDS_SaveCount = (int)Map20_GetDword(p + 4);
  if (FDS_SaveCount < 0 || FDS_SaveCount > FDS_SAVE_MAX_ENTRIES)
  {
    Map20_SaveReset();
    return;
  }

  FDS_SaveUsed = 0;
  for (i = 0; i < FDS_SAVE_MAX_ENTRIES; i++)
  {
    const BYTE *e = p + 16 + i * FDS_SAVE_ENTRY_BYTES;
    FDS_SaveIndex[i].dwOffset = Map20_GetDword(e + 0);
    FDS_SaveIndex[i].dwLength = Map20_GetDword(e + 4);
    FDS_SaveIndex[i].dwDataOfs = Map20_GetDword(e + 8);
    FDS_SaveIndex[i].dwSide = Map20_GetDword(e + 12);
  }

  /* Distrust the stored bounds -- this came off flash. */
  for (i = 0; i < FDS_SaveCount; i++)
  {
    DWORD end = FDS_SaveIndex[i].dwDataOfs + FDS_SaveIndex[i].dwLength;
    if (FDS_SaveIndex[i].dwLength > FDS_SAVE_DATA_BYTES ||
        end > FDS_SAVE_DATA_BYTES ||
        end < FDS_SaveIndex[i].dwDataOfs)
    {
      Map20_SaveReset();
      return;
    }
    if (end > FDS_SaveUsed)
    {
      FDS_SaveUsed = end;
    }
  }

  FDS_WriteEntry = -1;
  FDS_SaveDirty = false;
}

/* Overlay lookup on the read path. Returns -1 when the byte is not
   journalled and the flash image should be used. */
static inline int Map20_SaveLookup(int nSide, int nPos)
{
  int i;

  for (i = 0; i < FDS_SaveCount; i++)
  {
    if (FDS_SaveIndex[i].dwSide != (DWORD)nSide)
    {
      continue;
    }
    if ((DWORD)nPos >= FDS_SaveIndex[i].dwOffset &&
        (DWORD)nPos < FDS_SaveIndex[i].dwOffset + FDS_SaveIndex[i].dwLength)
    {
      return (int)(FDS_SaveIndex[i].dwDataOfs +
                   ((DWORD)nPos - FDS_SaveIndex[i].dwOffset));
    }
  }
  return -1;
}

/* Begin journalling a write transfer that starts at the current head. */
static void Map20_SaveBeginWrite(void)
{
  int i;

  FDS_WriteEntry = -1;

  /* Rewriting the same block is the normal case, so reuse its entry and
     its payload rather than growing the journal every time the player
     saves. Only an exact match is reused: a partial overlap would need
     splitting, and no game has been seen to do it. */
  for (i = 0; i < FDS_SaveCount; i++)
  {
    if (FDS_SaveIndex[i].dwSide == (DWORD)FDS_CurrentSide &&
        FDS_SaveIndex[i].dwOffset == (DWORD)FDS_DiskPos)
    {
      FDS_WriteEntry = i;
      FDS_SaveIndex[i].dwLength = 0;
      return;
    }
  }

  if (FDS_SaveCount >= FDS_SAVE_MAX_ENTRIES)
  {
    return; /* journal full -- the write is dropped */
  }

  i = FDS_SaveCount;
  FDS_SaveIndex[i].dwSide = (DWORD)FDS_CurrentSide;
  FDS_SaveIndex[i].dwOffset = (DWORD)FDS_DiskPos;
  FDS_SaveIndex[i].dwLength = 0;
  FDS_SaveIndex[i].dwDataOfs = FDS_SaveUsed;
  FDS_SaveCount++;
  FDS_WriteEntry = i;
}

static void Map20_SaveWriteByte(BYTE byData)
{
  struct FDS_SaveEntry *e;
  DWORD dwLimit;

  if (FDS_WriteEntry < 0)
  {
    return;
  }

  e = &FDS_SaveIndex[FDS_WriteEntry];

  /* A reused entry may only grow into the space it already owns; a fresh
     one may grow into whatever is left at the tail. */
  if (FDS_WriteEntry == FDS_SaveCount - 1)
  {
    dwLimit = FDS_SAVE_DATA_BYTES - e->dwDataOfs;
  }
  else
  {
    dwLimit = FDS_SaveIndex[FDS_WriteEntry + 1].dwDataOfs - e->dwDataOfs;
  }

  if (e->dwLength >= dwLimit)
  {
    return; /* out of room -- drop the rest of this block */
  }

  Map20_SavePayload()[e->dwDataOfs + e->dwLength] = byData;
  e->dwLength++;
  if (FDS_WriteEntry == FDS_SaveCount - 1 &&
      e->dwDataOfs + e->dwLength > FDS_SaveUsed)
  {
    FDS_SaveUsed = e->dwDataOfs + e->dwLength;
  }
  FDS_SaveDirty = true;
}

/*-------------------------------------------------------------------*/
/*  Side switching                                                   */
/*-------------------------------------------------------------------*/

void FDS_SetSide(int nSide)
{
  if (!FDS_IsDiskLoaded() || FDS_DiskSides < 2)
  {
    return;
  }

  nSide %= FDS_DiskSides;
  if (nSide < 0)
  {
    nSide += FDS_DiskSides;
  }

  /* Eject first and let the drive sit empty for a moment. Swapping the
     data underneath a running BIOS is what makes it hang. */
  FDS_PendingSide = nSide;
  FDS_DiskInserted = false;
  FDS_EjectCountdown = FDS_EJECT_SCANLINES;
  FDS_DiskPos = 0;
  FDS_XferArmed = false;
  FDS_SeekCycles = 0;
  FDS_WriteEntry = -1;
}

void FDS_NextSide(void)
{
  FDS_SetSide(FDS_CurrentSide + 1);
}

void FDS_PrevSide(void)
{
  FDS_SetSide(FDS_CurrentSide - 1);
}

/*-------------------------------------------------------------------*/
/*  Helpers                                                          */
/*-------------------------------------------------------------------*/

/* True while the drive is spinning and not held in transfer reset. */
static inline bool Map20_DriveRunning(void)
{
  return FDS_DiskInserted &&
         (FDS_Regs[5] & FDS_CTRL_MOTOR) &&
         !(FDS_Regs[5] & FDS_CTRL_XFER_RESET);
}

/* Schedule the next byte, carrying over however far the previous one
   overshot the scanline boundary. */
static inline void Map20_ArmNextByte(void)
{
  if (!Map20_DriveRunning() || !(FDS_Regs[5] & FDS_CTRL_XFER_START))
  {
    FDS_XferArmed = false;
    return;
  }
  FDS_SeekCycles += FDS_BYTE_CYCLES;
  /* If the CPU fell so far behind that the carry is meaningless, drop it
     rather than free-running a backlog of instant IRQs. */
  if (FDS_SeekCycles < 0)
  {
    FDS_SeekCycles = FDS_BYTE_CYCLES;
  }
  FDS_XferArmed = true;
}

/*-------------------------------------------------------------------*/
/*  Initialize Mapper 20                                             */
/*-------------------------------------------------------------------*/
void Map20_Init()
{
  /* Initialize Mapper */
  MapperInit = Map20_Init;

  /* Write to Mapper */
  MapperWrite = Map20_Write;

  /* Write to SRAM */
  MapperSram = Map20_Sram;

  /* Write to APU */
  MapperApu = Map20_Apu;

  /* Read from APU */
  MapperReadApu = Map20_ReadApu;

  /* Callback at VSync */
  MapperVSync = Map20_VSync;

  /* Callback at HSync */
  MapperHSync = Map20_HSync;

  /* Callback at PPU */
  MapperPPU = Map20_PPU;

  /* Callback at Rendering Screen ( 1:BG, 0:Sprite ) */
  MapperRenderScreen = Map20_RenderScreen;

  /* $6000-$7FFF is plain work RAM */
  SRAMBANK = SRAM;

  /* Clear the 24 KB of PRG RAM, but leave the BIOS half alone */
  InfoNES_MemorySet(&DRAM[FDS_PRGRAM_OFFSET], 0, FDS_PRGRAM_SIZE);

  /* Set ROM Banks */
  ROMBANK0 = &DRAM[FDS_PRGRAM_OFFSET + 0x0000]; /* $8000 RAM */
  ROMBANK1 = &DRAM[FDS_PRGRAM_OFFSET + 0x2000]; /* $A000 RAM */
  ROMBANK2 = &DRAM[FDS_PRGRAM_OFFSET + 0x4000]; /* $C000 RAM */
  ROMBANK3 = &DRAM[FDS_BIOS_OFFSET];            /* $E000 BIOS, read only */

  /* Reset the disk controller */
  InfoNES_MemorySet(FDS_Regs, 0, sizeof FDS_Regs);
  FDS_IrqLatch = 0;
  FDS_IrqCounter = 0;
  FDS_IrqEnable = 0;
  FDS_TimerIrqPending = false;
  FDS_XferIrqPending = false;
  FDS_XferArmed = false;
  FDS_SeekCycles = 0;
  FDS_DiskPos = 0;
  FDS_CurrentSide = 0;
  FDS_EjectCountdown = 0;
  FDS_PendingSide = 0;

  /* The write journal is deliberately NOT reset here. main.cpp fills it
     from flash via loadNVRAM() + FDS_DeserializeSave() before calling
     InfoNES_Reset(), and clearing it now would throw the save away. */
  FDS_WriteEntry = -1;

  /* The disk starts out in the drive. Modelling the "insert it yourself"
     prompt would only make the user press a button for no reason;
     ejecting and swapping sides is Phase 5. */
  FDS_DiskInserted = (FDS_DiskImage != NULL && FDS_DiskSides > 0);

  /* CHR RAM: NesHeader.byVRomSize is 0 for a synthesized FDS header, so
     InfoNES_SetupPPU() has already pointed PPUBANK[] at PPURAM and set
     byVramWriteEnable. Nothing to do here. */

  /* Set up wiring of the interrupt pin */
  K6502_Set_Int_Wiring(1, 1);
}

/*-------------------------------------------------------------------*/
/*  Mapper 20 Write Function                                         */
/*-------------------------------------------------------------------*/
void __not_in_flash_func(Map20_Write)(WORD wAddr, BYTE byData)
{
  /* $8000-$DFFF is RAM; $E000-$FFFF is the BIOS and ignores writes. */
  if (wAddr < 0xE000)
  {
    DRAM[FDS_PRGRAM_OFFSET + (wAddr - 0x8000)] = byData;
  }
}

/*-------------------------------------------------------------------*/
/*  Mapper 20 Write to SRAM Function                                 */
/*-------------------------------------------------------------------*/
void __not_in_flash_func(Map20_Sram)(WORD wAddr, BYTE byData)
{
  /* Never reached: the synthesized header sets ROM_SRAM, so K6502_Write
     stores $6000-$7FFF straight into SRAM[]. */
}

/*-------------------------------------------------------------------*/
/*  Mapper 20 Write to APU Function                                  */
/*-------------------------------------------------------------------*/
void __not_in_flash_func(Map20_Apu)(WORD wAddr, BYTE byData)
{
  BYTE byPrev;

  switch (wAddr)
  {
  case 0x4020: /* IRQ reload value, low byte */
    if (!(FDS_Regs[3] & 0x01))
    {
      return;
    }
    FDS_IrqLatch = (FDS_IrqLatch & 0xFF00) | byData;
    break;

  case 0x4021: /* IRQ reload value, high byte */
    if (!(FDS_Regs[3] & 0x01))
    {
      return;
    }
    FDS_IrqLatch = (WORD)((FDS_IrqLatch & 0x00FF) | ((WORD)byData << 8));
    break;

  case 0x4022: /* IRQ control */
    /* Not writable while $4023 bit0 is clear. Without this gate a write
       made with disk I/O disabled starts a timer that real hardware would
       have ignored, and the spurious IRQs land in whatever the game does
       on IRQ -- typically its music engine and mid-frame scroll writes. */
    if (!(FDS_Regs[3] & 0x01))
    {
      return;
    }
    FDS_IrqEnable = byData & 0x03;
    FDS_IrqCounter = (int)FDS_IrqLatch * FDS_THIRDS;
#if FDS_DEBUG_COUNTERS
    if (byData & 0x02)
    {
      FDS_DbgArmLine = (WORD)PPU_Scanline;
    }
#endif
    if (!(FDS_IrqEnable & 0x02))
    {
      FDS_TimerIrqPending = false;
    }
    break;

  case 0x4023: /* master I/O enable */
    /* bit0 enables the disk registers and the timer, bit1 the sound unit.
       Clearing bit0 stops the timer and acknowledges both IRQ sources. */
    FDS_Regs[3] = byData;
    if (!(byData & 0x01))
    {
      FDS_IrqEnable = 0;
      FDS_IrqCounter = 0;
      FDS_TimerIrqPending = false;
      FDS_XferIrqPending = false;
    }
    return;

  case 0x4024: /* write data port */
    if (Map20_DriveRunning() && !(FDS_Regs[5] & FDS_CTRL_READ_MODE))
    {
      Map20_SaveWriteByte(byData);
#if FDS_XFER_TRACE
      FDS_TraceByte(byData);
#endif
      if (FDS_DiskPos < FDS_SIDE_SIZE - 1)
      {
        FDS_DiskPos++;
      }
    }
    FDS_XferIrqPending = false;
    Map20_ArmNextByte();
    break;

  case 0x4025: /* FDS control */
  {
    bool bWasRunning = Map20_DriveRunning();
    bool bNowRunning;

    byPrev = FDS_Regs[5];
#if FDS_DEBUG_COUNTERS
    FDS_DbgCtrlWrites++;
#endif

    /* Transfer stopping. The BIOS has just consumed the two CRC bytes
       that a real disk carries but an .fds image does not, so step the
       head back over them -- otherwise every block would slip two bytes
       further out of alignment. Proper gap and CRC generation is Phase 4;
       this rewind is what makes block-at-a-time reads line up until then.
       Tested on the start bit alone, independently of the motor and reset
       bits, because the BIOS also ends transfers by asserting reset. */
    if (!(byData & FDS_CTRL_XFER_START))
    {
      bool bRewound = (byPrev & FDS_CTRL_XFER_START) && !(byData & FDS_CTRL_CRC);
#if FDS_XFER_TRACE
      /* Logged before the rewind so the raw head position is visible. */
      FDS_TraceStop(byData, bRewound);
#endif
      if (bRewound)
      {
        FDS_DiskPos -= 2;
        if (FDS_DiskPos < 0)
        {
          FDS_DiskPos = 0;
        }
      }
      FDS_XferArmed = false;
      FDS_SeekCycles = 0;
      FDS_WriteEntry = -1;
    }

    if (!(byData & FDS_CTRL_MOTOR))
    {
      /* Motor off. The Quick Disk mechanism returns the head to the start,
         and the BIOS depends on that to re-scan the disk for a file. */
      FDS_DiskPos = 0;
      FDS_XferArmed = false;
      FDS_SeekCycles = 0;
    }
    else if (byData & FDS_CTRL_XFER_RESET)
    {
      /* Held in reset: stop feeding bytes but leave the head alone. */
      FDS_XferArmed = false;
      FDS_SeekCycles = 0;
    }

    /* Commit before arming, so Map20_DriveRunning() sees the new state. */
    FDS_Regs[5] = byData;
    bNowRunning = Map20_DriveRunning();

    /* Start the byte clock on the leading edge of either condition: the
       start bit going up, or the drive spinning up while it is already
       set. Deliberately not re-armed when $4025 is rewritten mid-transfer
       (to toggle the IRQ enable, say) -- that would discard a byte the
       CPU has not collected yet. */
    if ((byData & FDS_CTRL_XFER_START) && bNowRunning &&
        (!(byPrev & FDS_CTRL_XFER_START) || !bWasRunning))
    {
#if FDS_XFER_TRACE
      FDS_TraceStart();
#endif
      FDS_SeekCycles = FDS_SEEK_CYCLES;
      FDS_XferArmed = true;
      if (!(byData & FDS_CTRL_READ_MODE))
      {
        /* A write transfer starts here; journal it from the current head. */
        Map20_SaveBeginWrite();
      }
    }

    /* Mirroring: 0 = vertical, 1 = horizontal, which is the opposite of
       InfoNES_Mirroring()'s argument. */
    InfoNES_Mirroring((byData & FDS_CTRL_MIRRORING) ? 0 : 1);
    return;
  }

  case 0x4026: /* external connector */
    break;

  default:
    /* The wavetable sound unit at $4040-$408F is Phase 6. */
    return;
  }

  FDS_Regs[wAddr & 7] = byData;
}

/*-------------------------------------------------------------------*/
/*  Mapper 20 Read from APU Function                                 */
/*-------------------------------------------------------------------*/
BYTE __not_in_flash_func(Map20_ReadApu)(WORD wAddr)
{
  BYTE byRet;

  switch (wAddr)
  {
  case 0x4030: /* disk status 0 -- reading acknowledges both IRQ flags */
    byRet = 0;
    if (FDS_TimerIrqPending)
    {
      byRet |= FDS_STATUS_TIMER_IRQ;
    }
    if (FDS_XferIrqPending)
    {
      byRet |= FDS_STATUS_BYTE_XFER;
    }
    /* FDS_STATUS_CRC_ERROR stays clear: an .fds image carries no CRC and
       the BIOS only checks that the field is there, it never verifies. */
    FDS_TimerIrqPending = false;
    FDS_XferIrqPending = false;
    return byRet;

  case 0x4031: /* read data port */
    byRet = 0;
    if (FDS_DiskInserted && FDS_DiskImage != NULL)
    {
      int nSaved = Map20_SaveLookup(FDS_CurrentSide, FDS_DiskPos);
      if (nSaved >= 0)
      {
        byRet = Map20_SavePayload()[nSaved];
      }
      else
      {
        byRet = FDS_DiskImage[FDS_CurrentSide * FDS_SIDE_SIZE + FDS_DiskPos];
      }
#if FDS_XFER_TRACE
      FDS_TraceByte(byRet);
#endif
      if (FDS_DiskPos < FDS_SIDE_SIZE - 1)
      {
        FDS_DiskPos++;
      }
    }
    FDS_XferIrqPending = false;
    Map20_ArmNextByte();
    return byRet;

  case 0x4032: /* drive status */
    byRet = 0;
    if (!FDS_DiskInserted)
    {
      byRet |= FDS_DRIVE_NO_DISK | FDS_DRIVE_NOT_READY | FDS_DRIVE_PROTECTED;
    }
    else if (!(FDS_Regs[5] & FDS_CTRL_MOTOR) ||
             (FDS_Regs[5] & FDS_CTRL_XFER_RESET))
    {
      byRet |= FDS_DRIVE_NOT_READY;
    }
    /* Reported writable even though writes are discarded, so that games
       do not refuse to start. Saving properly is Phase 5. */
    return byRet;

  case 0x4033: /* external connector / battery */
    /* bit7 = battery good. Reporting a flat battery makes the BIOS
       refuse to boot, so this one matters. */
    return 0x80;

  default:
    /* Unmapped: the CPU core's convention for an unreadable register. */
    return (BYTE)(wAddr >> 8);
  }
}

/*-------------------------------------------------------------------*/
/*  Mapper 20 V-Sync Function                                        */
/*-------------------------------------------------------------------*/
void __not_in_flash_func(Map20_VSync)()
{
}

/*-------------------------------------------------------------------*/
/*  Mapper 20 H-Sync Function                                        */
/*-------------------------------------------------------------------*/
void __not_in_flash_func(Map20_HSync)()
{
#if FDS_DEBUG_COUNTERS
  if (PPU_Scanline == SCAN_VBLANK_END)
  {
    /* Runs before InfoNES_HSync(), so PPU_Addr is still what the frame
       accumulated and has not been reloaded from PPU_Temp yet. */
    int i = FDS_DbgFrameIdx & 7;
    FDS_DbgRender[i] =
        (BYTE)((PPU_R1 & (R1_SHOW_SP | R1_SHOW_SCR)) ? 1 : 0);
    FDS_DbgAddrY[i] =
        (WORD)((((PPU_Addr >> 5) & 31) * 8) + ((PPU_Addr >> 12) & 7));
    FDS_DbgTempY =
        (WORD)((((PPU_Temp >> 5) & 31) * 8) + ((PPU_Temp >> 12) & 7));
    FDS_DbgFrameIdx++;
  }

  if (++FDS_DbgTick >= FDS_DEBUG_PERIOD)
  {
    FDS_DbgTick = 0;
    InfoNES_MessageBox(
        "FDS tIRQ/s=%d fire@%d R1=%02x render=%d%d%d%d%d%d%d%d "
        "tempY=%d addrY=%d,%d,%d,%d,%d,%d,%d,%d",
        FDS_DbgTimerIrq, FDS_DbgFireLine[0], PPU_R1,
        FDS_DbgRender[0], FDS_DbgRender[1], FDS_DbgRender[2],
        FDS_DbgRender[3], FDS_DbgRender[4], FDS_DbgRender[5],
        FDS_DbgRender[6], FDS_DbgRender[7], FDS_DbgTempY,
        FDS_DbgAddrY[0], FDS_DbgAddrY[1], FDS_DbgAddrY[2], FDS_DbgAddrY[3],
        FDS_DbgAddrY[4], FDS_DbgAddrY[5], FDS_DbgAddrY[6], FDS_DbgAddrY[7]);
    FDS_DbgTimerIrq = 0;
    FDS_DbgDiskIrq = 0;
    FDS_DbgCtrlWrites = 0;
  }
#endif

#if FDS_XFER_TRACE
  FDS_TraceHsync();
#endif

  /*-----------------------------------------------------------------*/
  /*  Side switching: finish the eject/insert cycle                  */
  /*-----------------------------------------------------------------*/
  if (FDS_EjectCountdown > 0)
  {
    if (--FDS_EjectCountdown == 0)
    {
      FDS_CurrentSide = FDS_PendingSide;
      FDS_DiskPos = 0;
      FDS_DiskInserted = true;
    }
  }

  /*-----------------------------------------------------------------*/
  /*  $4020-$4022 interval timer                                     */
  /*-----------------------------------------------------------------*/
  if (FDS_IrqEnable & 0x02)
  {
    FDS_IrqCounter -= FDS_SCANLINE_THIRDS;
    if (FDS_IrqCounter <= 0)
    {
      if (FDS_IrqEnable & 0x01)
      {
        /* Repeat mode: reload, keeping the overshoot. A latch shorter
           than one scanline would otherwise fire only once per line. */
        FDS_IrqCounter += (int)FDS_IrqLatch * FDS_THIRDS;
        if (FDS_IrqCounter <= 0)
        {
          FDS_IrqCounter = (int)FDS_IrqLatch * FDS_THIRDS;
        }
      }
      else
      {
        FDS_IrqEnable &= ~0x02;
        FDS_IrqCounter = 0;
      }
      FDS_TimerIrqPending = true;
      IRQ_REQ;
#if FDS_DEBUG_COUNTERS
      FDS_DbgTimerIrq++;
      FDS_DbgFireLine[FDS_DbgFireIdx & 7] = (WORD)PPU_Scanline;
      FDS_DbgFireIdx++;
#endif
    }
  }

  /*-----------------------------------------------------------------*/
  /*  Disk byte transfer                                             */
  /*-----------------------------------------------------------------*/
  if (FDS_XferArmed)
  {
    if (!Map20_DriveRunning())
    {
      FDS_XferArmed = false;
    }
    else
    {
      FDS_SeekCycles -= FDS_SCANLINE_THIRDS;
      if (FDS_SeekCycles <= 0)
      {
        /* The byte is now sitting in the read register. Stop the clock
           until the CPU takes it -- Map20_ArmNextByte() restarts it. */
        FDS_XferArmed = false;
        FDS_XferIrqPending = true;
        if (FDS_Regs[5] & FDS_CTRL_IRQ_ENABLE)
        {
          IRQ_REQ;
#if FDS_DEBUG_COUNTERS
          FDS_DbgDiskIrq++;
#endif
        }
      }
    }
  }
}

/*-------------------------------------------------------------------*/
/*  Mapper 20 PPU Function                                           */
/*-------------------------------------------------------------------*/
void __not_in_flash_func(Map20_PPU)(WORD wAddr)
{
}

/*-------------------------------------------------------------------*/
/*  Mapper 20 Rendering Screen Function                              */
/*-------------------------------------------------------------------*/
void __not_in_flash_func(Map20_RenderScreen)(BYTE byMode)
{
}
