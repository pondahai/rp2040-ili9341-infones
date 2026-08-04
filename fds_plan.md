# 紅白機磁碟機（Famicom Disk System）模擬實作計畫

目標專案：`software/infones/`（RP2040 + ILI9341）
擬定日期：2026-08-04
狀態：**Phase 1、2 已完成並通過實機驗證**（2026-08-04，分支 `feature/fds-phase1-2`）
　　　Phase 3 以後尚未動工

相關文件：[`claude_opus_5_analysis.md`](claude_opus_5_analysis.md)（現況架構分析）
實作結果：見文末第 6 節

---

## 0. 結論摘要

**可行，而且比預期容易**——關鍵在於 InfoNES 既有的 bank 指標設計剛好對得上 FDS 的
記憶體佈局，且 mapper 掛鉤介面已經齊全，不需要修改 6502 核心。

主要工作量集中在**磁碟控制器狀態機**（Phase 3）與**時序處理**（本專案特有的難點）。

| 項目 | 結論 |
|---|---|
| 記憶體 | ~~只需新增 **24 KB**~~ → 實測**完全不需新增**，見 6.1 ✅ |
| Flash | 四面 `.fds` = 262 KB，餘裕 1.5 MB ✅ |
| CPU 核心改動 | **不需要** ✅ |
| InfoNES 現成支援 | **完全沒有**，mapper 20 需從零實作 ⚠️ |
| 最大難點 | HSync 粒度不足以做精確磁碟時序 ⚠️ |
| 法律限制 | FDS BIOS 為任天堂版權碼，不可入 repo ⚠️ |

---

## 1. 可行性查證

### 1.1 記憶體：~~只需要多 24 KB~~ → 一點都不用多

> ⚠️ **本節的結論在實作時被推翻，實際不需要新增任何記憶體。**
> 下表保留原始評估，修正見 6.1。

FDS 的 RAM 是 $6000–$DFFF 共 32 KB，但本專案已經有大半可以沿用：

| 位址區段 | 現況 | FDS 需求 |
|---|---|---|
| $6000–$7FFF | `SRAM[0x2000]`（`InfoNES.cpp:78`）已存在 | 直接沿用 |
| $8000–$DFFF | `ROMBANK[0..2]` 三個指標 | ~~**需新增 24 KB 陣列**~~ 沿用 `DRAM[]` |
| $E000–$FFFF | `ROMBANK[3]` | ~~指向 flash 裡的 BIOS~~ 改放 `DRAM[]`，見 6.3 |
| CHR RAM 8 KB | `PPURAM[0x4000]`（16 KB）已含 | 直接沿用，同無 VROM 的卡帶 |

現有記憶體用量粗估（未實際編譯，僅靜態估算）：

```
ChrBuf        32 KB      InfoNES.cpp:81   ← 最大宗
PPURAM        16 KB      InfoNES.cpp:91
RAM            8 KB      InfoNES.cpp:69
SRAM           8 KB      InfoNES.cpp:78
audioRing      8 KB      main.cpp:133
wave_buffers   3.6 KB    InfoNES_pAPU.cpp:97
snd_buf        2 KB      main.cpp:127
scanline bufs  1.9 KB    main.cpp:111-113
SPRRAM        256 B      InfoNES.cpp:102
audio/mixer    6 KB      audio.c
FatFs + 其他   ~4 KB
                     ─────────
                     約 90 KB / 264 KB
```

> `DoubleFrame[2][256*240]`（245 KB）在 `InfoNES.cpp:220` 被 `#if 0` 掉了，
> 這是 FDS 有空間可用的直接原因——若那個陣列還在，本計畫不可行。
>
> 注意 `ChrBuf`（32 KB，`InfoNES.cpp:81`）**是有配置的**，
> `InfoNES.cpp:236` 那行註解掉的只是舊的重複宣告，不要誤判成沒佔記憶體。

即使把 FDS 的 24 KB 加上去也只到約 114 KB，仍有 150 KB 餘裕。

> **上面這份清單漏了 `DRAM[0xA000]`（40 KB，`InfoNES_Mapper.cpp:24`）。**
> 這個疏漏反而是好消息——那 40 KB 本來就已經配置，而且正是為 Disk System 準備的，
> 見 6.1。實測 `bss` 為 190,728 bytes（約 186 KB / 264 KB），比這裡的靜態估算高不少。

