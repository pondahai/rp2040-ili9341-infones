#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/divider.h"
#include <hardware/spi.h>
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/i2c.h"
#include "hardware/interp.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include <hardware/sync.h>
#include <pico/multicore.h>
#include <hardware/flash.h>
#include <memory>
#include <math.h>
//#include <util/dump_bin.h>
// #include <util/exclusive_proc.h>
//#include <util/work_meter.h>
#include <string.h>
#include <stdarg.h>
#include <algorithm>

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_pAPU.h"

//#include <dvi/dvi.h>
#include <tusb.h>
// #include "gamepad.h"
#include "rom_selector.h"
#include "menu.h"

#ifdef __cplusplus

#include "ff.h"

#endif

// #include <hagl_hal.h>
// #include <hagl.h>
// #define ST7789
// #undef ILI9341

#define DCS_SOFT_RESET                 0x01
#define DCS_EXIT_SLEEP_MODE            0x11
#define DCS_EXIT_INVERT_MODE           0x20
#define DCS_ENTER_INVERT_MODE          0x21
#define DCS_SET_DISPLAY_ON             0x29
#define DCS_SET_COLUMN_ADDRESS         0x2A
#define DCS_SET_PAGE_ADDRESS           0x2B
#define DCS_WRITE_MEMORY_START         0x2C
#define DCS_SET_ADDRESS_MODE           0x36
#define DCS_SET_PIXEL_FORMAT           0x3A

#define DCS_PIXEL_FORMAT_16BIT         0x55 /* 0b01010101 */
#define DCS_PIXEL_FORMAT_8BIT          0x22 /* 0b00100010 */

#define DCS_ADDRESS_MODE_MIRROR_Y      0x80
#define DCS_ADDRESS_MODE_MIRROR_X      0x40
#define DCS_ADDRESS_MODE_SWAP_XY       0x20
#define DCS_ADDRESS_MODE_BGR           0x08
#define DCS_ADDRESS_MODE_RGB           0x00
#define DCS_ADDRESS_MODE_FLIP_X        0x02


#define    DISPLAY_SPI_CLOCK_SPEED_HZ 63000000

#define    DISPLAY_PIXEL_FORMAT DCS_PIXEL_FORMAT_16BIT

#ifdef ILI9341
#define    DISPLAY_ADDRESS_MODE DCS_ADDRESS_MODE_BGR | DCS_ADDRESS_MODE_SWAP_XY
#endif
#ifdef ST7789
#define    DISPLAY_ADDRESS_MODE DCS_ADDRESS_MODE_RGB | DCS_ADDRESS_MODE_SWAP_XY | DCS_ADDRESS_MODE_MIRROR_Y
#endif

#define    DISPLAY_OFFSET_X 0
#define    DISPLAY_OFFSET_Y 0


#define    DISPLAY_WIDTH 320
#define    DISPLAY_HEIGHT 240


#undef    DISPLAY_INVERT 


#include "audio.h"


#define PIN_UP 9
#define PIN_DN 5
#define PIN_LT 8
#define PIN_RT 6
#define PIN_SL 28
#define PIN_ST 4
#define PIN_A 2
#define PIN_B 3

const uint LED_PIN = PICO_DEFAULT_LED_PIN;

// static hagl_backend_t *display;
WORD scanline_buf_internal_1[320];
WORD scanline_buf_internal_2[320];
WORD scanline_buf_outgoing[320];
// BYTE framebuffer[256*240];
uint8_t screen_x;
uint8_t screen_x_start;
uint8_t screen_y;
bool line_drawing=false;
BYTE frame_skip;
BYTE frame_skip_counter=0;
int frame_column_step=0;
// #define FRAME_COLUMN_WIDTH 28
int FRAME_COLUMN_WIDTH=28;
#define AUDIO_BUF_SIZE 5000
BYTE snd_buf[AUDIO_BUF_SIZE]={0};
int buf_residue_size=AUDIO_BUF_SIZE;
static int display_dma_channel;

// #ifndef DVICONFIG
// //#define DVICONFIG dviConfig_PicoDVI
// #define DVICONFIG dviConfig_PicoDVISock
// #endif

#define ERRORMESSAGESIZE 40
#define GAMESAVEDIR "/SAVES"
// util::ExclusiveProc exclProc_;
char *ErrorMessage;
bool isFatalError = false;
static FATFS fs;
char *romName;
namespace
{
    constexpr uint32_t CPUFreqKHz = 252000;

//    constexpr dvi::Config dviConfig_PicoDVI = {
//        .pinTMDS = {10, 12, 14},
//        .pinClock = 8,
//        .invert = true,
//    };
//
//    constexpr dvi::Config dviConfig_PicoDVISock = {
//       .pinTMDS = {12, 18, 16},
//        .pinClock = 14,
//        .invert = false,
//    };

//    std::unique_ptr<dvi::DVI> dvi_;

    static constexpr uintptr_t NES_FILE_ADDR = 0x10080000;

   ROMSelector romSelector_;
   // util::ExclusiveProc exclProc_;

    enum class ScreenMode
    {
        SCANLINE_8_7,
        NOSCANLINE_8_7,
        SCANLINE_1_1,
        NOSCANLINE_1_1,
        MAX,
    };
    ScreenMode screenMode_{};

    bool scaleMode8_7_ = true;

    void applyScreenMode()
    {
        bool scanLine = false;

        switch (screenMode_)
        {
        case ScreenMode::SCANLINE_1_1:
            scaleMode8_7_ = false;
            scanLine = true;
            break;

        case ScreenMode::SCANLINE_8_7:
            scaleMode8_7_ = true;
            scanLine = true;
            break;

        case ScreenMode::NOSCANLINE_1_1:
            scaleMode8_7_ = false;
            scanLine = false;
            break;

        case ScreenMode::NOSCANLINE_8_7:
            scaleMode8_7_ = true;
            scanLine = false;
            break;
        }

        //dvi_->setScanLine(scanLine);
    }
}

// #define CC(x) (((x >> 1) & 15) | (((x >> 6) & 15) << 4) | (((x >> 11) & 15) << 8))
// #define CC(x) (((x>>12)&15))|(((x>>8)&15)<<4)|(((x>>4)&15)<<8)|(((x)&15)<<12)

