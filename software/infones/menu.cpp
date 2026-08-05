#include <stdio.h>
#include <string.h>

#include "FrensHelpers.h"
#include <pico.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include <hardware/sync.h>

#include "InfoNES_System.h"
// #include <util/exclusive_proc.h>
// #include <gamepad.h>
#include "hardware/watchdog.h"
#include "InfoNES.h"
#include "RomLister.h"
#include "menu.h"
// #include "nespad.h"
// #include "wiipad.h"

#include "font_cjk.h"
#define FONT_CHAR_WIDTH CJK_CELL_WIDTH
#define FONT_CHAR_HEIGHT CJK_CELL_HEIGHT
#define FONT_FIRST_ASCII ASCII_FIRST
#define SCREEN_COLS 32
// A cell is 16 px tall now that CJK glyphs have to fit, so the 232 scanlines
// the menu is given hold 14 rows instead of the 29 the 8x8 font managed.
#define SCREEN_ROWS 14

#define STARTROW 2
#define ENDROW 11
#define PAGESIZE (ENDROW - STARTROW + 1)

#define VISIBLEPATHSIZE (SCREEN_COLS - 3)

// extern util::ExclusiveProc exclProc_;
void screenMode(int incr);
extern const WORD __not_in_flash_func(NesPalette)[];

#define CBLACK 15
#define CWHITE 48
#define CRED 6
#define CGREEN 10
#define CBLUE 2
#define CLIGHTBLUE 0x2C
#define DEFAULT_FGCOLOR CBLACK // 60
#define DEFAULT_BGCOLOR CWHITE

static int fgcolor = DEFAULT_FGCOLOR;
static int bgcolor = DEFAULT_BGCOLOR;

// A cell holds a glyph index, not a character. Resolving a codepoint to a glyph
// costs a binary search, and the rasteriser below runs 32 cells x 232 scanlines
// every frame -- far too hot to search in. putText() does the lookup once, when
// the text changes, and stores the result here.
enum cellKind : uint8_t
{
    CELL_ASCII,      // glyph indexes ascii_bitmap, one cell wide
    CELL_WIDE_LEAD,  // glyph indexes cjk_bitmap; left half of a two-cell glyph
    CELL_WIDE_TRAIL, // right half, carrying the same glyph index
    CELL_TOFU_LEAD,  // codepoint the font has no glyph for; drawn as a box
    CELL_TOFU_TRAIL,
};

struct charCell
{
    uint8_t fgcolor : 4;
    uint8_t bgcolor : 4;
    uint8_t kind;
    uint16_t glyph;
};

#define SCREENBUFCELLS SCREEN_ROWS *SCREEN_COLS
static charCell *screenBuffer;
// The buffer is borrowed from the emulator's SRAM array; make sure the wider
// cell still fits before someone discovers it at runtime.
static_assert(SCREENBUFCELLS * sizeof(charCell) <= SRAM_SIZE,
              "screen buffer no longer fits in the borrowed emulator SRAM");

// Whether the FDS BIOS is on the SD card. Checked once when the menu starts;
// without it .fds images cannot be run, so the menu says so and refuses to
// flash one.
static bool fdsBiosOnCard = false;

static bool isFDSImage(const char *path)
{
    return Frens::cstr_endswith(path, ".fds") || Frens::cstr_endswith(path, ".FDS");
}

static void checkFDSBios()
{
    FILINFO fno;
    fdsBiosOnCard = (f_stat(FDS_BIOS_FILE, &fno) == FR_OK);
    printf("FDS BIOS %s: %s\n", FDS_BIOS_FILE, fdsBiosOnCard ? "found" : "not found");
}

#define UP 9
#define DN 5
#define LT 8
#define RT 6
#define SL 28
#define ST 4
#define A 2
#define B 3

#define _LEFT   1 << 6
#define _RIGHT   1 << 7
#define _UP   1 << 4
#define _DOWN   1 << 5
#define _SELECT   1 << 2
#define _START   1 << 3
#define _AA   1 << 0
#define _BB  1 << 1