### 1.2 Mapper 掛鉤介面已經齊全

`InfoNES.h:254-264` 定義了完整的函式指標介面：

```c
extern void (*MapperWrite)(WORD wAddr, BYTE byData);
extern void (*MapperSram)(WORD wAddr, BYTE byData);
extern void (*MapperApu)(WORD wAddr, BYTE byData);
extern BYTE (*MapperReadApu)(WORD wAddr);
extern void (*MapperVSync)();
extern void (*MapperHSync)();
```

FDS 暫存器落在 $4020–$4026 與 $4030–$4033，而 `K6502_rw.h:454-462` 的寫入分派是：

```c
if (wAddr <= 0x4017) { APU_Reg[wAddr & 0x1f] = byData; }
else                 { MapperApu(wAddr, byData); }   // ← FDS 暫存器從這裡進來
```

讀取端 `K6502_rw.h:167` 同樣有 `MapperReadApu(wAddr)` 的出口。

**結論：不需要動 CPU 核心，全部可在新的 mapper 檔案裡完成。**

### 1.3 InfoNES 沒有任何 FDS 支援

`InfoNES_Mapper.cpp:30` 的 `MapperTable` 收錄 136 個 mapper，**獨缺 20 號**
（表中從 19 直接跳到 21）。這是從零實作，不是移植。

### 1.4 Flash 空間

`NES_FILE_ADDR = 0x10080000`（`main.cpp:241`），即 512 KB 偏移處。
2 MB flash 尚餘 1.5 MB；四面 `.fds` 映像為 4 × 65500 = 262 KB。

---

## 2. 分階段實作計畫

### Phase 0：前置決策（不寫程式）

必須先定案，否則後續會返工。

**決策一：BIOS 來源**
FDS 必須有 8 KB 的 `disksys.rom`，這是任天堂的版權碼，**不可包進 repo**。
建議：放在 SD 卡根目錄，開機時偵測；找不到就在 menu 顯示明確提示。

**決策二：檔案格式**
建議只支援 `.fds`（fwNES 格式：16 byte header + 每面 65500 bytes），不處理 `.qd`。

**決策三：存檔策略**
FDS 遊戲會寫回磁碟，這是它與卡帶最大的差別。
建議沿用現有的 flash NVRAM 機制（`main.cpp:337` `saveNVRAM()`），
而非遊戲中即時寫 SD——理由見第 3 節風險一。

---

### Phase 1：載入路徑

**工作量**：約 100 行

| 檔案 | 改動 |
|---|---|
| `RomLister.cpp:69-73` | 副檔名過濾加入 `.fds` / `.FDS` |
| `menu.cpp:575-640` | 燒錄流程本身不需改（逐 sector 複製，格式無關），但需記錄「這是 FDS 映像」 |
| `main.cpp:564` `parseROM()` | 新增分支：偵測 `FDS\x1a` magic 或 65500 的整數倍長度 |

`parseROM()` 目前硬性要求 iNES magic（`checkNESMagic`）。FDS 路徑需要
**合成一個假的 `NesHeader`**：mapper 設為 20、`byRomSize` 與 `byVRomSize` 設為 0，
好讓 InfoNES 既有的初始化流程能夠跑完。

**驗收標準**：能在 menu 看到 `.fds` 檔案、選取後重開機不當機（黑屏可接受）。

> ✅ **已達成**（2026-08-04）。實機 serial log：
>
> ```
> Single FDS disk image, 2 side(s).
> FDS BIOS loaded from /disksys.rom
> Now playing: Armana no Kiseki (Japan)
> FDS disk image: 2 side(s) at 10080010
> ```
>
> `10080010` = `NES_FILE_ADDR + 16`，確認 fwNES header 被正確跳過。

---

### Phase 2：記憶體映射

**工作量**：約 80 行，新檔 `mapper/InfoNES_Mapper_020.cpp`

原始構想（**實作時有調整，見下方**）：

