// Regression test for the two UTF-8 paths the Chinese menu depends on.
//
// Builds the firmware's own FatFs sources against its own unmodified ffconf.h
// and runs them on a RAM disk, so what it checks is the real configuration
// rather than a copy that can drift.
//
//   1. Writing currentloadedrom.txt. This must be f_write. Under
//      FF_LFN_UNICODE 2 the f_putc/f_puts family are encoding converters:
//      putc_bfd() parks the bytes of a multi-byte sequence in its putbuff
//      until the sequence completes, and f_putc builds a fresh putbuff every
//      call, so writing UTF-8 a byte at a time discards every non-ASCII
//      character. A ROM whose name was entirely Chinese produced an empty
//      file and bounced the emulator back to the menu.
//
//   2. Reading a Chinese filename back through f_readdir, which is what puts
//      the name in the ROM list in the first place.
//
// Exits non-zero if anything fails.
//
// Build and run: make && ./fatfs_utf8_test

#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"

#define SECTOR_COUNT 128
#define ROOT_ENTRIES 16

static BYTE ramdisk[SECTOR_COUNT * FF_MAX_SS];

static void put16(BYTE *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put32(BYTE *p, unsigned long v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

// Laid out by hand rather than with f_mkfs, so that ffconf.h can be used as
// the firmware ships it: FF_USE_MKFS is 0 there and should stay 0, since the
// firmware never formats a card.
static void formatFat12(void)
{
    memset(ramdisk, 0, sizeof ramdisk);
    BYTE *bs = ramdisk;

    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;      // jump, as check_fs expects
    memcpy(bs + 3, "MSDOS5.0", 8);
    put16(bs + 11, FF_MAX_SS);                      // BPB_BytsPerSec
    bs[13] = 1;                                     // BPB_SecPerClus
    put16(bs + 14, 1);                              // BPB_RsvdSecCnt
    bs[16] = 2;                                     // BPB_NumFATs
    put16(bs + 17, ROOT_ENTRIES);                   // BPB_RootEntCnt
    put16(bs + 19, SECTOR_COUNT);                   // BPB_TotSec16
    bs[21] = 0xF8;                                  // BPB_Media
    put16(bs + 22, 1);                              // BPB_FATSz16
    put16(bs + 24, 1);                              // BPB_SecPerTrk
    put16(bs + 26, 1);                              // BPB_NumHeads
    put32(bs + 28, 0);                              // BPB_HiddSec
    bs[38] = 0x29;                                  // BS_BootSig
    put32(bs + 39, 0x12345678);                     // BS_VolID
    memcpy(bs + 43, "NO NAME    ", 11);
    memcpy(bs + 54, "FAT12   ", 8);
    put16(bs + 510, 0xAA55);

    for (int fat = 0; fat < 2; fat++)               // media byte, end of chain
    {
        BYTE *p = ramdisk + (1 + fat) * FF_MAX_SS;
        p[0] = 0xF8; p[1] = 0xFF; p[2] = 0xFF;
    }
}

DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return 0; }
DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return 0; }
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (sector + count > SECTOR_COUNT) return RES_PARERR;
    memcpy(buff, ramdisk + sector * FF_MAX_SS, count * FF_MAX_SS);
    return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (sector + count > SECTOR_COUNT) return RES_PARERR;
    memcpy(ramdisk + sector * FF_MAX_SS, buff, count * FF_MAX_SS);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd)
    {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buff = SECTOR_COUNT; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buff = FF_MAX_SS; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
    }
    return RES_PARERR;
}
DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (1 << 21) | (1 << 16); }

static FATFS fs;
static int failures;

static void hex(const char *s, int n)
{
    for (int i = 0; i < n; i++) printf("%02x ", (unsigned char)s[i]);
}