// #define CC(x) ((((x >> 11) & 31) << 0) | (((x >> 5) & 63) << 5) | (((x >> 0) & 31) << 11))
#define CC(x) (x & 32767)
const WORD __not_in_flash_func(NesPalette)[64] = {
    /*
    CC(0x39ce), CC(0x1071), CC(0x0015), CC(0x2013), CC(0x440e), CC(0x5402), CC(0x5000), CC(0x3c20),
    CC(0x20a0), CC(0x0100), CC(0x0140), CC(0x00e2), CC(0x0ceb), CC(0x0000), CC(0x0000), CC(0x0000),
    CC(0x5ef7), CC(0x01dd), CC(0x10fd), CC(0x401e), CC(0x5c17), CC(0x700b), CC(0x6ca0), CC(0x6521),
    CC(0x45c0), CC(0x0240), CC(0x02a0), CC(0x0247), CC(0x0211), CC(0x0000), CC(0x0000), CC(0x0000),
    CC(0x7fff), CC(0x1eff), CC(0x2e5f), CC(0x223f), CC(0x79ff), CC(0x7dd6), CC(0x7dcc), CC(0x7e67),
    CC(0x7ae7), CC(0x4342), CC(0x2769), CC(0x2ff3), CC(0x03bb), CC(0x0000), CC(0x0000), CC(0x0000),
    CC(0x7fff), CC(0x579f), CC(0x635f), CC(0x6b3f), CC(0x7f1f), CC(0x7f1b), CC(0x7ef6), CC(0x7f75),
    CC(0x7f94), CC(0x73f4), CC(0x57d7), CC(0x5bf9), CC(0x4ffe), CC(0x0000), CC(0x0000), CC(0x0000),
*/

CC(0xAE73),CC(0xD120),CC(0x1500),CC(0x1340),CC(0x0E88),CC(0x02A8),CC(0x00A0),CC(0x4078),
CC(0x6041),CC(0x2002),CC(0x8002),CC(0xE201),CC(0xEB19),CC(0x0000),CC(0x0000),CC(0x0000),
CC(0xF7BD),CC(0x9D03),CC(0xDD21),CC(0x1E80),CC(0x17B8),CC(0x0BE0),CC(0x40D9),CC(0x61CA),
CC(0x808B),CC(0xA004),CC(0x4005),CC(0x8704),CC(0x1104),CC(0x0000),CC(0x0000),CC(0x0000),
CC(0xFFFF),CC(0xFF3D),CC(0xBF5C),CC(0x5FA4),CC(0xDFF3),CC(0xB6FB),CC(0xACFB),CC(0xC7FC),
CC(0xE7F5),CC(0x8286),CC(0xE94E),CC(0xD35F),CC(0x5B07),CC(0x0000),CC(0x0000),CC(0x0000),
CC(0xFFFF),CC(0x3FAF),CC(0xBFC6),CC(0x5FD6),CC(0x3FFE),CC(0x3BFE),CC(0xF6FD),CC(0xD5FE),
CC(0x34FF),CC(0xF4E7),CC(0x97AF),CC(0xF9B7),CC(0xFE9F),CC(0x0000),CC(0x0000),CC(0x0000),


};    


uint32_t getCurrentNVRAMAddr()
{

    if (!romSelector_.getCurrentROM())
    {
        return {};
    }
    int slot = romSelector_.getCurrentNVRAMSlot();
    if (slot < 0)
    {
        return {};
    }
    printf("SRAM slot %d\n", slot);
    return NES_FILE_ADDR - SRAM_SIZE * (slot + 1);

}


void saveNVRAM()
{
    if (!SRAMwritten)
    {
        printf("SRAM not updated.\n");
        return;
    }

    printf("save SRAM\n");
    // exclProc_.setProcAndWait([]
    //                          {
        static_assert((SRAM_SIZE & (FLASH_SECTOR_SIZE - 1)) == 0);
        if (auto addr = getCurrentNVRAMAddr())
        {
            auto ofs = addr - XIP_BASE;
            printf("write flash %x\n", ofs);
            {
                flash_range_erase(ofs, SRAM_SIZE);
                flash_range_program(ofs, SRAM, SRAM_SIZE);
            }
         } //});
    printf("done\n");

    SRAMwritten = false;
}

void loadNVRAM()
{
    if (auto addr = getCurrentNVRAMAddr())
    {
        printf("load SRAM %x\n", addr);
        memcpy(SRAM, reinterpret_cast<void *>(addr), SRAM_SIZE);
    }
    SRAMwritten = false;
}

extern int APU_Mute;