static WORD *WorkLineRom = nullptr;
void RomSelect_SetLineBuffer(WORD *p, WORD size)
{
    WorkLineRom = p;
}

// Decode one UTF-8 sequence, returning the bytes consumed (0 at end of string).
// FatFs hands us UTF-8 now (FF_LFN_UNICODE 2), and a filename is whatever the
// card happens to contain, so malformed input must not desynchronise the rest
// of the line: anything invalid consumes a single byte and reports U+FFFD.
static int decodeUtf8(const char *s, uint32_t *cp)
{
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] == 0)
    {
        return 0;
    }
    if (p[0] < 0x80)
    {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
    {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        if (*cp >= 0x80)
        {
            return 2;
        }
    }
    else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
    {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        if (*cp >= 0x800)
        {
            return 3;
        }
    }
    else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
             (p[3] & 0xC0) == 0x80)
    {
        // Above the BMP. The glyph index is 16 bit, so these can only ever be
        // tofu; report a codepoint no lookup will match and skip all four bytes.
        *cp = 0xFFFF;
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

static inline bool isHalfWidth(uint32_t cp)
{
    return cp >= ASCII_FIRST && cp < ASCII_FIRST + ASCII_COUNT;
}

// Binary search of the sorted codepoint table. Only ever called from putText().
static int findWideGlyph(uint32_t cp)
{
    if (cp > 0xFFFF)
    {
        return -1;
    }
    int lo = 0;
    int hi = CJK_GLYPH_COUNT - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        uint16_t probe = cjk_index[mid];
        if (probe == cp)
        {
            return mid;
        }
        if (probe < cp)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }
    return -1;
}

// Width in cells rather than bytes: a CJK character takes two.
static int displayWidth(const char *s)
{
    uint32_t cp;
    int len;
    int width = 0;
    while ((len = decodeUtf8(s, &cp)) > 0)
    {
        width += isHalfWidth(cp) ? 1 : 2;
        s += len;
    }
    return width;
}

// Step over one whole sequence. Scrolling a byte at a time would cut a
// multi-byte character in half and render the tail as tofu.
static int utf8Advance(const char *s)
{
    uint32_t cp;
    int len = decodeUtf8(s, &cp);
    return len > 0 ? len : 1;
}

// Codepoints the font has no glyph for draw as a hollow box, so that a missing
// character reads as missing instead of as a space.
static unsigned tofuSlice(bool lead, int glyphRow)
{
    if (glyphRow < 1 || glyphRow > 11)
    {
        return 0;
    }
    unsigned bits = (glyphRow == 1 || glyphRow == 11) ? 0x1ffe : 0x1002;
    return lead ? (bits & 0xff) : (bits >> 8);
}

// static constexpr int LEFT = 1 << 6;
// static constexpr int RIGHT = 1 << 7;
// static constexpr int UP = 1 << 4;
// static constexpr int DOWN = 1 << 5;
// static constexpr int SELECT = 1 << 2;
// static constexpr int START = 1 << 3;
// static constexpr int A = 1 << 0;
// static constexpr int B = 1 << 1;
// static constexpr int X = 1 << 8;
// static constexpr int Y = 1 << 9;