// menu.cpp's approach before the fix, kept so the failure mode stays visible
// and nobody "simplifies" the write back into a putc loop.
static int writeWithPutc(const char *path, const char *name, size_t len)
{
    FIL fil;
    if (f_open(&fil, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;
    for (size_t i = 0; i < len; i++)
    {
        if (f_putc(name[i], &fil) < 0) { f_close(&fil); return -1; }
    }
    f_close(&fil);
    return 0;
}

// What menu.cpp does now.
static int writeWithWrite(const char *path, const char *name, size_t len)
{
    FIL fil;
    UINT written;
    if (f_open(&fil, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;
    FRESULT fr = f_write(&fil, name, len, &written);
    f_close(&fil);
    return (fr == FR_OK && written == len) ? 0 : -1;
}

// What main.cpp does on the next boot.
static int readBack(const char *path, char *out, size_t cap)
{
    FIL fil;
    UINT r = 0;
    if (f_open(&fil, path, FA_READ) != FR_OK) return -1;
    f_read(&fil, out, cap - 1, &r);
    out[r] = 0;
    f_close(&fil);
    return (int)r;
}

static void checkRomNameWrite(const char *label, const char *filename)
{
    size_t len = strlen(filename) - 4;              // strip ".nes", as menu.cpp does
    char oldBuf[80] = {0}, newBuf[80] = {0};

    writeWithPutc("/old.txt", filename, len);
    writeWithWrite("/new.txt", filename, len);
    int oldLen = readBack("/old.txt", oldBuf, sizeof oldBuf);
    int newLen = readBack("/new.txt", newBuf, sizeof newBuf);
    int newOk = (newLen == (int)len) && memcmp(newBuf, filename, len) == 0;

    printf("  %-24s expected %2zu bytes: ", label, len);
    hex(filename, (int)len);
    printf("\n    f_putc loop (old) %2d bytes  %s\n", oldLen,
           (oldLen == (int)len) ? "intact" : (oldLen <= 0 ? "EMPTY -> boots back to menu" : "truncated"));
    printf("    f_write     (new) %2d bytes  %s\n", newLen, newOk ? "ok" : "FAIL");
    if (!newOk) failures++;
}

// Root directory sits after the reserved sector and both FATs.
#define ROOT_DIR_SECTOR 3

// Decode UTF-8 to UTF-16, returning the number of code units. Deliberately
// independent of FatFs so the expectation cannot move with the code under test.
static int utf8ToUtf16(const char *s, WORD *out, int cap)
{
    int n = 0;
    while (*s && n < cap)
    {
        const unsigned char *p = (const unsigned char *)s;
        DWORD cp;
        if (p[0] < 0x80) { cp = p[0]; s += 1; }
        else if ((p[0] & 0xE0) == 0xC0) { cp = ((DWORD)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); s += 2; }
        else if ((p[0] & 0xF0) == 0xE0)
        {
            cp = ((DWORD)(p[0] & 0x0F) << 12) | ((DWORD)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            s += 3;
        }
        else return -1;
        out[n++] = (WORD)cp;
    }
    return n;
}

// Pull the long name straight out of the on-disk directory entries.
static int readLfnFromDisk(WORD *out, int cap)
{
    BYTE *dir = ramdisk + ROOT_DIR_SECTOR * FF_MAX_SS;
    int longest = 0;
    for (int e = 0; e < ROOT_ENTRIES; e++)
    {
        BYTE *ent = dir + e * 32;
        if (ent[0] == 0x00 || ent[0] == 0xE5) continue;
        if (ent[11] != 0x0F) continue;                  // not an LFN entry
        int seq = (ent[0] & 0x1F) - 1;
        if (seq < 0) continue;
        static const int slot[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (int i = 0; i < 13; i++)
        {
            int at = seq * 13 + i;
            if (at >= cap) break;
            WORD wc = (WORD)(ent[slot[i]] | (ent[slot[i] + 1] << 8));
            out[at] = wc;
            if (wc != 0 && wc != 0xFFFF && at + 1 > longest) longest = at + 1;
        }
    }
    return longest;
}

// The read side. A round trip through FatFs alone proves nothing: with
// FF_LFN_UNICODE 0 every UTF-8 byte maps to some CP437 character and maps
// back again, so the name survives while what is stored on the card is
// mojibake. What has to be true is that the directory entry holds the real
// codepoints, since that is what a card written on a PC contains.
static void checkFilenameOnDisk(const char *name)
{
    FIL fil;
    char path[128];
    snprintf(path, sizeof path, "/%s", name);

    // Start from a clean root so only this file's entries are present.
    formatFat12();
    f_mount(&fs, "", 1);

    FRESULT fr = f_open(&fil, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) { printf("  %-24s f_open failed: %d  FAIL\n", name, fr); failures++; return; }
    f_close(&fil);

    WORD expect[64] = {0}, actual[64] = {0};
    int wanted = utf8ToUtf16(name, expect, 64);
    int got = readLfnFromDisk(actual, 64);
    int ok = (wanted > 0) && (got == wanted) && memcmp(expect, actual, wanted * sizeof(WORD)) == 0;

    printf("  %-24s on-disk LFN  %s\n", name, ok ? "ok" : "FAIL");
    if (!ok)
    {
        printf("    expected UTF-16:");
        for (int i = 0; i < wanted; i++) printf(" %04x", expect[i]);
        printf("\n    on disk        :");
        for (int i = 0; i < got; i++) printf(" %04x", actual[i]);
        printf("\n");
        failures++;
    }

    DIR dir;
    FILINFO info;
    int listed = 0;
    if (f_opendir(&dir, "/") == FR_OK)
    {
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0])
        {
            if (strcmp(info.fname, name) == 0) { listed = 1; break; }
        }
        f_closedir(&dir);
    }
    if (!listed) { printf("  %-24s f_readdir  FAIL\n", name); failures++; }
}

int main(void)
{
    formatFat12();
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) { printf("mount failed: %d\n", fr); return 1; }

    printf("FF_LFN_UNICODE=%d  FF_STRF_ENCODE=%d  FF_CODE_PAGE=%d  FF_USE_LFN=%d\n\n",
           FF_LFN_UNICODE, FF_STRF_ENCODE, FF_CODE_PAGE, FF_USE_LFN);

    printf("currentloadedrom.txt write path\n");
    checkRomNameWrite("all ASCII", "Contra.nes");
    checkRomNameWrite("all Traditional", "超級瑪利歐.nes");
    checkRomNameWrite("all Simplified", "超级马里奥.nes");
    checkRomNameWrite("mixed", "洛克人 3.nes");

    printf("\nfilename read path\n");
    checkFilenameOnDisk("Contra.nes");
    checkFilenameOnDisk("超級瑪利歐.nes");
    checkFilenameOnDisk("超级马里奥.nes");
    checkFilenameOnDisk("洛克人 3.nes");

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
