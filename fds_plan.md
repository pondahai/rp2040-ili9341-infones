# 紅白機磁碟機（Famicom Disk System）模擬實作計畫

目標專案：`software/infones/`（RP2040 + ILI9341）
擬定日期：2026-08-04
狀態：**Phase 1–5 已完成並通過實機驗證**（2026-08-04）
　　　分支 `feature/fds-phase1-2`（已併入 main）、`feature/fds-phase3`
　　　Phase 6（擴充音源）尚未動工
　　　**遺留兩個未解問題：背景垂直漂移（7.4）、磁碟動作期間的嗶聲（7.7）**

相關文件：[`claude_opus_5_analysis.md`](claude_opus_5_analysis.md)（現況架構分析）
實作結果：Phase 1、2 見第 6 節；Phase 3、4、5 見第 7 節

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
         └─> Phase 2（記憶體映射）     ✅ 【BIOS 開機畫面出現】
               └─> Phase 3（狀態機）   ✅ 已完成
                     └─> Phase 4（資料流）   ✅ 【遊戲可玩】隨 Phase 3 一併達成
                           ├─> Phase 5（換片＋存檔）✅ 已完成
                           └─> Phase 6（擴充音源）  ← 下一步（可選）
```

⚠️ 遺留：特定遊戲背景垂直漂移，見 7.4。

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

---

## 7. Phase 3、4、5 實作結果（2026-08-04）

分支 `feature/fds-phase3`。**兩款 `.fds` 遊戲都能載入並實際遊玩**，
其中一款完全正常，另一款有背景漂移（7.4）。

| commit | 內容 |
|---|---|
| `c509159` | Phase 3：磁碟控制器狀態機 |
| `ffb5404` | Phase 5：換片與存檔 |
| `43ab9a6` | `$4023` gating、計時器改用 1/3 cycle 為單位、加入診斷 |
| `0c11086` | 存檔日誌加磁片指紋、支援無檔頭 `.fds` |
| `899c618` | 捲動還原的診斷（純量測，無行為改動） |

### 7.1 Phase 3 與 Phase 4 一併達成

Phase 3 的驗收標準是「BIOS 讀出第一個 block 並通過 CRC」，
Phase 4 是「實際遊戲能開始執行」——**兩者在同一次實作就都過了**。

關鍵在於 **Phase 4 的 gap 模擬完全沒有需要**。計畫書預期要動態插入 gap
與假 CRC（約 150 行），實際上只需要一招：

> `$4025` 的 start bit 下降時，把磁頭往回退 2 bytes。

因為 BIOS 只是把 CRC 欄位**讀掉**、不驗算（計畫書第 4 節的判斷正確），
而 `.fds` 映像沒有那兩個 byte，不退就會讓每個 block 累積偏移。
這一招取代了整個 Phase 4。

### 7.2 讓 BIOS 開機所需的暫存器比預期少

Phase 2 結束時十個暫存器一個都沒實作，BIOS 仍能走到開機畫面。只需要
`$4032` 回報 `0x07`（無片）與 `$4033` 回報 `0x80`（電池正常）。
`$4033` 是**必要**的——回報電池沒電 BIOS 會拒絕開機。

`$4020–$4022` 的 IRQ 計時器原本被列為 Phase 2 的最大風險，實測不影響開機。

### 7.3 Phase 5：存檔不需要擴充 NVRAM slot

計畫書預期「FDS 存檔可能更大，需擴充 `getCurrentNVRAMAddr()` 的 slot 配置」。
實際改用**寫入日誌**避開了：一面 65500 bytes 放不進 RAM，映像在 flash 也不能
原地改，但遊戲寫回的只有一兩個小 block。每次寫入傳輸記成一筆日誌，
讀取時先查日誌再查 flash。

日誌放在 `DRAM` 扣掉 PRG RAM 與 BIOS 之後剩下的 8 KB，**尺寸剛好等於一個
NVRAM slot**，所以 slot 配置一行未改。

> ⚠️ **日誌必須綁定磁片身分。** NVRAM slot 按索引配置，單一映像永遠是 slot 0，
> 所以換一款 FDS 遊戲會繼承前一款的日誌，把舊資料覆蓋到新磁片上——
> 破壞程度足以讓 BIOS 完全讀不出磁片。`0c11086` 加了磁片指紋
> （對 side 0 前 64 bytes 做 FNV-1a，涵蓋 disk info block 裡的製造商、
> 遊戲名、版本，再加面數），對不上就整份忽略。
> **這是實際踩到並修好的迴歸，不是預防性設計。**

換片沿用 `SELECT + LEFT/RIGHT`（舊的切換 ROM 綁定已註解，位置是空的），
並完整模擬退片→空檔（約 1/3 秒）→插片。實測可以正常過「PLEASE SET DISK B」。

### 7.4 未解問題：《アルマナの奇跡》背景垂直漂移 ⚠️

**症狀**：遊戲進行中整片背景持續向上平移，主角與敵人正常。
開場動畫正常，進入遊戲後才發生。

**已排除的假設**（都有實測證據）：

| 假設 | 證據 | 結論 |
|---|---|---|
| 顯示層 GRAM 指標漂移（分析文件 2.3） | 精靈正常。若是 GRAM 漂移，整個畫面連精靈一起捲動 | 排除 |
| 這個 fork 既有的問題 | **`.nes` 遊戲載入遊玩畫面完全正常**（原記「完全正常」，7.8 證實聲音不正常，已收窄） | 排除 |
| 記憶體映射錯誤 | 開場動畫正常；映射錯會直接當掉 | 排除 |
| 鏡像極性反了 | `$4025` bit3 只在掃描線 241、254 變動（都在 vblank），且橫捲遊戲需要的垂直鏡像與實作一致 | 排除 |
| 遊戲每幀做鏡像分割 | 同上，75 秒內鏡像只變動過那幾次 | 排除 |
| 計時器 IRQ 相位漂移 | `fire=189,189,…` 連續八次相同、14 秒無變化 | **排除** |

> **記一個推理錯誤**：曾因為「漂移與 `en=02` 同步」而認定是計時器。
> 但 `en=02` 只代表「在遊戲中」，那是相關不是因果。
> 後來實測相位完全穩定，整條推理鏈作廢。

**兩款遊戲的計時器對照**（實測）：

| | latch | 換算 | 裝填 | 觸發 | 漂移 |
|---|---|---|---|---|---|
| 另一款 FDS 遊戲 | 5720 | 50.3 行 | 掃描線 241 | 29（穩定） | 無 |
| アルマナの奇跡 | 23948 | 210.7 行 | 掃描線 241 | 189（穩定） | **有** |

兩者相位都穩定，所以 latch 長短不是原因。

**尚未驗證的假設**（`899c618` 已埋好量測，但尚未取得資料）：

`InfoNES.cpp:771` 的捲動還原一幀只做一次，而且有前提：

```c
if ((PPU_R1 & R1_SHOW_SP) || (PPU_R1 & R1_SHOW_SCR))   // 必須開著算繪
{
    if (PPU_Scanline == SCAN_VBLANK_END)   // 261
        PPU_Addr = PPU_Temp;               // 唯一的還原點
    else if (PPU_Scanline < SCAN_UNKNOWN_START)
        ... 每條掃描線遞增 Y ...
}
```

若遊戲在掃描線 261 當下關閉算繪（常見於 vblank 期間安全更新 VRAM），
還原不會發生，`PPU_Addr` 的垂直分量會逐幀累積——症狀與觀察吻合，
且精靈由 SPRRAM 定位不受影響。

量測方式：在 `Map20_HSync()`（早於 `InfoNES_HSync()`）於掃描線 261 取樣
算繪旗標、`PPU_Temp` 與 `PPU_Addr` 的 Y。判讀：

- `render=00000000` → 還原被跳過，**假設成立**，且這是 fork 的既有弱點而非 mapper 問題
- `render=11111111` 且 `addrY` 逐幀遞增 → 還原有做但無效，往 loopy 邏輯查
- `tempY` 本身逐幀遞增 → 遊戲自己在捲動，往它的 IRQ handler 查

**背景資料**：《アルマナの奇跡》（Konami, 1987-08-11）是**橫向**捲動動作遊戲，
所以垂直方向的持續漂移本身就是異常。網路上查不到這款遊戲在模擬器上的
已知問題記錄。

### 7.5 已知的簡化與可能不正確之處

以下都是實作時有意識做的取捨或未經驗證的判斷，**任何一項都可能是 7.4 的成因**，
也是日後出問題時該優先檢查的清單：

| 項目 | 現況 | 風險 |
|---|---|---|
| 傳輸模型 | 需求驅動：CPU 讀走 `$4031` 才排下一個 byte | 實測 298 cycles/byte，真機 149，載入約慢一倍；且不會像真機那樣掉 byte |
| 馬達關閉回捲 | 關馬達時磁頭回到磁片起點 | 依據 Quick Disk 是線性機構的推論，**FCEUX 沒有這樣做** |
| `$4025` bit1 | 視為「暫停傳輸、磁頭不動」 | 語意來源不確定，各家文件說法不完全一致 |
| gap / CRC | 完全不生成，只靠下降緣退 2 bytes | 對兩款測試遊戲夠用，不保證通用 |
| `$4030` bit4/6/7 | CRC 錯誤、end of head、RW enable 都固定 0 | 遊戲若檢查會誤判 |
| `$4032` bit2 | 回報「可寫」，但寫入只進日誌不回磁片 | 存檔靜默失效於日誌溢位時 |
| 計時器解析度 | 每條掃描線最多觸發一次 | latch < 114 cycles 時會嚴重少觸發 |
| 計時器 repeat 模式 | 重載時保留餘數，未經實測驗證 | 兩款測試遊戲都用單發模式，repeat 路徑**從未被執行過** |
| 寫入日誌容量 | 8 筆、8 KB，溢位靜默丟棄 | 存檔較大的遊戲會失敗且無提示 |
| `$4026` | 完全忽略 | 外部連接埠，一般遊戲不用 |
| 換片空檔長度 | 固定約 1/3 秒 | 任意值，未對照真機 |
| FDS 音源 | 未實作（Phase 6） | **FDS 音樂會缺主旋律聲道，聽起來不對是預期內** |

### 7.6 診斷工具

`mapper/InfoNES_Mapper_020.cpp` 內的 `FDS_DEBUG_COUNTERS`（目前為 `1`）
每秒經 USB serial 輸出一行狀態。**問題釐清後應設回 `0`。**

讀取 serial 的方式見下（Windows 特有的兩個坑）：

- picotool 無法把板子踢進 BOOTSEL（reset interface 沒綁 WinUSB 驅動），
  但**不需要 Zadig**：用 1200 baud touch 開關 CDC 埠即可，
  `Open()` 會拋「裝置不存在」，那是成功不是錯誤
- 讀 serial **必須設 `DtrEnable = $true`**，否則 `tud_cdc_connected()`
  為假，韌體不輸出，抓到的是空的

### 7.7 磁碟動作期間的間歇尖銳嗶聲 ⚠️ 未驗證

**症狀**：磁碟讀取期間持續有間歇性的尖銳嗶嗶聲，遊戲進行中沒有。

**這可能是分析文件第 8 節寫好的觸發條件。** 該節記錄 4.2（音訊 ring buffer
無溢位保護 + 節流迴圈無逾時）暫不處理，但列了三個症狀，出現任一就該回頭實作修法。
其中第二條是「音訊出現**規律性的爆音後跳針**（缺陷一的溢位特徵）」——描述吻合。

**時機也吻合**：只在磁碟動作期間發生，而那正是本 mapper 每秒觸發約 6000 次
IRQ 的時段（實測 `dIRQ/s≈5900`）。遊戲進行中 `dIRQ/s=0`，沒有嗶聲。

**推測的機制**（未驗證）：

磁碟傳輸的 IRQ 風暴讓 core0 的模擬變慢，`InfoNES_SoundOutput()` 供應樣本的
速率跟著不穩。音訊路徑上那個沒有逾時的節流迴圈（分析文件 4.2 缺陷二）與
沒有溢位保護的 `write()`（缺陷一）在這種不穩定下就會產生規律性的破音。

**一個加重因素是本 mapper 造成的**：傳輸採需求驅動模型，實測 298 cycles/byte，
真機是 149（見 7.5）。**載入時間因此約為真機的兩倍，IRQ 風暴的持續時間也加倍。**
把傳輸改成在 byte 備妥時就開始計下一個 byte 的時間（而非等 CPU 取走），
可以把這段時間砍半，但那是行為改動，尚未進行。

**尚未驗證**，因此不確定是：

- 音訊管線在負載下的既有弱點（分析文件 4.2），與 FDS 無關，只是第一次被
  FDS 的 IRQ 風暴觸發，或
- 本 mapper 的 IRQ 行為本身有問題

**若要往下查**，第一步是量 `audioRing` 在磁碟動作期間的 `readable_size()`
是否週期性歸零（欠載）或發生 head 越過 tail（溢位）——兩者的修法不同。

### 7.8 已解決：`$40xx` 位址解碼把匣帶暫存器折回 APU ✅

**症狀**：《超級瑪利兄弟 2》（金牌瑪利）主角跳躍音效異常，
聽起來像「原本的跳躍音效被拉長」。`.fds` 與 `.nes` 兩種格式都會。
**《超級瑪利兄弟一代》所有聲音完全正常。**

**成因**（`K6502_rw.h`）：

```c
case 0x4000: /* Sound */
  switch (wAddr & 0x1f)      // ← 整個 $40xx 頁被折回 APU