void RomSelect_PadState(DWORD *pdwPad1, bool ignorepushed = false)
{

    static DWORD prevButtons{};
    // auto &gp = io::getCurrentGamePadState(0);

    // int v = (gp.buttons & io::GamePadState::Button::LEFT ? LEFT : 0) |
    //         (gp.buttons & io::GamePadState::Button::RIGHT ? RIGHT : 0) |
    //         (gp.buttons & io::GamePadState::Button::UP ? UP : 0) |
    //         (gp.buttons & io::GamePadState::Button::DOWN ? DOWN : 0) |
    //         (gp.buttons & io::GamePadState::Button::A ? A : 0) |
    //         (gp.buttons & io::GamePadState::Button::B ? B : 0) |
    //         (gp.buttons & io::GamePadState::Button::SELECT ? SELECT : 0) |
    //         (gp.buttons & io::GamePadState::Button::START ? START : 0) |
    //         (gp.buttons & io::GamePadState::Button::X ? X : 0) |
    //         (gp.buttons & io::GamePadState::Button::Y ? Y : 0) |
    //         0;
    // v |= nespad_state;
    int v=0;
    if (gpio_get(A)==0)      v |= _AA;
    if (gpio_get(B)==0)      v |= _BB;
    if (gpio_get(ST)==0)      v |= _START;
    if (gpio_get(SL)==0)      v |= _SELECT;
    if (gpio_get(LT)==0)      v |= _LEFT;
    if (gpio_get(RT)==0)      v |= _RIGHT;
    if (gpio_get(UP)==0)      v |= _UP;
    if (gpio_get(DN)==0)      v |= _DOWN;

// #if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
//     v |= wiipad_read();
// #endif

    *pdwPad1 = 0;
   
    unsigned long pushed;
 
    if (ignorepushed == false)
    {
        pushed = v & ~prevButtons;
    }
    else
    {
        pushed = v;
    }
    // if (p1 & SELECT)
    // {
    //     if (pushed & UP)
    //     {
    //         screenMode(-1);
    //         v = 0;
    //     }
    //     else if (pushed & DOWN)
    //     {
    //         screenMode(+1);
    //         v = 0;
    //     }
    // }
    if (pushed)
    {
        *pdwPad1 = v;
    }
    prevButtons = v;
}
void RomSelect_DrawLine(int line, int selectedRow)
{
    memset(WorkLineRom, 0, 640);

    int row = line / FONT_CHAR_HEIGHT;
    if (row >= SCREEN_ROWS)
    {
        // 14 rows of 16 px cover 224 of the 232 scanlines the menu is handed.
        // Fill the remainder with the background rather than leaving it black.
        WORD bgcolor = NesPalette[screenBuffer[0].bgcolor];
        for (auto i = 0; i < SCREEN_COLS * FONT_CHAR_WIDTH; ++i)
        {
            WorkLineRom[i] = bgcolor;
        }
        return;
    }
    int glyphRow = line % FONT_CHAR_HEIGHT - CJK_GLYPH_YSHIFT;

    for (auto i = 0; i < SCREEN_COLS; ++i)
    {
        const charCell &cell = screenBuffer[row * SCREEN_COLS + i];
        WORD fgcolor, bgcolor;
        if (row == selectedRow)
        {
            fgcolor = NesPalette[cell.bgcolor];
            bgcolor = NesPalette[cell.fgcolor];
        }
        else
        {
            fgcolor = NesPalette[cell.fgcolor];
            bgcolor = NesPalette[cell.bgcolor];
        }

        // Both glyph tables are fixed stride with the y_offset already baked in,
        // so selecting a row is arithmetic: no per-glyph metrics, no search.
        unsigned fontSlice = 0;
        switch (cell.kind)
        {
        case CELL_ASCII:
            if (glyphRow >= 0 && glyphRow < ASCII_GLYPH_ROWS)
            {
                fontSlice = ascii_bitmap[cell.glyph * ASCII_GLYPH_ROWS + glyphRow];
            }
            break;
        case CELL_WIDE_LEAD:
        case CELL_WIDE_TRAIL:
            if (glyphRow >= 0 && glyphRow < CJK_GLYPH_ROWS)
            {
                unsigned bits = cjk_bitmap[cell.glyph * CJK_GLYPH_ROWS + glyphRow];
                fontSlice = (cell.kind == CELL_WIDE_LEAD) ? (bits & 0xff) : (bits >> 8);
            }
            break;
        case CELL_TOFU_LEAD:
        case CELL_TOFU_TRAIL:
            fontSlice = tofuSlice(cell.kind == CELL_TOFU_LEAD, glyphRow);
            break;
        }

        for (auto bit = 0; bit < FONT_CHAR_WIDTH; bit++)
        {
            *WorkLineRom = (fontSlice & 1) ? fgcolor : bgcolor;
            fontSlice >>= 1;
            WorkLineRom++;
        }
    }
    return;
}

