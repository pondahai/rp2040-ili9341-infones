/*===================================================================*/
/*                                                                   */
/*                Mapper 20 : Famicom Disk System                    */
/*                                                                   */
/*  Phase 1/2 : load path and memory mapping only.                   */
/*              The disk controller state machine ($4020-$4033)      */
/*              is Phase 3 -- the register handlers below are        */
/*              deliberately minimal: they report "no disk in the    */
/*              drive", which is enough for the BIOS to reach its    */
/*              boot screen and wait there.                          */
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

/* The BIOS is copied here before the emulator starts (see main.cpp).
   It is not part of this repository -- disksys.rom is Nintendo code
   and has to be supplied by the user on the SD card. */
static bool FDS_BiosOk = false;

/* The .fds image in flash: side 0 data, side 1 data, ... */
static const BYTE *FDS_DiskImage = NULL;
static int FDS_DiskSides = 0;

/* $4023 bit0: master enable for the disk registers and the sound unit */
static BYTE FDS_MasterEnable = 0;

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

  FDS_MasterEnable = 0;

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
  switch (wAddr)
  {
  case 0x4023: /* master I/O enable */
    FDS_MasterEnable = byData & 0x01;
    break;

  case 0x4025: /* FDS control */
    /* Only the mirroring bit belongs to Phase 2 (it is memory mapping).
       The motor / transfer / IRQ bits are Phase 3. */
    InfoNES_Mirroring((byData & 0x08) ? 0 : 1);
    break;

  default:
    /* $4020-$4022, $4024, $4026 and the sound unit at $4040-$408F
       are Phase 3 / Phase 6. */
    break;
  }
}

/*-------------------------------------------------------------------*/
/*  Mapper 20 Read from APU Function                                 */
/*-------------------------------------------------------------------*/
BYTE __not_in_flash_func(Map20_ReadApu)(WORD wAddr)
{
  switch (wAddr)
  {
  case 0x4030:
    /* Disk status: no transfer complete, no CRC error, drive idle. */
    return 0x00;

  case 0x4031:
    /* Read data port: nothing is being transferred yet. */
    return 0x00;

  case 0x4032:
    /* bit0 = disk not inserted, bit1 = drive not ready,
       bit2 = disk not writable.
       Phase 2 reports an empty drive on purpose: the BIOS then draws
       its boot screen and waits, which is exactly the milestone here.
       Phase 3 will drive these bits from the disk state machine. */
    return 0x07;

  case 0x4033:
    /* bit7 = battery good. Reporting a flat battery makes the BIOS
       refuse to boot, so this one does matter already. */
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
  /* Phase 3 hangs the disk transfer timing and the $4020-$4022 IRQ
     timer off this callback. */
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
