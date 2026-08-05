# rp2040-ili9341-infones

![](ezgif-1-d75ccea12c.gif)

**Build and usage instructions: [`software/infones/README.md`](software/infones/README.md)** — toolchain setup, pin assignments, how to get games onto the SD card, and what an FDS disk image additionally needs.

This is a handheld game console based on RP2040, designed for playing MakeCode Arcade games. Inspired by https://github.com/shuichitakano/pico-infones and https://github.com/fhoedemakers/pico-infonesPlus with thanks to "infones" https://github.com/jay-kumogata/InfoNES, I have made it capable of playing NES games as well. The main difference between this project and others is that I use the ILI9341 LCD, which is overclocked with the RP2040, along with an overclocked SPI interface, allowing this handheld to barely play NES games.

Famicom Disk System (mapper 20) images are supported as well: two-sided disks, side switching and saving all work. The FDS BIOS is Nintendo code and is not part of this repository — see the build README for where to put it. The FDS expansion sound channel is not emulated yet, so FDS music is missing its lead voice.

The menu renders Simplified and Traditional Chinese, so ROMs with Chinese filenames show up under their real names rather than as mojibake. Glyphs are Cubic 11 (俐方體十一號), an 11×11 pixel font, repacked into the firmware — details and the licensing note are in the [build README](software/infones/README.md#選單的中文顯示--chinese-text-in-the-menu). Fitting a 16 px cell into the menu's 232 scanlines costs some room: the ROM list shows 10 entries per page instead of 24.