void InfoNES_PadState(DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem)
{
#if 0
    static constexpr int LEFT = 1 << 6;
    static constexpr int RIGHT = 1 << 7;
    static constexpr int UP = 1 << 4;
    static constexpr int DOWN = 1 << 5;
    static constexpr int SELECT = 1 << 2;
    static constexpr int START = 1 << 3;
    static constexpr int A = 1 << 0;
    static constexpr int B = 1 << 1;

    static DWORD prevButtons[2]{};
    static int rapidFireMask[2]{};
    static int rapidFireCounter = 0;

    ++rapidFireCounter;
    bool reset = false;

    for (int i = 0; i < 2; ++i)
    {
        auto &dst = i == 0 ? *pdwPad1 : *pdwPad2;
        auto &gp = io::getCurrentGamePadState(i);

        int v = (gp.buttons & io::GamePadState::Button::LEFT ? LEFT : 0) |
                (gp.buttons & io::GamePadState::Button::RIGHT ? RIGHT : 0) |
                (gp.buttons & io::GamePadState::Button::UP ? UP : 0) |
                (gp.buttons & io::GamePadState::Button::DOWN ? DOWN : 0) |
                (gp.buttons & io::GamePadState::Button::A ? A : 0) |
                (gp.buttons & io::GamePadState::Button::B ? B : 0) |
                (gp.buttons & io::GamePadState::Button::SELECT ? SELECT : 0) |
                (gp.buttons & io::GamePadState::Button::START ? START : 0) |
                0;

        int rv = v;
        if (rapidFireCounter & 2)
        {
            // 15 fire/sec
            rv &= ~rapidFireMask[i];
        }

        dst = rv;

        auto p1 = v;
        auto pushed = v & ~prevButtons[i];
        if (p1 & SELECT)
        {
            if (pushed & LEFT)
            {
                saveNVRAM();
                romSelector_.prev();
                reset = true;
            }
            if (pushed & RIGHT)
            {
                saveNVRAM();
                romSelector_.next();
                reset = true;
            }
            if (pushed & START)
            {
                saveNVRAM();
                reset = true;
            }
            if (pushed & A)
            {
                rapidFireMask[i] ^= io::GamePadState::Button::A;
            }
            if (pushed & B)
            {
                rapidFireMask[i] ^= io::GamePadState::Button::B;
            }
            if (pushed & UP)
            {
                screenMode_ = static_cast<ScreenMode>((static_cast<int>(screenMode_) - 1) & 3);
                applyScreenMode();
            }
            else if (pushed & DOWN)
            {
                screenMode_ = static_cast<ScreenMode>((static_cast<int>(screenMode_) + 1) & 3);
                applyScreenMode();
            }
        }

        prevButtons[i] = v;
    }

    *pdwSystem = reset ? PAD_SYS_QUIT : 0;
#endif
    static constexpr int _LEFT = 1 << 6;
    static constexpr int _RIGHT = 1 << 7;
    static constexpr int _UP = 1 << 4;
    static constexpr int _DOWN = 1 << 5;
    static constexpr int _SELECT = 1 << 2;
    static constexpr int _START = 1 << 3;
    static constexpr int _AA = 1 << 0;
    static constexpr int _BB = 1 << 1;

    static DWORD prevButtons[2]{};
    static int rapidFireMask[2]{};
    static int rapidFireCounter = 0;

    ++rapidFireCounter;
    bool reset = false;
    
    for (int i = 0; i < 2; ++i){

    
    auto &dst = i == 0 ? *pdwPad1 : *pdwPad2;
    int v=0;
    if (gpio_get(PIN_A)==0)      v |= _AA;
    if (gpio_get(PIN_B)==0)      v |= _BB;
    if (gpio_get(PIN_ST)==0)      v |= _START;
    if (gpio_get(PIN_SL)==0)      v |= _SELECT;
    if (gpio_get(PIN_LT)==0)      v |= _LEFT;
    if (gpio_get(PIN_RT)==0)      v |= _RIGHT;
    if (gpio_get(PIN_UP)==0)      v |= _UP;
    if (gpio_get(PIN_DN)==0)      v |= _DOWN;

    int rv = v;
        if (rapidFireCounter & 2)
        {
            // 15 fire/sec
            rv &= ~rapidFireMask[i];
        }

        dst = rv;
        auto p1 = v;
        auto pushed = v & ~prevButtons[i];
        if (p1 & _SELECT)
        {
            if (pushed & _LEFT)
            {
                saveNVRAM();
                romSelector_.prev();
                reset = true;
            }
            if (pushed & _RIGHT)
            {
                saveNVRAM();
                romSelector_.next();
                reset = true;
            }
            if (pushed & _START)
            {
                saveNVRAM();
                reset = true;
            }
            if (pushed & _AA)
            {
                rapidFireMask[i] ^= _AA;
            }
            if (pushed & _BB)
            {
                rapidFireMask[i] ^= _BB;
            }
            if (pushed & _UP)
            {
                APU_Mute = 0;

                // screenMode_ = static_cast<ScreenMode>((static_cast<int>(screenMode_) - 1) & 3);
                // applyScreenMode();
            }
            else if (pushed & _DOWN)
            {
                APU_Mute = 1;

                // screenMode_ = static_cast<ScreenMode>((static_cast<int>(screenMode_) + 1) & 3);
                // applyScreenMode();
            }
        }

        prevButtons[i] = *pdwPad1;
    }
    *pdwSystem = reset ? PAD_SYS_QUIT : 0;
}

void InfoNES_MessageBox(const char *pszMsg, ...)
{
    printf("[MSG]");
    va_list args;
    va_start(args, pszMsg);
    vprintf(pszMsg, args);
    va_end(args);
    printf("\n");
}

bool parseROM(const uint8_t *nesFile)
{

    memcpy(&NesHeader, nesFile, sizeof(NesHeader));
    if (!checkNESMagic(NesHeader.byID))
    {
        return false;
    }

    nesFile += sizeof(NesHeader);

    memset(SRAM, 0, SRAM_SIZE);

    if (NesHeader.byInfo1 & 4)
    {
        memcpy(&SRAM[0x1000], nesFile, 512);
        nesFile += 512;
    }

    auto romSize = NesHeader.byRomSize * 0x4000;
    ROM = (BYTE *)nesFile;
    nesFile += romSize;

    if (NesHeader.byVRomSize > 0)
    {
        auto vromSize = NesHeader.byVRomSize * 0x2000;
        VROM = (BYTE *)nesFile;
        nesFile += vromSize;
    }

    return true;
}

void InfoNES_ReleaseRom()
{
    ROM = nullptr;
    VROM = nullptr;
}

void InfoNES_SoundInit()
{
}

int InfoNES_SoundOpen(int samples_per_sync, int sample_rate)
{
    return 0;
}

void InfoNES_SoundClose()
{
}

int __not_in_flash_func(InfoNES_GetSoundBufferSize)()
{
   // return dvi_->getAudioRingBuffer().getFullWritableSize();
    return 128;
}

/*
 *  call from InfoNES_pAPUHsync
 */
