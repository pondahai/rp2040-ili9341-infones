#ifndef ROMSELECT
#define ROMSELECT
#define ROMINFOFILE "/currentloadedrom.txt"
// Famicom Disk System BIOS. Nintendo copyrighted code, so it is not part of
// this repository -- the user drops it in the root of the SD card.
#define FDS_BIOS_FILE "/disksys.rom"
void RomSelect_SetLineBuffer(WORD *p, WORD size);
void menu(uintptr_t NES_FILE_ADDR, char *errorMessage, bool isFatalError);

#endif