```c
static BYTE FDS_PrgRam[0x6000];   // $8000-$DFFF，24 KB
static BYTE *FDS_Bios;            // 指向 flash 裡的 BIOS，不複製

void Map20_Init(void) {
    ROMBANK0 = FDS_PrgRam + 0x0000;   // $8000
    ROMBANK1 = FDS_PrgRam + 0x2000;   // $A000
    ROMBANK2 = FDS_PrgRam + 0x4000;   // $C000
    ROMBANK3 = FDS_Bios;              // $E000  ← XIP，唯讀
    InfoNES_SetupChr();               // CHR RAM 走 PPURAM
}

void Map20_Write(WORD addr, BYTE data) {   // $8000-$FFFF 的寫入
    if (addr < 0xE000) FDS_PrgRam[addr - 0x8000] = data;
    // $E000 以上是 BIOS，忽略寫入
}
```

實際採用的版本改用既有的 `DRAM[]`，且 `InfoNES_SetupChr()` 不需要呼叫
（`byVRomSize == 0` 時 `InfoNES_SetupPPU()` 已經把 `PPUBANK[]` 指好了，
Map0 也只在有 VROM 時才呼叫它）：

```c
#define FDS_PRGRAM_OFFSET 0x0000   // DRAM[0x0000..0x5FFF] → $8000-$DFFF
#define FDS_BIOS_OFFSET   0x6000   // DRAM[0x6000..0x7FFF] → $E000-$FFFF

ROMBANK0 = &DRAM[FDS_PRGRAM_OFFSET + 0x0000];
ROMBANK1 = &DRAM[FDS_PRGRAM_OFFSET + 0x2000];
ROMBANK2 = &DRAM[FDS_PRGRAM_OFFSET + 0x4000];
ROMBANK3 = &DRAM[FDS_BIOS_OFFSET];
```

同時在 `InfoNES_Mapper.cpp:30` 的 `MapperTable` 補上 `{20, Map20_Init}`，
並在 `InfoNES_Mapper.h` 宣告。

這是整個計畫裡最順的一段——InfoNES 的 bank 指標粒度（8 KB）
剛好對上 FDS 的記憶體佈局。

**驗收標準**：**BIOS 開機畫面出現**（轉圈的 Disk System logo）。
這是最重要的里程碑，代表 CPU、記憶體、PPU 三者全部接通。

> ✅ **已達成**（2026-08-04）。實機上磁碟系統畫面與聲音都正常出現。
> 有聲音是額外的訊號——那是 NES 內建 APU，代表 pAPU 在 mapper 20 下也正常
> （FDS 專屬波表音源仍在 Phase 6）。
>
> 讓 BIOS 走到這一步所需的暫存器回報比預期少：$4030–$4033 只要回報
> 「磁碟機沒插片（`0x07`）+ 電池正常（`0x80`）」即可，
> **$4020–$4022 完全沒實作也不影響**——見 6.2。

---

### Phase 3：磁碟控制器狀態機

**工作量**：約 250 行 —— **本計畫最難的一段**

需實作的暫存器：

| 位址 | 方向 | 功能 |
|---|---|---|
| $4020–$4021 | W | IRQ 計時器重載值（低/高位元組） |
| $4022 | W | IRQ 計時器控制 |
| $4023 | W | 主控制（啟用磁碟 I/O、音源） |
| $4024 | W | 寫入資料埠 |
| $4025 | W | FDS 控制（馬達、傳輸方向、CRC、IRQ 致能） |
| $4026 | W | 外部連接埠 |
| $4030 | R | 狀態（傳輸完成旗標、CRC 錯誤、磁碟就緒） |
| $4031 | R | 讀取資料埠 |
| $4032 | R | 磁碟狀態（有無插片、防寫、就緒） |
| $4033 | R | 電池狀態 |

核心是一台狀態機：馬達轉動 → 逐 byte 送出磁碟資料 → 每 byte 觸發一次 IRQ →
BIOS 讀走。時序掛在 `MapperHSync()` 上。

> ### ⚠️ 本專案特有的難點：時序粒度
>
> `MapperHSync()` 每條掃描線呼叫一次，即每 ~113 個 CPU cycle 才有一次解析度。
> 真機 FDS 每 149 個 cycle 傳送一個 byte——粒度勉強足夠，但必須在每次 HSync
> 裡補送多個 byte 並累積小數誤差。
>
> **建議一開始就採用「每 HSync 批次送 N bytes」的模型，不要嘗試 cycle-accurate。**
> 如果 BIOS 卡在讀取畫面不動，八成是這裡。