void __not_in_flash_func(InfoNES_SoundOutput)(int samples, BYTE *wave1, BYTE *wave2, BYTE *wave3, BYTE *wave4, BYTE *wave5)
{
    static int test_i=0;

    while (samples)
    {
        // auto &ring = dvi_->getAudioRingBuffer();
        // auto n = std::min<int>(samples, ring.getWritableSize());
        auto n = std::min<int>(samples, buf_residue_size);
        // auto n = samples;
        if (!n)
        {
            return;
        }
        // auto p = ring.getWritePointer();
        auto p = &snd_buf[AUDIO_BUF_SIZE-buf_residue_size];

        int ct = n;
        while (ct--)
        {
            uint8_t w1 = *wave1++;
            uint8_t w2 = *wave2++;
            uint8_t w3 = *wave3++;
            uint8_t w4 = *wave4++;
            uint8_t w5 = *wave5++;
            //            w3 = w2 = w4 = w5 = 0;
            // int l = w1 * 6 + w2 * 3 + w3 * 5 + w4 * 3 * 17 + w5 * 2 * 32;
            // int r = w1 * 3 + w2 * 6 + w3 * 5 + w4 * 3 * 17 + w5 * 2 * 32;
            // *p++ = {static_cast<short>(l), static_cast<short>(r)};

            // *p++ =  w1 * 6 + w2 * 6  + w3 * 5 + w4 * 3 * 17 + w5 * 2 * 32 ;
            *p++ =  (w1 * 4 + w2 * 6  + w3 * .1  + w4 * .1 + w5 * 3 ) * 1;
            // *p++ = snd_drum[test_i++];
            // if(test_i > sizeof(snd_drum)) test_i = 0;


            // pulse_out = 0.00752 * (pulse1 + pulse2)
            // tnd_out = 0.00851 * triangle + 0.00494 * noise + 0.00335 * dmc

            // 0.00851/0.00752 = 1.131648936170213
            // 0.00494/0.00752 = 0.6569148936170213
            // 0.00335/0.00752 = 0.4454787234042554

            // 0.00752/0.00851 = 0.8836662749706228
            // 0.00494/0.00851 = 0.5804935370152762
            // 0.00335/0.00851 = 0.3936545240893067
        }

        // ring.advanceWritePointer(n);
        samples -= n;
        buf_residue_size -= n;
        if(buf_residue_size <= 0) buf_residue_size = AUDIO_BUF_SIZE;
        
    }

}


extern WORD PC;


////
/*
 *
 */
static void display_write_command(const uint8_t command)
{
    /* Set DC low to denote incoming command. */
    gpio_put(DISPLAY_PIN_DC, 0);

    /* Set CS low to reserve the SPI bus. */
    gpio_put(DISPLAY_PIN_CS, 0);

    spi_write_blocking(DISPLAY_SPI_PORT, &command, 1);

    /* Set CS high to ignore any traffic on SPI bus. */
    gpio_put(DISPLAY_PIN_CS, 1);
}

static void display_write_data(const uint8_t *data, size_t length)
{
    size_t sent = 0;

    if (0 == length) {
        return;
    };

    /* Set DC high to denote incoming data. */
    gpio_put(DISPLAY_PIN_DC, 1);

    /* Set CS low to reserve the SPI bus. */
    gpio_put(DISPLAY_PIN_CS, 0);

    spi_write_blocking(DISPLAY_SPI_PORT, data, length);

    /* Set CS high to ignore any traffic on SPI bus. */
    gpio_put(DISPLAY_PIN_CS, 1);
}
 void __not_in_flash_func(display_set_address)(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    uint8_t command;
    uint8_t data[4];
    static uint16_t prev_x1, prev_x2, prev_y1, prev_y2;

    x1 = x1 + DISPLAY_OFFSET_X;
    y1 = y1 + DISPLAY_OFFSET_Y;
    x2 = x2 + DISPLAY_OFFSET_X;
    y2 = y2 + DISPLAY_OFFSET_Y;

    /* Change column address only if it has changed. */
    if ((prev_x1 != x1 || prev_x2 != x2)) {
        display_write_command(DCS_SET_COLUMN_ADDRESS);
        data[0] = x1 >> 8;
        data[1] = x1 & 0xff;
        data[2] = x2 >> 8;
        data[3] = x2 & 0xff;
        display_write_data(data, 4);

        prev_x1 = x1;
        prev_x2 = x2;
    }

    /* Change page address only if it has changed. */
    if ((prev_y1 != y1 || prev_y2 != y2)) {
        display_write_command(DCS_SET_PAGE_ADDRESS);
        data[0] = y1 >> 8;
        data[1] = y1 & 0xff;
        data[2] = y2 >> 8;
        data[3] = y2 & 0xff;
        display_write_data(data, 4);

        prev_y1 = y1;
        prev_y2 = y2;
    }
    // 
    display_write_command(DCS_WRITE_MEMORY_START);
}
////
/*
 *  setting column and page, start and stop
 */
void ili9341_infones_frame_timing_register_init()
{
        uint8_t command;
        uint8_t data[4];
        int x=0;



#if 0
        display_set_address(x+((320-256)/2), 4, (x+((320-256)/2)+FRAME_COLUMN_WIDTH-1), (240-4-1));
#endif
        display_set_address(x+((320-256)/2), 4, (x+((320-256)/2)+256-1), (240-4-1));

////
        /*
         *   keep chip select active, let the next data be written continuously
         */
        gpio_put(DISPLAY_PIN_DC, 1);
        gpio_put(DISPLAY_PIN_CS, 0);

}
void st7789_infones_frame_timing_register_init()
{
        uint8_t command;
        uint8_t data[4];
        int x=0;



        display_set_address(x+((160-128)/2), 4/2, (x+((160-128)/2)+128-1), (240-4-1)/2);

////
        /*
         *   keep chip select active, let the next data be written continuously
         */
        gpio_put(DISPLAY_PIN_DC, 1);
        gpio_put(DISPLAY_PIN_CS, 0);

}

void __not_in_flash_func(core1_main)()
{
#if 0
    while (true)
    {
        dvi_->registerIRQThisCore();
        dvi_->waitForValidLine();

        dvi_->start();
        while (!exclProc_.isExist())
        {
            if (scaleMode8_7_)
            {
                dvi_->convertScanBuffer12bppScaled16_7(34, 32, 288 * 2);
                // 34 + 252 + 34
                // 32 + 576 + 32
            }
            else
            {
                dvi_->convertScanBuffer12bpp();
            }
        }

        dvi_->unregisterIRQThisCore();
        dvi_->stop();

        exclProc_.processOrWaitIfExist();
    }
#endif

    while(true){
    //     if(line_drawing){

    //     for(int x=0;x<256;x++){
    //         hagl_put_pixel(display,screen_y,x+((320-256)/2),scanline_buf_outgoing[x]);
    //     }

        /*
         *   (1/22050) * 735 = 33333us
         */
        sleep_us(12850);


        /*
         *   sound process
         */
            
            

            // int id = audio_play_once(snd_buf,735*2);








    //     line_drawing = false;
    // }
        // for(int y=0;y<240;y++)
        //     for(int x=0;x<256;x++){
        //         hagl_put_pixel(display,y,x+((320-256)/2),framebuffer[x+(256*y)]);
        //     }
        // if(audio_step_counter++>153){
            // audio_step_counter=0;
            // audio_mixer_step();
        // }
        // sleep_us(130);
    } // while
}