void drawline(int scanline, int selectedRow)
{
    RomSelect_PreDrawLine(scanline);
    RomSelect_DrawLine(scanline - 4, selectedRow);
    InfoNES_PostDrawLine(scanline);
}

static void putText(int x, int y, const char *text, int fgcolor, int bgcolor)
{
    if (text == nullptr || y < 0 || y >= SCREEN_ROWS)
    {
        return;
    }

    uint32_t cp;
    int len;
    while (x < SCREEN_COLS && (len = decodeUtf8(text, &cp)) > 0)
    {
        text += len;
        charCell *cell = &screenBuffer[y * SCREEN_COLS + x];
        if (isHalfWidth(cp))
        {
            cell->kind = CELL_ASCII;
            cell->glyph = cp - ASCII_FIRST;
            cell->fgcolor = fgcolor;
            cell->bgcolor = bgcolor;
            x++;
            continue;
        }
        // Anything else is double width. Stop rather than let half a glyph
        // spill past the right edge of the screen.
        if (x + 1 >= SCREEN_COLS)
        {
            break;
        }
        int glyph = findWideGlyph(cp);
        cell[0].kind = (glyph < 0) ? CELL_TOFU_LEAD : CELL_WIDE_LEAD;
        cell[1].kind = (glyph < 0) ? CELL_TOFU_TRAIL : CELL_WIDE_TRAIL;
        cell[0].glyph = cell[1].glyph = (glyph < 0) ? 0 : (uint16_t)glyph;
        cell[0].fgcolor = cell[1].fgcolor = fgcolor;
        cell[0].bgcolor = cell[1].bgcolor = bgcolor;
        x += 2;
    }
}

void DrawScreen(int selectedRow)
{
    for (auto line = 4; line < 236; line++)
    {
        drawline(line, selectedRow);
    }
}

void ClearScreen(charCell *screenBuffer, int color)
{
    for (auto i = 0; i < SCREENBUFCELLS; i++)
    {
        screenBuffer[i].bgcolor = color;
        screenBuffer[i].fgcolor = color;
        screenBuffer[i].kind = CELL_ASCII;
        screenBuffer[i].glyph = ' ' - ASCII_FIRST;
    }
}

void displayRoms(Frens::RomLister romlister, int startIndex)
{
    char buffer[ROMLISTER_MAXPATH + 4];
    auto y = STARTROW;
    auto entries = romlister.GetEntries();
    ClearScreen(screenBuffer, bgcolor);
    putText(1, 0, "Choose a rom to play:", fgcolor, bgcolor);
    putText(1, SCREEN_ROWS - 1, "A: Select, B: Back", fgcolor, bgcolor);
    for (auto index = startIndex; index < romlister.Count(); index++)
    {
        if (y <= ENDROW)
        {
            auto info = entries[index];
            if (info.IsDirectory)
            {
                snprintf(buffer, sizeof(buffer), "D %s", info.Path);
            }
            else
            {
                snprintf(buffer, sizeof(buffer), "R %s", info.Path);
            }

            putText(1, y, buffer, fgcolor, bgcolor);
            y++;
        }
    }
    if (!fdsBiosOnCard)
    {
        putText(1, SCREEN_ROWS - 2, "No " FDS_BIOS_FILE ": .fds disabled", CRED, bgcolor);
    }
}

void DisplayFatalError(char *error)
{
    ClearScreen(screenBuffer, bgcolor);
    putText(0, 0, "Fatal error:", fgcolor, bgcolor);
    putText(1, 3, error, fgcolor, bgcolor);
    while (true)
    {
        auto frameCount = InfoNES_LoadFrame();
        DrawScreen(-1);
    }
}

