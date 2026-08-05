# infoNES for RP2040 + ILI9341 / ST7789

NES 與 FDS（紅白機磁碟機）模擬器韌體。遊戲從 SD 卡選取，燒進 flash 後執行。
NES and Famicom Disk System emulator firmware. Games are picked from an SD card,
burned into flash, and run from there.

---

## 需要準備 / Prerequisites

| 項目 | 說明 |
|---|---|
| Raspberry Pi Pico SDK | 2.x（本專案以 2.2.0 建置驗證） |
| ARM 工具鏈 | `arm-none-eabi-gcc`（SDK 隨附的 14.2 可用） |
| CMake ≥ 3.13 + Ninja 或 Make | |
| **picotool** | **建議用預先編好的**，見下方說明 |
| **pioasm** | **建議用預先編好的**，見下方說明 |

> ⚠️ **Windows 使用者請注意**：SDK 找不到 `picotool` / `pioasm` 時會嘗試
> 自行編譯，而 **`pioasm` 需要的是「主機端」C++ 編譯器**（MSVC 或 MinGW），
> 不是 ARM 那一套。沒有安裝的話會停在
> `No CMAKE_CXX_COMPILER could be found`。
> 最省事的做法是直接指向現成的執行檔（見下方 `-Dpioasm_DIR` / `-Dpicotool_DIR`）。
> 用 VS Code 的 Raspberry Pi Pico 擴充套件安裝過 SDK 的話，
> 這兩個工具已經在 `~/.pico-sdk/` 底下了。

---

## 編譯 / Build

```bash
cd software/infones
mkdir build && cd build

# 若您的環境未設定好 Pico SDK，需要手動指定路徑：
cmake .. -G Ninja \
  -DPICO_SDK_PATH=<pico-sdk 的路徑> \
  -DPICO_TOOLCHAIN_PATH=<arm-none-eabi 工具鏈的路徑> \
  -Dpicotool_DIR=<picotool 的 cmake 設定檔目錄> \
  -Dpioasm_DIR=<pioasm 的 cmake 設定檔目錄>

ninja
```

產出 `infoNES.uf2`。按住 BOOTSEL 接上 USB，把它拖進出現的虛擬磁碟即可。
Produces `infoNES.uf2`. Hold BOOTSEL while plugging in USB, then drag the file
onto the drive that appears.

> 💡 **Windows 免按鈕自動燒錄小技巧**：
> 如果您的 Pico 已經燒錄過並在執行中，您可以透過 PowerShell 送出 1200 baud 的序列埠連線訊號，強制 Pico 自動重啟進入 BOOTSEL 模式，並使用指令直接複製檔案，完全不需要按按鈕！
> ```powershell
> # 1. 觸發 1200 baud 重啟訊號
> foreach ($port in @('COM2','COM3','COM4')) { try { $p = New-Object System.IO.Ports.SerialPort $port,1200; $p.Open(); Start-Sleep -Milliseconds 500; $p.Close() } catch {} }
> Start-Sleep -Seconds 2
> 
> # 2. 自動將編譯好的 uf2 複製到 Pico 隨身碟 (假設自動掛載為 E:)
> Copy-Item -Path .\build\infoNES.uf2 -Destination E:\
> ```

`PICO_SDK_PATH` **不指定的話**會退回預設值 `../../../pico-sdk`，也就是假設
你在倉庫旁邊有一份 pico-sdk clone。多數情況下你會需要明確指定它。
命令列的 `-DPICO_SDK_PATH=` 與環境變數 `PICO_SDK_PATH` 都比預設值優先。

<details>
<summary>Windows / VS Code Pico 擴充套件的實際路徑範例</summary>

```bash
cmake .. -G Ninja \
  -DPICO_SDK_PATH="$HOME/.pico-sdk/sdk/2.2.0" \
  -DPICO_TOOLCHAIN_PATH="$HOME/.pico-sdk/toolchain/14_2_Rel1" \
  -DCMAKE_MAKE_PROGRAM="$HOME/.pico-sdk/ninja/v1.12.1/ninja.exe" \
  -Dpicotool_DIR="$HOME/.pico-sdk/picotool/2.2.0-a4/picotool" \
  -Dpioasm_DIR="$HOME/.pico-sdk/tools/2.2.0/pioasm"
```
</details>