static void __not_in_flash_func(blink_led)(void)
{
  static uint64_t last_blink = 0;

// frame timing control
  uint64_t cur_time = time_us_64();
  uint64_t diff_time = cur_time - last_blink;
  while (last_blink + 16666 > cur_time) {cur_time = time_us_64();}
    gpio_xor_mask(1<<LED_PIN);
    last_blink = cur_time;
      
  // uint64_t cur_time = time_us_64();
  // if (last_blink + 16666 < cur_time) {
  //   gpio_xor_mask(1<<LED_PIN);
  //   last_blink = cur_time;
  //   if(frame_column_step==0 && FRAME_COLUMN_WIDTH>0) FRAME_COLUMN_WIDTH--;
  // }else{
  //   if(frame_column_step==0 && FRAME_COLUMN_WIDTH<256) FRAME_COLUMN_WIDTH++;
  // }
}

uint32_t FrameCounter=0;
uint16_t test_color_bar = 0;
/*
 *  call from InfoNES_HSync() 
 *  in every frame
 */
int __not_in_flash_func(InfoNES_LoadFrame)()
{
#if 0
    gpio_put(LED_PIN, hw_divider_s32_quotient_inlined(dvi_->getFrameCounter(), 60) & 1);
    //    printf("%04x\n", PC);

    tuh_task();
#endif
/*
 *
 */
    blink_led();

/*
 *
 */
    frame_skip = true;
    if(frame_skip_counter++ == 2){
        frame_skip_counter = 0;
        frame_skip = false;
        // if(frame_skip == false) frame_skip = true;
        // else frame_skip = false;
            
        /*
         *   sound process : 735 samples per frame
         */  

    }
    // (AUDIO_BUF_SIZE-buf_residue_size)
    if(frame_skip_counter == 0){
        int j=0;
        for(int i=0;i<(AUDIO_BUF_SIZE-buf_residue_size);i+=1,j++){
            snd_buf[j]=snd_buf[i];
        }
        // snd_buf[j++]=snd_buf[(AUDIO_BUF_SIZE-buf_residue_size)];
        audio_play_once(snd_buf,j);
        audio_mixer_step();
        buf_residue_size = AUDIO_BUF_SIZE;
    }

/*
 *
 */
#if 0    
    frame_column_step += FRAME_COLUMN_WIDTH;
    test_color_bar = NesPalette[frame_column_step&63];
    if(frame_column_step > 256) frame_column_step = 0;
#endif

    /*
     *   setting frame display column
     *
     *   only column change, page remains the same.
     *
     */

    //     for(int x=0;x<256;x+=1){

    //         hagl_put_pixel(display,x+((320-256)/2),screen_y,scanline_buf_internal[x]);
    //     }
    // return;

#if 0
        uint8_t command;
        uint8_t data[4];
        int x=0;

//// DCS_SET_COLUMN_ADDRESS
                gpio_put(DISPLAY_PIN_DC, 0);

                /* Set CS low to reserve the SPI bus. */
                gpio_put(DISPLAY_PIN_CS, 0);

                command = DCS_SET_COLUMN_ADDRESS;
                spi_write_blocking(DISPLAY_SPI_PORT, &command, 1);

                /* Set CS high to ignore any traffic on SPI bus. */
                gpio_put(DISPLAY_PIN_CS, 1);
////
                /* Set DC high to denote incoming data. */
                gpio_put(DISPLAY_PIN_DC, 1);

                /* Set CS low to reserve the SPI bus. */
                gpio_put(DISPLAY_PIN_CS, 0);

                int x_width_end = (frame_column_step+FRAME_COLUMN_WIDTH > 256)?256:frame_column_step+FRAME_COLUMN_WIDTH;
                data[0] = x+((320-256)/2)+frame_column_step >> 8;
                data[1] = x+((320-256)/2)+frame_column_step & 0xff;
                data[2] = (x+((320-256)/2)+x_width_end-1) >> 8;
                data[3] = (x+((320-256)/2)+x_width_end-1) & 0xff;
                spi_write_blocking(DISPLAY_SPI_PORT, data, 4);

                /* Set CS high to ignore any traffic on SPI bus. */
                gpio_put(DISPLAY_PIN_CS, 1);


//// DCS_WRITE_MEMORY_START
                gpio_put(DISPLAY_PIN_DC, 0);

                /* Set CS low to reserve the SPI bus. */
                gpio_put(DISPLAY_PIN_CS, 0);

                command = DCS_WRITE_MEMORY_START;
                spi_write_blocking(DISPLAY_SPI_PORT, &command, 1);

                /* Set CS high to ignore any traffic on SPI bus. */
                gpio_put(DISPLAY_PIN_CS, 1);


                gpio_put(DISPLAY_PIN_DC, 1);
                gpio_put(DISPLAY_PIN_CS, 0);

#endif



    return FrameCounter++;
}
#if 0
namespace
{
    dvi::DVI::LineBuffer *currentLineBuffer_{};
}

void __not_in_flash_func(drawWorkMeterUnit)(int timing,
                                            [[maybe_unused]] int span,
                                            uint32_t tag)
{
    if (timing >= 0 && timing < 640)
    {
        auto p = currentLineBuffer_->data();
        p[timing] = tag; // tag = color
    }
}

void __not_in_flash_func(drawWorkMeter)(int line)
{
    if (!currentLineBuffer_)
    {
        return;
    }

    memset(currentLineBuffer_->data(), 0, 64);
    memset(&currentLineBuffer_->data()[320 - 32], 0, 64);
    (*currentLineBuffer_)[160] = 0;
    if (line == 4)
    {
        for (int i = 1; i < 10; ++i)
        {
            (*currentLineBuffer_)[16 * i] = 31;
        }
    }

    constexpr uint32_t clocksPerLine = 800 * 10;
    constexpr uint32_t meterScale = 160 * 65536 / (clocksPerLine * 2);
    util::WorkMeterEnum(meterScale, 1, drawWorkMeterUnit);
    //    util::WorkMeterEnum(160, clocksPerLine * 2, drawWorkMeterUnit);
}
#endif

void __not_in_flash_func(RomSelect_PreDrawLine)(int line)
{
    if(line % 2 == 0){
        RomSelect_SetLineBuffer(scanline_buf_internal_1, 256);
    }else{
        RomSelect_SetLineBuffer(scanline_buf_internal_2, 256);
    }
}