void DisplayEmulatorErrorMessage(char *error)
{
    DWORD PAD1_Latch;
    ClearScreen(screenBuffer, bgcolor);
    putText(0, 0, "Error occured:", fgcolor, bgcolor);
    putText(0, 3, error, fgcolor, bgcolor);
    putText(0, ENDROW, "Press a button to continue.", fgcolor, bgcolor);
    while (true)
    {
        auto frameCount = InfoNES_LoadFrame();
        DrawScreen(-1);
        RomSelect_PadState(&PAD1_Latch);
        if (PAD1_Latch > 0)
        {
            return;
        }
    }
}

void showSplashScreen()
{
    DWORD PAD1_Latch;
    char s[SCREEN_COLS];
    ClearScreen(screenBuffer, bgcolor);

    strcpy(s, "maGc - infones");
    putText(SCREEN_COLS / 2 - (strlen(s) + 4) / 2, 1, s, fgcolor, bgcolor);

    putText((SCREEN_COLS / 2 - (strlen(s)) / 2) + 9, 1, "N", CRED, bgcolor);
    putText((SCREEN_COLS / 2 - (strlen(s)) / 2) + 10, 1, "E", CGREEN, bgcolor);
    putText((SCREEN_COLS / 2 - (strlen(s)) / 2) + 11, 1, "S", CBLUE, bgcolor);
    // strcpy(s, "Emulator");
    //putText(SCREEN_COLS / 2 - strlen(s) / 2, 5, s, fgcolor, bgcolor);
     strcpy(s, "@pondahai");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 3, s, CLIGHTBLUE, bgcolor);

    strcpy(s, "Pico Port");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 5, s, fgcolor, bgcolor);
     strcpy(s, "@shuichi_takano");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 6, s, CLIGHTBLUE, bgcolor);

    // Trimmed to fit: 14 rows of 16 px replaced the old 29 rows of 8 px.
    strcpy(s, "Menu System & SD Card" );
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 8, s, fgcolor, bgcolor);
    strcpy(s, "@frenskefrens");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 9, s, CLIGHTBLUE, bgcolor);

    // strcpy(s, "(S)NES controller support");
    //putText(SCREEN_COLS / 2 - strlen(s) / 2, 17, s, fgcolor, bgcolor);

    // strcpy(s, "@PaintYourDragon @adafruit");
    //putText(SCREEN_COLS / 2 - strlen(s) / 2, 18, s, CLIGHTBLUE, bgcolor);

    strcpy(s, "PCB Design");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 11, s, fgcolor, bgcolor);

    strcpy(s, "@pondahai");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 12, s, CLIGHTBLUE, bgcolor);
    int startFrame = -1;
    while (true)
    {
        auto frameCount = InfoNES_LoadFrame();
        if (startFrame == -1)
        {
            startFrame = frameCount;
        }
        DrawScreen(-1);
        RomSelect_PadState(&PAD1_Latch);
        if (PAD1_Latch > 0 || (frameCount - startFrame) > 1000)
        {
            return;
        }
        if ((frameCount % 30) == 0)
        {
            for (auto i = 0; i < SCREEN_COLS; i++)
            {
                auto col = rand() % 63;
                putText(i, 0, " ", col, col);
                col = rand() % 63;
                putText(i, SCREEN_ROWS - 1, " ", col, col);
            }
            for (auto i = 1; i < SCREEN_ROWS - 1; i++)
            {
                auto col = rand() % 63;
                putText(0, i, " ", col, col);
                col = rand() % 63;
                putText(SCREEN_COLS - 1, i, " ", col, col);
            }
        }
    }
}

void screenSaver()
{
    DWORD PAD1_Latch;
    WORD frameCount;
    while (true)
    {
        frameCount = InfoNES_LoadFrame();
        DrawScreen(-1);
        RomSelect_PadState(&PAD1_Latch);
        if (PAD1_Latch > 0)
        {
            return;
        }
        if ((frameCount % 3) == 0)
        {
            auto color = rand() % 63;
            auto row = rand() % SCREEN_ROWS;
            auto column = rand() % SCREEN_COLS;
            putText(column, row, " ", color, color);
        }
    }
}
// Global instances of local vars in romselect() some used in Lambda expression later on
static char *selectedRomOrFolder;
static uintptr_t FLASH_ADDRESS;
static bool errorInSavingRom = false;
static char *globalErrorMessage;

