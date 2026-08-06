# 交接文件

給接手這個專案的新對話串。**先讀這份，再依指引去讀對應的文件與程式碼。**

最後更新：2026-08-04（對應 `main` 分支）

---

## 1. 一句話現況

RP2040 + ILI9341 掌機的 NES 模擬器，**紅白機磁碟機（FDS）模擬的 Phase 1–5 已完成
並通過實機驗證**，只剩 Phase 6（FDS 波表音源）未動工；另有一個症狀已消失、
但因果尚未驗證的問題待收尾。

---

## 2. 先讀哪些文件

| 文件 | 內容 | 什麼時候讀 |
|---|---|---|
| `fds_plan.md` | FDS 實作計畫**與實作結果**。第 6 節＝Phase 1、2 結果；第 7 節＝Phase 3、4、5 結果 | 要碰 FDS 就必讀 |
| `claude_opus_5_analysis.md` | 專案整體架構分析與問題清單 | 要碰顯示、音訊、時脈就必讀 |

這兩份文件裡的**第 6、7 節與問題清單是活的紀錄**，包含已排除的假設、
踩過的坑、推理錯誤的檢討。動手前先查，可以省下重複踩雷的時間。

> ⚠️ **已知過時處**：`fds_plan.md` 7.6 寫「`FDS_DEBUG_COUNTERS` 目前為 `1`」，
> 實際上已經設回 `0`（`mapper/InfoNES_Mapper_020.cpp:181`）。
> `InfoNES_pAPU.cpp:37` 的 `APU_CAP_DEBUG` 也是 `0`。

---

## 3. 程式碼地圖（FDS 相關）

**主體**

```
software/infones/mapper/InfoNES_Mapper_020.cpp    1030 行，FDS mapper 全部在這
```

檔內結構：

| 位置 | 內容 |
|---|---|
| `:28-32` | `DRAM` 佈局與 `static_assert` |
| `:56-79` | 時序常數、`$4025`/`$4030`/`$4032` 位元定義 |
| `:110-117` | 存檔日誌格式（magic、8 筆、16 bytes/筆） |
| `:181` | `FDS_DEBUG_COUNTERS` 診斷開關（**目前 0**） |
| `:571` | `Map20_Init()` — bank 指派 |
| `:668` | `Map20_Apu()` — $4020–$4026 寫入 |
| `:827` | `Map20_ReadApu()` — $4030–$4033 讀取 |
| `:907` | `Map20_HSync()` — 計時器與磁碟傳輸的時序核心 |

**整合點**

| 檔案 | 內容 |
|---|---|
| `InfoNES_Mapper.h:155-168` | FDS 常數與對外 API 宣告 |
| `InfoNES_Mapper.cpp` | `MapperTable` 加 `{20, Map20_Init}`＋一行 `#include` |
| `main.cpp:619` | `parseFDS()` — 合成假 iNES header |
| `main.cpp:343-397` | 存檔／載入日誌（接在既有 NVRAM 機制上） |
| `main.cpp:545-559` | `SELECT + LEFT/RIGHT` 換片 |
| `menu.cpp` / `menu.h` | BIOS 缺席提示、擋下燒錄 |
| `rom_selector.h` | 辨識 FDS magic，避開 TAR 解析 |
| `RomLister.cpp` | `.fds` 副檔名 |

**順帶修掉的既有缺陷（與 FDS 無關，但別誤刪）**

| 檔案 | 內容 |
|---|---|
| `K6502_rw.h:370` | `$4017` 以上一律交給 mapper，不再用 `& 0x1f` 折回 APU（見 `fds_plan.md` 7.8） |
| `InfoNES_pAPU.cpp` | sweep 靜音保護、除以零、`memset` 越界（見 7.9） |

---

## 4. 待辦事項（建議順序）

### 4.1 補 7.10 的因果驗證 ⚠️ 建議優先

修掉 7.8 的位址解碼後，7.4（背景垂直漂移）與 7.7（磁碟嗶聲）的症狀都消失了，
**但 7.4 為何跟著好，沒有經過驗證的解釋**——`$4020→$4000` 的污染只涉及 APU，
推不出視訊症狀。

`fds_plan.md` 7.10 已經寫好補證方法：單獨還原 7.8 的修改（保留 7.9），
在 `K6502_rw.h` 的 `case 0x14` 加計數器，看《アルマナの奇跡》遊玩期間
精靈 DMA 被誤觸幾次、漂移是否隨之回來。

**在補上證據之前，不要把「解碼修好」當成 7.4 的已知成因來引用。**

### 4.2 Phase 6：FDS 波表音源