/*
 *  InfoNES_PreDrawLine and 
 *  InfoNES_PostDrawLine
 *  
 *   call from InfoNES_HSync() 
 *  on every scanline 
 */
void __not_in_flash_func(InfoNES_PreDrawLine)(int line)
{
#if 0
    util::WorkMeterMark(0xaaaa);
    auto b = dvi_->getLineBuffer();
    util::WorkMeterMark(0x5555);
    InfoNES_SetLineBuffer(b->data() + 32, b->size());
    //    (*b)[319] = line + dvi_->getFrameCounter();

    currentLineBuffer_ = b;
#endif
    if(line % 2 == 0){
        InfoNES_SetLineBuffer(scanline_buf_internal_1, 256);
    }else{
        InfoNES_SetLineBuffer(scanline_buf_internal_2, 256);
    }
}

void __not_in_flash_func(InfoNES_PostDrawLine)(int line)
{
#if 0
#if !defined(NDEBUG)
    util::WorkMeterMark(0xffff);
    drawWorkMeter(line);
#endif

    assert(currentLineBuffer_);
    dvi_->setLineBuffer(line, currentLineBuffer_);
    currentLineBuffer_ = nullptr;
#endif
//     #define screen_x_step 4
// if(line == 4){
//     screen_x_start+=screen_x_step;
//     if(screen_x_start>320) screen_x_start=0;
//     screen_x=screen_x_start;
// }
// for(int i=0;i<screen_x_step;i++){
//             hagl_put_pixel(display,line,screen_x+((320-256)/2),scanline_buf_internal[screen_x]);
        
//             screen_x++;
//             if(screen_x>320){ 
//                 screen_x=0;
//             }
//          }

/*
 *  frame skip
 */
// if(frame_skip) return;


        screen_y = line;

    // if(line_drawing==false){

        // if(line_drawing == false){
        // screen_y = line;
        // __builtin_memcpy(scanline_buf_outgoing,scanline_buf_internal,sizeof(WORD)*256);
        // sleep_us(100);
        // line_drawing = true;
        // }

        // __builtin_memcpy(framebuffer+(sizeof(BYTE)*line),scanline_buf_internal,sizeof(BYTE)*256);
    // if(line == 4){ 
    //     frame_skip--;
    //     if(frame_skip<0) frame_skip=0;
    // }

        /*
         *  each scanline only display partial column 
         *  spi_write is here, also contorl the speed of frame rate
         *  less the FRAME_COLUMN_WIDTH, speed up  the frame rate
         */ 

        // uint8_t command;
        // uint8_t data[4];

// Continue Write
            // display_write_data(&scanline_buf_internal[x], 2);
                /* Set DC high to denote incoming data. */
                // gpio_put(DISPLAY_PIN_DC, 1);

                /* Set CS low to reserve the SPI bus. */
                // gpio_put(DISPLAY_PIN_CS, 0);
#if 0
            for(int x=frame_column_step;x<frame_column_step+FRAME_COLUMN_WIDTH && x<256;x+=1){
                // data[1] = scanline_buf_internal[x] >> 8;
                // data[0] = scanline_buf_internal[x] & 0xff;

                spi_write_blocking(DISPLAY_SPI_PORT, (uint8_t *)&scanline_buf_internal[x], 2);

                // spi_write_blocking(DISPLAY_SPI_PORT, (uint8_t *)&test_color_bar, 2);
            }
#endif
#if 0
                 spi_write_blocking(DISPLAY_SPI_PORT, (uint8_t *)&scanline_buf_internal[frame_column_step], FRAME_COLUMN_WIDTH*2);
                 // spi_write_blocking(DISPLAY_SPI_PORT, (uint8_t *)&test_color_bar, FRAME_COLUMN_WIDTH*2);
#endif
#if 0
                 spi_write_blocking(DISPLAY_SPI_PORT, (uint8_t *)scanline_buf_internal, 256*2);
#endif
    WORD *fb;
    if(line % 2 == 0){
        fb = scanline_buf_internal_1;
    }else{
        fb = scanline_buf_internal_2;
    }        
#ifdef ILI9341
                dma_channel_wait_for_finish_blocking(display_dma_channel);
        // memcpy(scanline_buf_outgoing,scanline_buf_internal,sizeof(scanline_buf_outgoing));
                dma_channel_set_trans_count(display_dma_channel, 256*2, false);
                dma_channel_set_read_addr(display_dma_channel, fb, true);   
                // dma_channel_wait_for_finish_blocking(display_dma_channel);             
// static uint32_t frame_counter=0;
//         if(screen_y == 4){
//             display_set_address(0+((320-256)/2), 4, (0+((320-256)/2)+256-1), (120+4-1));
//             gpio_put(DISPLAY_PIN_DC, 1);
//             gpio_put(DISPLAY_PIN_CS, 0);
//         }
//         if(screen_y == 120+4){
//             display_set_address(0+((320-256)/2), 120+4, (0+((320-256)/2)+256-1), (240-4-1));
//             gpio_put(DISPLAY_PIN_DC, 1);
//             gpio_put(DISPLAY_PIN_CS, 0);
//         }
//         if(screen_y < (120+4) ){
//             if(frame_counter % 2 == 0){
//                 dma_channel_wait_for_finish_blocking(display_dma_channel);
//                 dma_channel_set_trans_count(display_dma_channel, 256*2, false);
//                 dma_channel_set_read_addr(display_dma_channel, (uint8_t *)scanline_buf_internal, true);   
//                 dma_channel_wait_for_finish_blocking(display_dma_channel);             
//             }
//         }
//         else{
//             if(frame_counter % 2 == 1){
//                 dma_channel_wait_for_finish_blocking(display_dma_channel);
//                 dma_channel_set_trans_count(display_dma_channel, 256*2, false);
//                 dma_channel_set_read_addr(display_dma_channel, (uint8_t *)scanline_buf_internal, true);   
//                 dma_channel_wait_for_finish_blocking(display_dma_channel);    
//             }         
//         }
//         if(screen_y == 240-4-1){
//             frame_counter++;
//         }
#endif
#ifdef ST7789
    if(screen_y % 2 == 0){
        dma_channel_wait_for_finish_blocking(display_dma_channel);
        int j=0;
        for(int i=0;i<256;i+=2,j++){
            scanline_buf_outgoing[j] = fb[i];
        } 
        dma_channel_set_trans_count(display_dma_channel, 128*2, false);
        dma_channel_set_read_addr(display_dma_channel, scanline_buf_outgoing, true);   
        // dma_channel_wait_for_finish_blocking(display_dma_channel);             
    }
#endif
                /* Set CS high to ignore any traffic on SPI bus. */
                // gpio_put(DISPLAY_PIN_CS, 1);



    // } // for
}