IRQ 觸發使用 `K6502.h:51` 的 `IRQ_REQ` 巨集。

> **Phase 2 完成後的補充**：$4020–$4022 的 IRQ 計時器原本被列為「可能擋住 Phase 2
> 驗收」的風險，實測**沒有**——BIOS 不靠它就能走完開機流程。但 Phase 3 仍然必須實作它，
> 因為磁碟傳輸的時序依賴它。目前 `Map20_Apu()` 對這三個位址是空的 `default` 分支，
> `Map20_HSync()` 也是空的，兩處都留了註解標示 Phase 3 的接手點。

**驗收標準**：BIOS 能讀出磁碟第一個 block 並通過 CRC 檢查。

---

### Phase 4：磁碟資料流與 gap 模擬

**工作量**：約 150 行

`.fds` 檔案存放的是「已解析的 block」，不含真實磁碟的 gap 與 CRC。
送資料時需要**動態插入**：

- 每個 block 前補 gap + 起始標記（block start mark）
- 每個 block 後補兩個 byte 的假 CRC

BIOS 只檢查 CRC 欄位是否存在，不會實際驗算，因此填 0 即可。

**驗收標準**：實際遊戲能開始執行。
建議測試片：《Super Mario Bros.》或《The Legend of Zelda》單面版。

---

### Phase 5：換片 UI 與存檔

**工作量**：約 200 行

**換片**
多數 FDS 遊戲會要求 A 面 / B 面切換。建議沿用現有的組合鍵慣例——
`main.cpp:419-433` 已有 `SELECT + LEFT/RIGHT` 切換 ROM 的前例，
改為切換磁碟面。

> 切換時**必須模擬「退片 → 空檔 → 插片」的完整狀態轉換**（$4032 的
> 磁碟就緒位元要先變成「無片」再變回「有片」）。直接把資料換掉會讓 BIOS 卡住。

**存檔**
記錄被寫過的 block，於 `saveNVRAM()` 時機一併寫入 flash。
注意現有 NVRAM slot 大小為 `SRAM_SIZE`（8 KB，`InfoNES.h:23`），
FDS 存檔可能更大，需擴充 `getCurrentNVRAMAddr()`（`main.cpp:319`）的 slot 配置。

**驗收標準**：存檔後斷電重開，遊戲進度仍在。

---

### Phase 6：擴充音源（可選）

**工作量**：約 200 行

FDS 內建一個 6 bit 波表音源。目前 `InfoNES_SoundOutput()` 的簽章固定為五個聲道
（`InfoNES_System.h:72`）：

```c
void InfoNES_SoundOutput(int samples, BYTE *wave1, ..., BYTE *wave5);
```

兩條路線：

| 做法 | 優點 | 缺點 |
|---|---|---|
| **改簽章加 `wave6`**（建議） | 乾淨、聲道獨立 | 需同步改 `InfoNES_System.h:72`、`InfoNES_pAPU.cpp:1189`、`main.cpp:626` 三處 |
| 混進 `wave5`（DPCM 通道） | 不動介面 | 兩者互相干擾 |

> **這一階段一定要排在最後。** 音訊路徑上有手工調校的節流器
> （見分析文件 4.1 的取樣率調校史），先把畫面跑起來，最後再碰聲音。

**驗收標準**：FDS 專屬音效正確發聲，且幀率掉幅可接受。

---

## 3. 風險與注意事項

### 風險一：不要在遊戲執行中寫 SD 卡 ⚠️ 高

SD 在 spi1、LCD 在 spi0，匯流排雖然分開，但 `f_write()` 會阻塞數十毫秒。
而 core0 主迴圈裡有那個**沒有逾時的音訊節流迴圈**（分析文件 4.2）——
阻塞夠久就會觸發已記錄在案的凍結路徑。

**這是 4.2 從「理論問題」變成「實際問題」的最可能途徑。**

因此：
- 預設採用 flash NVRAM 存檔（Phase 0 決策三）
- **若最終決定要在遊戲中寫 SD，Phase 5 之前必須先修掉 4.2**

### 風險二：`menu.cpp` 借用 `PPURAM` 當燒錄緩衝 ⚠️ 中 → ✅ 未發生