`fds_plan.md` 第 2 節 Phase 6 有完整規劃。兩條路線（改 `InfoNES_SoundOutput()`
簽章加 `wave6`／混進 `wave5`），建議前者。

> **為什麼建議排在 4.1 之後**：Phase 6 要動 `$4040-$408F`，
> 正好是 7.10 裡「`$4054`／`$4074` 誤觸精靈 DMA」的那段位址。
> 因果還沒釐清就去動，出問題會分不清是新的還是舊的。

### 4.3 分析文件 4.2：音訊 ring buffer

`write()` 無溢位保護 ＋ 節流迴圈無逾時，最壞情況是整機凍結需斷電。
修法（約十行）已寫在 `claude_opus_5_analysis.md` 4.2 節。
狀態已從「暫不處理」改為「待修」。

### 4.4 其他

- `claude_opus_5_analysis.md` 第 4 節還有 4.4（SPI 同步防護）、4.5（節流機制重複）等未處理項
- `fds_plan.md` 7.5 列了 12 項實作時的簡化與取捨，是日後出問題的優先檢查清單

---

## 5. 環境限制（很重要）

**雲端工作階段無法編譯，也無法燒錄。**

- 這個環境沒有 pico-sdk。若要驗證能編譯過，得先自行取得 SDK
- 無法燒錄、無法讀 USB serial、無法看畫面
- **所有實機驗證只有使用者能做**——包含 serial log、畫面症狀、聲音症狀

因此：交出的程式碼預設是「未經實機驗證」。請明確說出哪些部分沒驗證過，
不要用「已修復」描述只在腦中推導過的改動。

讀 serial 的方法（Windows 特有的坑）記在 `fds_plan.md` 7.6，
使用者要自己操作時可以指給他看。

---

## 6. 專案慣例與地雷

**mapper 不是獨立編譯單元**
新增 mapper 只要三步：建檔、`MapperTable` 加一列、`InfoNES_Mapper.cpp` 加一行
`#include`。**不需要改 `CMakeLists.txt`**（`fds_plan.md` 6.2）。

**診斷開關用完要設回 0**
`FDS_DEBUG_COUNTERS`、`APU_CAP_DEBUG`。debug 輸出設計成「每 N 秒重印」
而非「一次性印出」，因為 USB CDC 重新列舉時 host 端會抓到舊 handle
且不會報錯（`fds_plan.md` 7.8 末）。

**記憶體是共用的**
`DRAM[0xA000]`（40 KB，`InfoNES_Mapper.cpp:24`）現在被 FDS 佔用：
PRG RAM 24 KB ＋ BIOS 8 KB ＋ 存檔日誌 8 KB，剛好用滿。
新功能要用記憶體前先確認沒撞到。

**顯示層有一個精確的行數約定**
每幀必須剛好送 232 行對齊 LCD 視窗（`claude_opus_5_analysis.md` 2.3）。
動 `PPU_ScanTable` 或 `InfoNES_HSync()` 的行數判斷會讓畫面滾動。

**音訊取樣率是速度節流器，不是筆誤**
`audio_init(7, 19654)` 對上 APU 的 22050 Hz，差的 12% 是刻意的
（`claude_opus_5_analysis.md` 4.1 有完整的 git 考證）。**不要「順手修正」成 22050。**

**BIOS 是版權碼**
`disksys.rom` 放 SD 卡根目錄，**不可入 repo**。`.gitignore` 已涵蓋。

---

## 7. 開新對話串的提示詞

貼這段就能接上：

```
專案 pondahai/rp2040-ili9341-infones。

請先讀 repo 根目錄的 HANDOVER.md，再依它的指引讀 fds_plan.md
與 claude_opus_5_analysis.md 的相關章節。

這次要做的是：<填入 HANDOVER.md 第 4 節的其中一項>

從 main 開新分支。無法編譯與實機驗證沒關係，
但請明確標示哪些部分沒有驗證過。
```

要接續已完成的實機測試時，記得補上**測試結果**——那是雲端工作階段永遠不知道、
只有使用者能提供的資訊。例如：

```
Phase 6 的 wave6 已接上，實機聽起來主旋律有出來但音量偏小，
且進入遊戲後偶爾破音。serial log 如下：<貼上>
```

---

## 8. 分支與歷史

- 開發都在 `main`
- FDS 相關的實作歷史在 `feature/fds-phase1-2`、`feature/fds-phase3`（皆已併入）
- 文件類變更走 PR 再合併，commit 訊息保持可追溯

`git log --oneline` 看得到完整脈絡；`fds_plan.md` 第 6、7 節的表格
也列出了各階段對應的 commit。