```

只有 `$4000-$4017` 屬於 2A03，以上是匣帶空間。但 `0x1f` 遮罩讓：

| CPU 寫入 | `& 0x1f` | 實際打到 |
|---|---|---|
| `$4020` 計時器 latch 低 | `0x00` | **`$4000`** 方波 1 音量/duty |
| `$4021` 計時器 latch 高 | `0x01` | **`$4001`** 方波 1 sweep |
| `$4022` 計時器控制 | `0x02` | **`$4002`** 方波 1 週期低位 |
| `$4023` 主控致能 | `0x03` | **`$4003`** 方波 1 週期高位 + 長度 |

原本的 `MapperApu()` 仍有被呼叫，所以 FDS 計時器功能正常——
它只是**同時**污染 APU。FDS 遊戲每幀重設計時器（實測 `tIRQ/s=60`），
於是**方波 1 每一幀都被砸掉一次**。跳躍音效正好走方波 1。

一代是 NROM，從不寫 `$4020` 以上，完全碰不到這條路徑——
這就是「一款壞、另一款極相似的正常」的真正原因。

順帶堵掉兩個更難看的：`$4034 & 0x1f = 0x14` 原本會觸發**精靈 DMA**；
`$4040-$408F`（FDS 音源，Phase 6）整段落在 APU 上。

**修法**：`$4017` 以上一律交給 mapper，不進 APU 解碼。

**這是 fork 的既有缺陷，不是 mapper 20 造成的**，但只有使用 `$4020` 以上
的 mapper 會踩到。也因此 7.4 排除表裡「`.nes` 遊戲載入遊玩完全正常」
那一列必須收窄為「**畫面**正常」——`.nes` 的金牌瑪利（轉版 mapper 常見
解碼 `$4020`）聲音同樣是壞的。7.4 的結論不受影響（那是視訊問題）。

#### 解題過程與兩次推理錯誤

線索的價值在於**它同時出現在 `.fds` 與 `.nes`，卻不出現在一代**。
「全域性」的解釋（效能、取樣率）無法挑遊戲，一開始就該被排除；
「資料相依」這個判斷是對的，但連錯兩次成因：

| # | 假設 | 怎麼推翻的 |
|---|---|---|
| 1 | 4.1 的 12% 節流讓音效引擎變慢 | **一代所有聲音正常**。全域慢不可能挑遊戲 |
| 2 | 方波 1 sweep 無號數下溢（`ApuC?Freq` 是 `DWORD`，上掃會回捲成 `0xFFFFFFFF`，靜音判斷失效） | 補上真機的靜音保護後**聲音完全沒變**，計數器顯示每秒只擋下 1–4 次（正常音樂的下掃碰上限），數量級對不上 |

> **記一個方法論錯誤**：前兩次都建立在「跳躍音走方波 1 + sweep」這個
> **從未驗證過的前提**上，直接跳到程式碼裡找可疑之處。
> 真正解題的是放棄推理、改成量測——攔截按下 A 鍵後 24 幀內所有寫進
> `$4000-$4003` 的原始值。答案在第一眼就看出來了：每幀重複的
> `0=58 1=16 2=02` 中，`0x1658 = 5720` 正是 7.4 表格裡已經記錄過的
> 計時器 latch 值。**線索早就在文件裡，只是沒去對。**

假設 2 的 sweep 保護雖然不是本題的解，但按規格本來就該有，一併保留
（見 7.9）。

#### 量測工具踩到的坑

`InfoNES_pAPU.cpp` 的 `APU_CAP_DEBUG`（已設回 `0`）：A 鍵上升緣觸發，
錄 24 幀內 `$4000-$4003` 的所有寫入。有兩個坑值得記：

- **切換 FDS/NES 要 reset 板子，USB CDC 會重新列舉**，host 端抓著的是
  舊 handle，之後永遠讀不到東西**且不會報錯**，就是安靜。
  燒錄後也要等列舉完成再開埠（等 2 秒不夠，殘影 handle 會 open 成功）。
- 因此 debug 輸出不要設計成「一次性印出」——改成**把 trace 存著、
  每 3 秒重印一次**，就不必卡在按鍵那一刻建立連線。

### 7.9 順帶修掉的 APU 既有缺陷

查 7.8 的過程中發現，都與本題無關但確實是 bug：

| 位置 | 問題 | 影響 |
|---|---|---|
| sweep 單元（方波 1、2） | 缺真機的靜音保護：目前週期 < 8 或目標 > `$7FF` 時不該寫回週期值。`ApuC?Freq` 是 `DWORD`，上掃會下溢回捲成極大值，反而讓 `< 8` 的靜音判斷失效 | 聲道以凍結的相位增量持續發聲 |
| 六處 `ApuPulseMagic / (Freq / 2)` | `Freq == 1` 時除數為 0。RP2040 硬體除法器**不會 fault**，只回垃圾值 | 靜默的錯誤相位增量 |
| 五個聲道的靜音路徑 | `memset(…, 2 * n)`，但 `wave_buffers[5][735]` 每列僅 735 bytes，n≈416 時越界 97 bytes | 前四列被下個聲道蓋掉；DPCM 那列**直接寫出陣列之外** |


---

*Phase 1–5 已實作完成並實機驗證；Phase 6 仍為計畫。*
*7.4（捲動漂移）、7.7（磁碟嗶聲）為未解問題；7.8、7.9 已解決並實機驗證。*