`menu.cpp:592` 以 `InfoNes_GetPPURAM(&bufsize)` 取得 PPURAM 當作 flash 寫入暫存區。
FDS 映像燒錄可沿用同一招，但需確認此時 FDS 的 24 KB PRG RAM 尚未初始化，
否則兩者會互相踩踏。

> **實作後確認不成立。** 兩者用的是不同陣列（燒錄用 `PPURAM`，FDS PRG RAM 在 `DRAM`），
> 而且 menu 燒錄完成後會透過 watchdog 重開機，`Map20_Init()` 是重開後才執行的，
> 時間上也不重疊。燒錄流程完全沒有修改。

### 風險三：時序粒度 ⚠️ 中

見 Phase 3 的說明。這是最可能卡關的技術點，建議預留額外除錯時間。

### 風險四：效能餘裕 ℹ️ 低

FDS 的 mapper 邏輯僅在磁碟 I/O 時運作，對每幀成本影響很小。
但擴充音源會在每個取樣點多一次計算，而目前 SPI 已佔用 90% 的幀時間
（分析文件第 3 節），**Phase 6 可能使幀率再掉幾格**。
這是把它排在最後的另一個理由。

---

## 4. 執行順序

```
Phase 0（決策）                        ✅ 已定案
   └─> Phase 1（載入路徑）             ✅ 已完成
         └─> Phase 2（記憶體映射）     ✅ 【BIOS 開機畫面出現】← 目前位置
               └─> Phase 3（狀態機）   ← 下一步
                     └─> Phase 4（資料流）   ← 【遊戲可玩】
                           ├─> Phase 5（換片＋存檔）
                           └─> Phase 6（擴充音源，可選）
```

兩個關鍵里程碑：

- **Phase 2 結束**：看到 BIOS 開機畫面，代表核心接通，是第一個真正有意義的訊號
- **Phase 4 結束**：遊戲能跑，功能已具雛形

Phase 5 與 6 屬於體驗完善，可獨立排程。

---

## 5. 新增與修改檔案清單

**新增**

```
software/infones/mapper/InfoNES_Mapper_020.cpp    FDS mapper 與磁碟控制器
```

**修改**

| 檔案 | 位置 | 內容 |
|---|---|---|
| `InfoNES_Mapper.cpp` | `:30` | `MapperTable` 加入 `{20, Map20_Init}` |
| `InfoNES_Mapper.h` | — | 宣告 `Map20_Init` |
| `RomLister.cpp` | `:69-73` | 副檔名加入 `.fds` |
| `main.cpp` | `:564` | `parseROM()` 加 FDS 分支 |
| `main.cpp` | `:319` | NVRAM slot 配置擴充（Phase 5） |
| `main.cpp` | `:419` | 換片組合鍵（Phase 5） |
| `menu.cpp` | `:575` | ~~FDS 映像標記~~ → 改為 BIOS 缺席提示與阻擋燒錄 |
| ~~`CMakeLists.txt`~~ | ~~`:25`~~ | ~~加入新的 mapper 原始檔~~ **不需要**，見 6.2 |
| `rom_selector.h` | — | （計畫未列到）辨識 FDS magic，避免走 TAR 解析 |
| `menu.h` | — | （計畫未列到）`FDS_BIOS_FILE` 常數 |
| `InfoNES_System.h` | `:72` | `wave6` 參數（Phase 6，可選） |
| `InfoNES_pAPU.cpp` | `:1189` | 同上 |

---

---

## 6. Phase 1、2 實作結果（2026-08-04）

分支 `feature/fds-phase1-2`。**驗收標準「BIOS 開機畫面出現」已在實機達成**，
磁碟系統畫面與聲音都正常。

| commit | 內容 |
|---|---|
| `5fe12d6` | FDS Phase 1+2 |
| `32aaa04` | `.gitignore`（BIOS、磁碟映像、build 產出） |
| `35c63d2` | 補上 9 個從未被 commit 的 mapper 檔（既有問題，與 FDS 無關） |
| `06f13a9` | `CMakeLists.txt` 的 `PICO_SDK_PATH` 修正（分析文件 4.7） |

### 6.1 出入一：不需要新增 24 KB，實測成本 4 bytes