bool loadAndReset()
{

    auto rom = romSelector_.getCurrentROM();
    if (!rom)
    {
        printf("ROM does not exists.\n");
        return false;
    }

    if (!parseROM(rom))
    {
        printf("NES file parse error.\n");
        return false;
    }
    loadNVRAM();

    if (InfoNES_Reset() < 0)
    {
        printf("NES reset error.\n");
        return false;
    }

    return true;

}

int InfoNES_Menu()
{
    // InfoNES_Main() のループで最初に呼ばれる
    loadAndReset();
    return 0;
}



static void key_init() {
    gpio_init(PIN_UP);
    gpio_pull_up(PIN_UP);
    gpio_set_dir(PIN_UP, GPIO_IN);
    gpio_init(PIN_DN);
    gpio_pull_up(PIN_DN);
    gpio_set_dir(PIN_DN, GPIO_IN);
    gpio_init(PIN_LT);
    gpio_pull_up(PIN_LT);
    gpio_set_dir(PIN_LT, GPIO_IN);
    gpio_init(PIN_RT);
    gpio_pull_up(PIN_RT);
    gpio_set_dir(PIN_RT, GPIO_IN);
    gpio_init(PIN_ST);
    gpio_pull_up(PIN_ST);
    gpio_set_dir(PIN_ST, GPIO_IN);
    gpio_init(PIN_SL);
    gpio_pull_up(PIN_SL);
    gpio_set_dir(PIN_SL, GPIO_IN);
    gpio_init(PIN_A);
    gpio_pull_up(PIN_A);
    gpio_set_dir(PIN_A, GPIO_IN);
    gpio_init(PIN_B);
    gpio_pull_up(PIN_B);
    gpio_set_dir(PIN_B, GPIO_IN);
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);

}








static void display_spi_master_init()
{
    // https://github.com/Bodmer/TFT_eSPI/discussions/2432
// Get the processor sys_clk frequency in Hz
 uint32_t freq = clock_get_hz(clk_sys);

 // clk_peri does not have a divider, so input and output frequencies will be the same
 clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    freq,
                    freq);


    gpio_set_function(DISPLAY_PIN_DC, GPIO_FUNC_SIO);
    gpio_set_dir(DISPLAY_PIN_DC, GPIO_OUT);

    gpio_set_function(DISPLAY_PIN_CS, GPIO_FUNC_SIO);
    gpio_set_dir(DISPLAY_PIN_CS, GPIO_OUT);

    gpio_set_function(DISPLAY_PIN_CLK,  GPIO_FUNC_SPI);
    gpio_set_function(DISPLAY_PIN_MOSI, GPIO_FUNC_SPI);

    if (DISPLAY_PIN_MISO > 0) {
        gpio_set_function(DISPLAY_PIN_MISO, GPIO_FUNC_SPI);
    }

    /* Set CS high to ignore any traffic on SPI bus. */
    gpio_put(DISPLAY_PIN_CS, 1);

    spi_init(DISPLAY_SPI_PORT, DISPLAY_SPI_CLOCK_SPEED_HZ);

    uint32_t baud = spi_set_baudrate(DISPLAY_SPI_PORT, DISPLAY_SPI_CLOCK_SPEED_HZ);
    uint32_t peri = clock_get_hz(clk_peri);
    uint32_t sys = clock_get_hz(clk_sys);

// DMA init
    display_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config channel_config = dma_channel_get_default_config(display_dma_channel);
    channel_config_set_transfer_data_size(&channel_config, DMA_SIZE_8);
    if (spi0 == DISPLAY_SPI_PORT) {
        channel_config_set_dreq(&channel_config, DREQ_SPI0_TX);
    } else {
        channel_config_set_dreq(&channel_config, DREQ_SPI1_TX);
    }
    dma_channel_set_config(display_dma_channel, &channel_config, false);
    dma_channel_set_write_addr(display_dma_channel, &spi_get_hw(DISPLAY_SPI_PORT)->dr, false);

}

void display_init()
{

    /* Init the spi driver. */
    display_spi_master_init();
    sleep_ms(100);

    /* Reset the display. */
    if (DISPLAY_PIN_RST > 0) {
        gpio_set_function(DISPLAY_PIN_RST, GPIO_FUNC_SIO);
        gpio_set_dir(DISPLAY_PIN_RST, GPIO_OUT);

        gpio_put(DISPLAY_PIN_RST, 0);
        sleep_ms(100);
        gpio_put(DISPLAY_PIN_RST, 1);
        sleep_ms(100);
    }

    /* Send minimal init commands. */
    display_write_command(DCS_SOFT_RESET);
    sleep_ms(200);

    display_write_command(DCS_SET_ADDRESS_MODE);
    uint8_t mode1 = DISPLAY_ADDRESS_MODE;
    display_write_data(&mode1, 1);

    display_write_command(DCS_SET_PIXEL_FORMAT);
    uint8_t mode2 = DISPLAY_PIXEL_FORMAT;
    display_write_data(&mode2, 1);

#ifdef DISPLAY_INVERT
    display_write_command(DCS_ENTER_INVERT_MODE);

#else
    display_write_command(DCS_EXIT_INVERT_MODE);
#endif

    display_write_command(DCS_EXIT_SLEEP_MODE);
    sleep_ms(200);

    display_write_command(DCS_SET_DISPLAY_ON);
    sleep_ms(200);

    /* Enable backlight */
    if (DISPLAY_PIN_BL > 0) {
        gpio_set_function(DISPLAY_PIN_BL, GPIO_FUNC_SIO);
        gpio_set_dir(DISPLAY_PIN_BL, GPIO_OUT);

        gpio_put(DISPLAY_PIN_BL, 1);
    }

    /* Set the default viewport to full screen. */
    display_set_address(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);


}
void display_clear()
{
#ifdef ILI9341
    display_set_address(0,0,320-1,240-1);
    BYTE pixel[2]={0x00,0x00};
    for(int i=0;i<320*240;i+=1){
        display_write_data(pixel,2);
    }
#endif
#ifdef ST7789
    display_set_address(0,0,160-1,128-1);
    BYTE pixel[2]={0x00,0x00};
    for(int i=0;i<160*128;i+=1){
        display_write_data(pixel,2);
    }
#endif
  

}