> 💡 **Windows 環境的更簡單作法**：
> 建議下載官方的 [Pico Setup for Windows](https://github.com/raspberrypi/pico-setup-windows/releases) 整合安裝檔。安裝完成後，從開始選單開啟 **「Pico - Developer Command Prompt」**。
> 在這個專屬終端機內，系統已將 SDK 與工具鏈的環境變數全數設定完畢，您只需直接執行：
> ```bash
> cmake .. -G Ninja
> ninja
> ```
> 即可成功編譯，完全不需手動指定那一大串路徑！

---

## 選擇 LCD / Selecting the LCD

`CMakeLists.txt:47-48`，把要用的那一行取消註解：

```cmake
set(LCD_CONTROLLER "ILI9341" CACHE STRING "Select the LCD controller type")
#set(LCD_CONTROLLER "ST7789" CACHE STRING "Select the LCD controller type")
```

**換過 LCD 之後必須清空整個 `build` 資料夾再重新編譯**，否則會混到上一次的中間檔。
After switching the LCD you must empty the whole `build` folder and rebuild.

---

## 接腳 / Pin assignments

以下是預設值（`CMakeLists.txt:41-57`），都可以用 `-D<名稱>=<GPIO>` 覆寫。
Defaults below; every one can be overridden with `-D<NAME>=<GPIO>`.

| | SPI | CS | SCK/CLK | MOSI | MISO | DC | RST | BL |
|---|---|---|---|---|---|---|---|---|
| SD 卡 | spi1 | 13 | 10 | 11 | 12 | — | — | — |
| LCD | spi0 | 17 | 18 | 19 | −1 | 20 | 21 | 22 |

按鍵直接接 GPIO，低電位有效（`menu.cpp:78-85`）：
Buttons are wired straight to GPIO, active low:

| 上 | 下 | 左 | 右 | A | B | Select | Start |
|---|---|---|---|---|---|---|---|
| 9 | 5 | 8 | 6 | 2 | 3 | 28 | 4 |

---

## 放遊戲 / Getting games onto the device

**遊戲 ROM 不隨附也不會進倉庫**，請自備。
No ROMs are included or committed; supply your own.

SD 卡格式化為 FAT，放進去即可，支援子目錄：
Format the SD card as FAT and copy files in; subdirectories work:

```
SD:/
├── disksys.rom          ← 只有要玩 .fds 才需要，見下方
├── game1.nes
├── game2.fds
└── somefolder/
    └── game3.nes
```

副檔名認得 `.nes` / `.NES` / `.fds` / `.FDS`（`RomLister.cpp:68-83`）。

開機後進入選單：

| 按鍵 | 動作 |
|---|---|
| 上 / 下 | 移動選取 |
| 左 / 右 | 翻頁 |
| **A** | 進入資料夾，或**把選取的遊戲燒進 flash 並重開機**執行 |
| **B** | 回上一層目錄 |
| **Start** | 直接執行目前已經燒在 flash 裡的遊戲 |

選取遊戲後會把映像燒進 flash（`NES_FILE_ADDR = 0x10080000`）再重開機——
重開是必要的，這樣音訊才會正確初始化。之後每次開機都會直接跑那個遊戲，
要換片再進選單。

也可以跳過 SD 卡，直接用 picotool 把 ROM 燒到同一個位址：
You can also skip the SD card and burn a ROM to that address with picotool:

```bash
picotool load yourgame.nes -t bin -o 0x10080000
```

---

## 玩 FDS 遊戲 / Playing FDS games

**需要 FDS BIOS：把 `disksys.rom` 放在 SD 卡根目錄。**
An FDS BIOS is required: put `disksys.rom` in the SD card root.

這是任天堂的版權碼，**不在也不會進本倉庫**，請自行取得。
找不到的話選單會在底部顯示 `No /disksys.rom: .fds disabled`，
且 `.fds` 檔案無法選取（`menu.cpp:296-299`）。

支援雙面以上的磁片與遊戲存檔。存檔沿用既有的 flash NVRAM 機制，
不會即時寫回 SD 卡。實作細節與已知限制見
[`fds_plan.md`](../../fds_plan.md)。

---

## 文件 / Documentation

- [`fds_plan.md`](../../fds_plan.md) — FDS 模擬的實作計畫、實測結果與已知限制
- [`claude_opus_5_analysis.md`](../../claude_opus_5_analysis.md) — 架構分析與問題清單

---

## Credits

Inspired by https://github.com/shuichitakano/pico-infones and
https://github.com/fhoedemakers/pico-infonesPlus with thanks to "infones"
https://github.com/jay-kumogata/InfoNES