`InfoNES_Mapper.cpp:24` 早就有 `BYTE DRAM[DRAM_SIZE]`，`DRAM_SIZE` 為 `0xA000`
（40 KB，`InfoNES_Mapper.h:22`），註解寫的正是 *Disk System RAM*——這是上游 InfoNES
為 FDS 預留的，目前只有 mapper 235 借去用。FDS 需要的 $8000–$FFFF 共 32 KB
剛好放得進去：

```
DRAM[0x0000..0x5FFF]  →  $8000-$DFFF  PRG RAM  24 KB
DRAM[0x6000..0x7FFF]  →  $E000-$FFFF  BIOS      8 KB
DRAM[0x8000..0x9FFF]  →  未使用        8 KB
```

實測（`arm-none-eabi-size`，Release）：

| | text（flash） | bss（RAM） |
|---|---|---|
| `main` | 351,900 | 190,728 |
| Phase 1+2 | 353,540 | 190,732 |
| **差** | **+1,640 B** | **+4 B** |

那 4 bytes 是幾個 static 旗標與指標。第 1.1 節預算的 24 KB 完全省下。

### 6.2 出入二：`CMakeLists.txt` 不需要修改

第 5 節列了「`CMakeLists.txt:25` 加入新的 mapper 原始檔」，實際上不需要——
mapper 是被 `#include` 進 `InfoNES_Mapper.cpp` 的（見該檔 `:175` 起的一長串
`#include "mapper/..."`），不是獨立的編譯單元。新增 mapper 只要三處：
建立檔案、`MapperTable` 加一列、加一行 `#include`。

（`06f13a9` 確實動了 `CMakeLists.txt`，但那是修 SDK 路徑，與 FDS 無關。）

### 6.3 BIOS 放在 RAM 而非 flash

第 1.1 節原本規劃 `ROMBANK3` 指向 flash 裡的 BIOS 走 XIP、零複製。
實作改成開機時從 SD 讀 8 KB 進 `DRAM[0x6000]`，理由是 menu 的燒錄迴圈
以 16 KB 為單位串流寫入，要在中間插一段 BIOS 會把那段邏輯弄複雜；
而 `DRAM` 反正已經存在。副作用是 $E000–$FFFF 的存取比 XIP 更快。

BIOS 載入時機在 `initSDCard()` 成功之後、模擬器啟動之前，每次開機一次。
找不到 `/disksys.rom` 時不視為錯誤：menu 底部顯示紅字提示，
且選取 `.fds` 會被擋下並說明原因，不會燒錄後重開機進黑屏。

### 6.4 讓 BIOS 開機所需的暫存器比預期少

Phase 3 的十個暫存器一個都沒實作，BIOS 仍能走完開機流程。只需要：

- `$4032` 回報 `0x07`（沒插片、未就緒、防寫）
- `$4033` 回報 `0x80`（電池正常）——這個**必要**，回報電池沒電 BIOS 會拒絕開機
- `$4025` bit3 的鏡像切換（屬於記憶體映射，含在 Phase 2）

`$4020–$4022` 的 IRQ 計時器原列為 Phase 2 的最大風險，實測不影響開機畫面。

### 6.5 順帶修掉的既有問題

兩個與 FDS 無關、但擋住建置的問題：

1. **repo 少了 9 個 mapper 檔**（182/183/185/187/188/189/191/193/194）。
   `InfoNES_Mapper.cpp` include 了它們，但從未被 commit，**任何人 clone 都編不起來**。
   已從作者本機工作區還原（兩邊共有檔案 byte 完全相同）。
2. **`CMakeLists.txt:10` 的 `set(PICO_SDK_PATH ...)` 沒加 `CACHE`**，
   會蓋掉 `-D` 與環境變數（分析文件 4.7）。已加上 `if(NOT DEFINED ...)` 保護。

### 6.6 下一步

Phase 3（磁碟控制器狀態機）尚未開始。接手點：

- `Map20_Apu()` 的 `default` 分支——$4020–$4022、$4024、$4026
- `Map20_ReadApu()` 的 `$4030`/`$4031`——目前回傳 `0x00`
- `Map20_HSync()`——目前是空的，時序掛在這裡
- `FDS_SetDiskImage()` 已把映像指標與面數存好，但 Phase 2 還沒有人讀它

---

*Phase 1、2 已實作完成；Phase 3 以後仍為計畫。*
