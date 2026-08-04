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
/*    DRAM[0x8000..0x9FFF]  -> unused                                */
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

#define FDS_BYTE_CYCLES 149  /* CPU cycles between bytes on real hardware */
#define FDS_SEEK_CYCLES 200  /* delay after the transfer is (re)started   */

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
    FDS_IrqLatch = (FDS_IrqLatch & 0xFF00) | byData;
    break;

  case 0x4021: /* IRQ reload value, high byte */
    FDS_IrqLatch = (WORD)((FDS_IrqLatch & 0x00FF) | ((WORD)byData << 8));
    break;

  case 0x4022: /* IRQ control */
    FDS_IrqEnable = byData & 0x03;
    FDS_IrqCounter = FDS_IrqLatch;
    if (!(FDS_IrqEnable & 0x02))
    {
      FDS_TimerIrqPending = false;
    }
    break;

  case 0x4023: /* master I/O enable */
    /* bit0 enables the disk registers, bit1 the sound unit. Nothing is
       gated on it here: the BIOS sets up the timer before enabling, and
       enforcing the gate is a good way to lose those writes. */
    break;

  case 0x4024: /* write data port */
    /* Writing to the disk is Phase 5, so the byte is discarded -- but the
       transfer still has to advance, otherwise a game that tries to save
       would sit waiting for a byte-transfer IRQ that never comes. */
    if (Map20_DriveRunning() && !(FDS_Regs[5] & FDS_CTRL_READ_MODE))
    {
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

    /* Transfer stopping. The BIOS has just consumed the two CRC bytes
       that a real disk carries but an .fds image does not, so step the
       head back over them -- otherwise every block would slip two bytes
       further out of alignment. Proper gap and CRC generation is Phase 4;
       this rewind is what makes block-at-a-time reads line up until then.
       Tested on the start bit alone, independently of the motor and reset
       bits, because the BIOS also ends transfers by asserting reset. */
    if (!(byData & FDS_CTRL_XFER_START))
    {
      if ((byPrev & FDS_CTRL_XFER_START) && !(byData & FDS_CTRL_CRC))
      {
        FDS_DiskPos -= 2;
        if (FDS_DiskPos < 0)
        {
          FDS_DiskPos = 0;
        }
      }
      FDS_XferArmed = false;
      FDS_SeekCycles = 0;
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
      FDS_SeekCycles = FDS_SEEK_CYCLES;
      FDS_XferArmed = true;
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
      byRet = FDS_DiskImage[FDS_CurrentSide * FDS_SIDE_SIZE + FDS_DiskPos];
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
  /*-----------------------------------------------------------------*/
  /*  $4020-$4022 interval timer                                     */
  /*-----------------------------------------------------------------*/
  if (FDS_IrqEnable & 0x02)
  {
    FDS_IrqCounter -= STEP_PER_SCANLINE;
    if (FDS_IrqCounter <= 0)
    {
      if (FDS_IrqEnable & 0x01)
      {
        /* Repeat mode: reload, keeping the overshoot. A latch shorter
           than one scanline would otherwise fire only once per line. */
        FDS_IrqCounter += FDS_IrqLatch;
        if (FDS_IrqCounter <= 0)
        {
          FDS_IrqCounter = FDS_IrqLatch;
        }
      }
      else
      {
        FDS_IrqEnable &= ~0x02;
        FDS_IrqCounter = 0;
      }
      FDS_TimerIrqPending = true;
      IRQ_REQ;
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
      FDS_SeekCycles -= STEP_PER_SCANLINE;
      if (FDS_SeekCycles <= 0)
      {
        /* The byte is now sitting in the read register. Stop the clock
           until the CPU takes it -- Map20_ArmNextByte() restarts it. */
        FDS_XferArmed = false;
        FDS_XferIrqPending = true;
        if (FDS_Regs[5] & FDS_CTRL_IRQ_ENABLE)
        {
          IRQ_REQ;
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