bool initSDCard()
{
    FRESULT fr;
    TCHAR str[40];
    sleep_ms(1000);

    printf("Mounting SDcard");
    fr = f_mount(&fs, "", 1);
    if (fr != FR_OK)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "SD card mount error: %d", fr);
        printf("%s\n", ErrorMessage);
        return false;
    }
    printf("\n");

    fr = f_chdir("/");
    if (fr != FR_OK)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot change dir to / : %d", fr);
        printf("%s\n", ErrorMessage);
        return false;
    }
    // for f_getcwd to work, set
    //   #define FF_FS_RPATH        2
    // in drivers/fatfs/ffconf.h
    fr = f_getcwd(str, sizeof(str));
    if (fr != FR_OK)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot get current dir: %d", fr);
        printf("%s\n", ErrorMessage);
        return false;
    }
    printf("Current directory: %s\n", str);
    printf("Creating directory %s\n", GAMESAVEDIR);
    fr = f_mkdir(GAMESAVEDIR);
    if (fr != FR_OK)
    {
        if (fr == FR_EXIST)
        {
            printf("Directory already exists.\n");
        }
        else
        {
            snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot create dir %s: %d", GAMESAVEDIR, fr);
            printf("%s\n", ErrorMessage);
            return false;
        }
    }
    return true;
}

int main()
{
    char selectedRom[80];
    romName = selectedRom;
    char errMSG[ERRORMESSAGESIZE];
    errMSG[0] = selectedRom[0] = 0;
    ErrorMessage = errMSG;

    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(100);
    set_sys_clock_khz(CPUFreqKHz, true);


    stdio_init_all();
    key_init();

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);

    // display = hagl_init();
    // hagl_clear(display);

    display_init(); 
    display_clear();
#ifdef ILI9341
    ili9341_infones_frame_timing_register_init();
#endif
#ifdef ST7789
    st7789_infones_frame_timing_register_init();
    APU_Mute = 0;
#endif

    line_drawing=false;

    //
    // 
    // play samples in core1
    //
    // 
    // 735 samples per frame
    //
    audio_init(7,19654);


    //tusb_init();

    romSelector_.init(NES_FILE_ADDR);


    // util::dumpMemory((void *)NES_FILE_ADDR, 1024);

#if 0
    //
    auto *i2c = i2c0;
    static constexpr int I2C_SDA_PIN = 16;
    static constexpr int I2C_SCL_PIN = 17;
    i2c_init(i2c, 100 * 1000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    // gpio_pull_up(I2C_SDA_PIN);
    // gpio_pull_up(I2C_SCL_PIN);
    i2c_set_slave_mode(i2c, false, 0);

    {
        constexpr int addrSegmentPointer = 0x60 >> 1;
        constexpr int addrEDID = 0xa0 >> 1;
        constexpr int addrDisplayID = 0xa4 >> 1;

        uint8_t buf[128];
        int addr = 0;
        do
        {
            printf("addr: %04x\n", addr);
            uint8_t tmp = addr >> 8;
            i2c_write_blocking(i2c, addrSegmentPointer, &tmp, 1, false);

            tmp = addr & 255;
            i2c_write_blocking(i2c, addrEDID, &tmp, 1, true);
            i2c_read_blocking(i2c, addrEDID, buf, 128, false);

            util::dumpMemory(buf, 128);
            printf("\n");

            addr += 128;
        } while (buf[126]); 
    }
#endif
#if 0
    //
    dvi_ = std::make_unique<dvi::DVI>(pio0, &DVICONFIG,
                                      dvi::getTiming640x480p60Hz());
    //    dvi_->setAudioFreq(48000, 25200, 6144);
    dvi_->setAudioFreq(44100, 28000, 6272);
    dvi_->allocateAudioBuffer(256);
    //    dvi_->setExclusiveProc(&exclProc_);

    dvi_->getBlankSettings().top = 4 * 2;
    dvi_->getBlankSettings().bottom = 4 * 2;
    // dvi_->setScanLine(true);

    applyScreenMode();

    // 空サンプル詰めとく
    dvi_->getAudioRingBuffer().advanceWritePointer(255);
#endif
    // multicore_launch_core1(core1_main);

    // InfoNES_Main();

    isFatalError = !initSDCard();
    // When a game is started from the menu, the menu will reboot the device.
    // After reboot the emulator will start the selected game.
    if (watchdog_caused_reboot() && isFatalError == false)
    {
        // Determine loaded rom
        printf("Rebooted by menu\n");
        FIL fil;
        FRESULT fr;
        size_t tmpSize;
        printf("Reading current game from %s and starting emulator\n", ROMINFOFILE);
        fr = f_open(&fil, ROMINFOFILE, FA_READ);
        if (fr == FR_OK)
        {
            size_t r;
            fr = f_read(&fil, selectedRom, sizeof(selectedRom), &r);        
            if (fr != FR_OK)
            {
                snprintf(ErrorMessage, 40, "Cannot read %s:%d\n", ROMINFOFILE, fr);
                selectedRom[0] = 0;
                printf(ErrorMessage);
            } else {
                selectedRom[r] = 0;
            }
        }
        else
        {
            snprintf(ErrorMessage, 40, "Cannot open %s:%d\n", ROMINFOFILE, fr);
            printf(ErrorMessage);
        }
        f_close(&fil);
    }
    while (true)
    {
        if (strlen(selectedRom) == 0)
        {
            screenMode_ = ScreenMode::NOSCANLINE_8_7;
            applyScreenMode();
            menu(NES_FILE_ADDR, ErrorMessage, isFatalError);  // never returns, but reboots upon selecting a game
        }
        printf("Now playing: %s\n", selectedRom);
        romSelector_.init(NES_FILE_ADDR);
        InfoNES_Main();
        selectedRom[0] = 0;
    }

    return 0;
}