static bool showSplash = true;

void menu(uintptr_t NES_FILE_ADDR, char *errorMessage, bool isFatal)
{
    FLASH_ADDRESS = NES_FILE_ADDR;
    int firstVisibleRowINDEX = 0;
    int selectedRow = STARTROW;
    char currentDir[FF_MAX_LFN];
    int totalFrames = -1;

    globalErrorMessage = errorMessage;
    FRESULT fr;
    DWORD PAD1_Latch;

    int horzontalScrollIndex = 0;
    printf("Starting Menu\n");
    size_t ramsize;
    // Borrow Emulator RAM buffer for screen.
    screenBuffer = (charCell *)InfoNes_GetRAM(&ramsize);
    size_t chr_size;
    // Borrow ChrBuffer to store directory contents
    void *buffer = InfoNes_GetChrBuf(&chr_size);
    Frens::RomLister romlister(buffer, chr_size);

    if (strlen(errorMessage) > 0)
    {
        if (isFatal) // SD card not working, show error
        {
            DisplayFatalError(errorMessage);
        }
        else
        {
            DisplayEmulatorErrorMessage(errorMessage); // Emulator cannot start, show error
        }
        showSplash = false;
    }
    if (showSplash)
    {
        showSplash = false;
        showSplashScreen();
    }
    checkFDSBios();
    romlister.list("/");
    displayRoms(romlister, firstVisibleRowINDEX);
    while (1)
    {

        auto frameCount = InfoNES_LoadFrame();
        auto index = selectedRow - STARTROW + firstVisibleRowINDEX;
        auto entries = romlister.GetEntries();
        selectedRomOrFolder = (romlister.Count() > 0) ? entries[index].Path : nullptr;
        errorInSavingRom = false;
        DrawScreen(selectedRow);
        RomSelect_PadState(&PAD1_Latch);
        if (PAD1_Latch > 0)
        {
            totalFrames = frameCount; // Reset screenSaver
            // reset horizontal scroll of highlighted row
            horzontalScrollIndex = 0;
            putText(3, selectedRow, selectedRomOrFolder, fgcolor, bgcolor);

            // if ((PAD1_Latch & Y) == Y)
            // {
            //     fgcolor++;
            //     if (fgcolor > 63)
            //     {
            //         fgcolor = 0;
            //     }
            //     printf("fgColor++ : %02d (%04x)\n", fgcolor, NesPalette[fgcolor]);
            //     displayRoms(romlister, firstVisibleRowINDEX);
            // }
            // else if ((PAD1_Latch & X) == X)
            // {
            //     bgcolor++;
            //     if (bgcolor > 63)
            //     {
            //         bgcolor = 0;
            //     }
            //     printf("bgColor++ : %02d (%04x)\n", bgcolor, NesPalette[bgcolor]);
            //     displayRoms(romlister, firstVisibleRowINDEX);
            // }
            // else 
            if ((PAD1_Latch & _UP) == _UP && selectedRomOrFolder)
            {
                if (selectedRow > STARTROW)
                {
                    selectedRow--;
                }
                else
                {
                    if (firstVisibleRowINDEX > 0)
                    {
                        firstVisibleRowINDEX--;
                        displayRoms(romlister, firstVisibleRowINDEX);
                    }
                }
            }
            else if ((PAD1_Latch & _DOWN) == _DOWN && selectedRomOrFolder)
            {
                if (selectedRow < ENDROW && (index) < romlister.Count() - 1)
                {
                    selectedRow++;
                }
                else
                {
                    if (index < romlister.Count() - 1)
                    {
                        firstVisibleRowINDEX++;
                        displayRoms(romlister, firstVisibleRowINDEX);
                    }
                }
            }
            else if ((PAD1_Latch & _LEFT) == _LEFT && selectedRomOrFolder)
            {
                firstVisibleRowINDEX -= PAGESIZE;
                if (firstVisibleRowINDEX < 0)
                {
                    firstVisibleRowINDEX = 0;
                }
                selectedRow = STARTROW;
                displayRoms(romlister, firstVisibleRowINDEX);
            }
            else if ((PAD1_Latch & _RIGHT) == _RIGHT && selectedRomOrFolder)
            {
                if (firstVisibleRowINDEX + PAGESIZE < romlister.Count())
                {
                    firstVisibleRowINDEX += PAGESIZE;
                }
                selectedRow = STARTROW;
                displayRoms(romlister, firstVisibleRowINDEX);
            }
            else if ((PAD1_Latch & _BB) == _BB)
            {
                fr = f_getcwd(currentDir, 40);
                if (fr != FR_OK)
                {
                    printf("Cannot get current dir: %d\n", fr);
                }
                if (strcmp(currentDir, "/") != 0)
                {
                    romlister.list("..");
                    firstVisibleRowINDEX = 0;
                    selectedRow = STARTROW;
                    displayRoms(romlister, firstVisibleRowINDEX);
                }
            }
            else if ((PAD1_Latch & _START) == _START && (PAD1_Latch & _SELECT) != _SELECT)
            {
                // start emulator with currently loaded game
                break;
            }
            else if ((PAD1_Latch & _AA) == _AA && selectedRomOrFolder)
            {

                if (entries[index].IsDirectory)
                {
                    romlister.list(selectedRomOrFolder);
                    firstVisibleRowINDEX = 0;
                    selectedRow = STARTROW;
                    displayRoms(romlister, firstVisibleRowINDEX);
                }
                else if (isFDSImage(selectedRomOrFolder) && !fdsBiosOnCard)
                {
                    // Flashing the image would only lead to a black screen
                    // after the reboot, so refuse here where we can explain.
                    snprintf(globalErrorMessage, 40, "Missing %s on SD card", FDS_BIOS_FILE);
                    DisplayEmulatorErrorMessage(globalErrorMessage);
                    globalErrorMessage[0] = 0;
                    displayRoms(romlister, firstVisibleRowINDEX);
                }
                else
                {
                    // https://kevinboone.me/picoflash.html?i=1
                    // https://www.makermatrix.com/blog/read-and-write-data-with-the-pi-pico-onboard-flash/
                    multicore_reset_core1(); // Stop Core 1 (Audio) safely before erasing/writing flash
                    printf("Start saving rom to flash memory\n");
                    // exclProc_.setProcAndWait(
                    //     []
                    //     {
                            FIL fil;
                            FRESULT fr;
                            // borrow buffer from where to flash from emulator
                            size_t bufsize;
                            BYTE *buffer = (BYTE *)InfoNes_GetPPURAM(&bufsize);

                            auto ofs = FLASH_ADDRESS - XIP_BASE;
                            printf("write %s rom to flash %x\n", selectedRomOrFolder, ofs);
                            fr = f_open(&fil, selectedRomOrFolder, FA_READ);

                            UINT bytesRead;
                            if (fr == FR_OK)
                            {
                                for (;;)
                                {
                                    fr = f_read(&fil, buffer, bufsize, &bytesRead);
                                    if (fr == FR_OK)
                                    {
                                        if (bytesRead == 0)
                                        {
                                            break;
                                        }
                                        printf("Flashing %d bytes to flash address %x\n", bytesRead, ofs);
                                        printf("  -> Erasing...");

                                        // Disable interupts, erase, flash and enable interrupts
                                        uint32_t ints = save_and_disable_interrupts();
                                        flash_range_erase(ofs, bufsize);
                                        printf("\n  -> Flashing...");
                                        flash_range_program(ofs, buffer, bufsize);
                                        restore_interrupts(ints);
                                        //
                                        
                                        printf("\n");
                                        ofs += bufsize;
                                    }
                                    else
                                    {
                                        snprintf(globalErrorMessage, 40, "Error reading rom: %d", fr);
                                        printf("Error reading rom: %d\n", fr);
                                        errorInSavingRom = true;
                                        break;
                                    }
                                }
                                f_close(&fil);
                            }
                            else
                            {
                                snprintf(globalErrorMessage, 40, "Cannot open rom %d", fr);
                                printf("%s\n", globalErrorMessage);
                                errorInSavingRom = true;
                            }
                            if (!errorInSavingRom)
                            {
                                // Create file containing name currently loaded rom
                                printf("Creating %s\n", ROMINFOFILE);
                                fr = f_open(&fil, ROMINFOFILE, FA_CREATE_ALWAYS | FA_WRITE);
                                if (fr == FR_OK)
                                {
                                    // f_write, not a f_putc loop. f_putc builds a
                                    // fresh putbuff every call, and under
                                    // FF_LFN_UNICODE 2 it holds the bytes of a
                                    // multi-byte sequence in that buffer until the
                                    // sequence completes -- so writing a UTF-8 name
                                    // one byte at a time drops every non-ASCII
                                    // character on the floor. f_write is binary and
                                    // does no encoding conversion.
                                    size_t nameLen = strlen(selectedRomOrFolder) - 4; // without ".nes"
                                    UINT written;
                                    fr = f_write(&fil, selectedRomOrFolder, nameLen, &written);
                                    printf("%.*s\n", (int)nameLen, selectedRomOrFolder);
                                    if (fr != FR_OK || written != nameLen)
                                    {
                                        snprintf(globalErrorMessage, 40, "Error writing file %d", fr);
                                        printf("%s\n", globalErrorMessage);
                                        errorInSavingRom = true;
                                    }
                                }
                                else
                                {
                                    printf("Cannot create %s:%d\n", ROMINFOFILE, fr);
                                    snprintf(globalErrorMessage, 40, "Cannot create %s:%d", ROMINFOFILE, fr);
                                    errorInSavingRom = true;
                                }
                                f_close(&fil);
                            }
                        // });
                    printf("done\n");
                    if (!errorInSavingRom)
                    {
                        selectedRomOrFolder[strlen(selectedRomOrFolder) - 4] = 0;
                        break; // starts emulator
                    }
                    else
                    {
                        DisplayEmulatorErrorMessage(globalErrorMessage);
                        globalErrorMessage[0] = 0;
                    }
                }
            }
        }
        // scroll selected row horizontally if textsize exceeds rowlength
        if (selectedRomOrFolder)
        {
            if ((frameCount % 30) == 0)
            {
                // Measured and advanced in whole characters: a byte at a time
                // would cut a UTF-8 sequence in half, and a CJK name is half as
                // many characters as it is cells wide.
                if (displayWidth(selectedRomOrFolder + horzontalScrollIndex) > VISIBLEPATHSIZE)
                {
                    horzontalScrollIndex += utf8Advance(selectedRomOrFolder + horzontalScrollIndex);
                }
                else
                {
                    horzontalScrollIndex = 0;
                }
                putText(3, selectedRow, selectedRomOrFolder + horzontalScrollIndex, fgcolor, bgcolor);
            }
        }
        if (totalFrames == -1)
        {
            totalFrames = frameCount;
        }
        if ((frameCount - totalFrames) > 800)
        {
            printf("Starting screensaver\n");
            totalFrames = -1;
            screenSaver();
            displayRoms(romlister, firstVisibleRowINDEX);
        }
    }
    // Wait until user has released all buttons
    while (1)
    {
        InfoNES_LoadFrame();
        DrawScreen(-1);
        RomSelect_PadState(&PAD1_Latch, true);
        if (PAD1_Latch == 0)
        {
            break;
        }
    }
// #if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
//     wiipad_end();
// #endif

    // Don't return from this function call, but reboot in order to get the sound properly working
    // Starting emulator after return from menu often disables or corrupts sound
    // After reboot, the emulator starts the selected game.
    printf("Rebooting...\n");
    watchdog_enable(100, 1);
    while(1);
    // Never return
}
