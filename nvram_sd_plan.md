# 存檔搬到 SD 卡 / NVRAM on the SD card

把遊戲存檔（卡帶 SRAM 與 FDS 磁碟日誌）從 flash 搬到 SD 卡上的檔案。

**狀態：未完成。輔助函式與主機端測試已經寫好，但沒有接進韌體。**
起草於 2026-08-05，本文件整理於 2026-08-15。

---

## 1. 為什麼要做

現況是存檔寫在 flash，位址由 `getCurrentNVRAMAddr()` 算出來
（`main.cpp:349`）：

```c
return NES_FILE_ADDR - SRAM_SIZE * (slot + 1);
```

這個設計有三個問題，第一個最嚴重：

### 1.1 實際上只有一個存檔槽，換遊戲就覆蓋

選單燒錄一個遊戲之後，`romSelector_.init()` 認出 NES/FDS magic，走
`singleROM_` 那條路（`rom_selector.h`），於是 `getCurrentNVRAMSlot()`
**永遠回傳 0**。

所以不管玩過多少款遊戲，存檔都寫在同一個位址。**換一款遊戲再存檔，
上一款的進度就沒了**，而且不會有任何提示。

多槽只在 TAR 包多個 ROM 時才會發生，那不是選單的日常路徑。

### 1.2 槽號跟 ROM 在清單裡的位置綁在一起

多 ROM 模式下，slot 是「在我前面有幾個有 NVRAM 的 ROM」算出來的
（`rom_selector.h` 的 `getCurrentNVRAMSlot()`）。TAR 內容一改，
所有槽號都會位移，存檔就對不上遊戲了。

### 1.3 槽位往下長，會撞到 image 尾巴

存檔槽從 `NES_FILE_ADDR` 往**低位址**長。而 image 的尾巴往**高位址**長。
兩者朝對方靠近：

| | image 結束 | slot0 | slot1 |
|---|---|---|---|
| 預設編譯 | `0x1007d078` | `0x1007e000` | `0x1007c000` ← **已經在 image 裡** |
| 偏移編譯 | `0x10080f78` | `0x10082000` | `0x10080000` ← **同樣在 image 裡** |

slot 0 目前安全（餘裕約 4 KB），但 slot 1 以上會直接踩進程式碼。
現在沒出事只是因為 §1.1 —— 實務上永遠只用 slot 0。

`check_flash_layout.cmake`（2026-08-15 加入）會擋住 image 尾巴撞上 slot 0，
但擋不住「多槽」這個設計本身。

**搬到 SD 卡之後，這三個問題一起消失**：每個遊戲一個檔案，不用配置槽位、
不佔 flash、不磨損 flash、也不會跟 image 打架。

---

## 2. 現況盤點

### 2.1 已經寫好的（未入版控，在工作區）

| 檔案 | 內容 |
|---|---|
| `software/infones/nvram_path.h` | `buildNVRAMPath()`：遊戲名 → `/SAVES/<name>.SAV`（FDS 用 `.FSV`）。刻意不依賴 pico，主機端測得到 |
| `software/tools/ramdisk_fat12.h` | 手工排版的 FAT12 RAM disk。手工排是為了讓 `ffconf.h` 維持韌體出貨的樣子（`FF_USE_MKFS` 保持 0，韌體不該有格式化能力） |
| `software/tools/nvram_save_test/` | 主機端回歸測試，結構比照已入版控的 `fatfs_utf8_test/` |

測試驗兩件事（見 `nvram_save_test/main.c` 檔頭）：

1. **路徑產生** — UTF-8 中文名原樣通過、`.SAV`／`.FSV` 要分開
   （`game.nes` 和 `game.fds` 都會變成 `game`，但兩個 8 KB blob 不能互換）、
   **絕不截斷**（切斷 UTF-8 多位元組序列會產生壞檔名）
2. **寫入序列** — 先寫固定的暫存檔名再 rename 就位，
   **中途斷電只會賠掉新存檔，不會弄丟舊的**

`Makefile` 直接編譯韌體自己的 `nvram_path.h` 與 `ff.c`／未修改的 `ffconf.h`，
不複製任何東西，所以不會跟韌體脫節。

> ⚠️ 2026-08-15 的工作階段**沒有跑過這個測試**（本機 PATH 上沒有主機端
> C 編譯器，當初編好的執行檔也無法在 Git Bash 下執行）。
> 它是否通過**尚未在本階段確認**。

### 2.2 韌體端的現況

- `saveNVRAM()`（`main.cpp:367`）與 `loadNVRAM()`（`main.cpp:416`）
  **仍然只走 flash**
- `main.cpp:234` 有一份自己的 `#define GAMESAVEDIR "/SAVES"`，
  跟 `nvram_path.h` 裡那份**重複**
- `initSDCard()` 會**建立** `/SAVES` 目錄（`main.cpp:1702`），
  但目前沒有任何東西寫進去 —— 這是當初動工留下的唯一痕跡
- 遊戲名有現成的來源：`selectedRom[80]`，
  由選單寫進 `currentloadedrom.txt`、開機時讀回，副檔名已經去掉

**所以缺的就是最後一哩：把 `saveNVRAM()`／`loadNVRAM()` 接到檔案上。**

---

## 3. 設計

### 3.1 路徑

```
/SAVES/<遊戲名>.SAV     卡帶 SRAM（8 KB）
/SAVES/<遊戲名>.FSV     FDS 磁碟日誌（8 KB）
/SAVES/~WRITING.TMP     寫入中的暫存檔（固定名稱）
```

遊戲名用 `selectedRom`，UTF-8 原樣通過（FatFs 設定為 `FF_LFN_UNICODE 2`）。

暫存檔用**固定名稱**而不是「原名 + 後綴」，是為了讓長遊戲名永遠不會
讓暫存路徑成為放不下的那一個。

### 3.2 寫入序列（斷電安全）

```
f_open(NVRAM_TEMP_PATH, CREATE_ALWAYS|WRITE)
f_write(8 KB)
f_sync
f_close
f_unlink(目標路徑)        ← 舊檔還在，到這一步之前斷電都不損失
f_rename(TEMP → 目標)
```

在 rename 之前斷電：舊存檔完好，只賠掉這次的新進度。
這正是 `nvram_save_test` 驗的第二件事。

### 3.3 讀取

`loadNVRAM()` 開檔讀 8 KB。檔案不存在就當作空白（跟目前讀到抹除過的
flash 是 `0xFF` 等效）。FDS 那條路仍然呼叫 `FDS_DeserializeSave()`
驗 magic。

### 3.4 舊存檔怎麼辦

flash 裡可能有一份 slot 0 的舊存檔。三個選項：

| 選項 | 代價 |
|---|---|
| A 不管它 | 使用者的舊進度消失，且不會有提示 |
| B 首次載入時，SD 上沒有檔案就從 flash 讀，之後只寫 SD | 多幾行程式碼，一次性 |
| C 開機時主動搬移 | 要處理搬到一半斷電 |

**建議 B**：讀取時 fallback 到 flash，寫入一律走 SD。自然而然完成遷移，
不需要額外的搬移步驟，也不必處理搬移中斷。

---

## 4. 分階段

### Phase 1：把現有檔案入版控

`nvram_path.h`、`ramdisk_fat12.h`、`nvram_save_test/` 目前是未追蹤狀態。
先確認測試能編能過，再一起提交。

**先決條件**：需要主機端 C 編譯器（MinGW／MSYS2／WSL 皆可）。
`fatfs_utf8_test/` 已經在版控裡，可以拿它確認編譯環境是否正常。

### Phase 2：接上韌體

1. `main.cpp` 改用 `#include "nvram_path.h"`，刪掉重複的 `GAMESAVEDIR`
2. `saveNVRAM()` 改走 §3.2 的序列
3. `loadNVRAM()` 改走 §3.3，並實作 §3.4 的選項 B fallback
4. `getCurrentNVRAMAddr()` 只留給 fallback 讀取用

**注意 `SRAMwritten` 旗標的語意不變**：沒寫過 SRAM 的遊戲仍然要跳過寫入，
不要平白產生空存檔（目前是為了不磨損 flash，現在則是不要在 SD 上留垃圾）。

### Phase 3：實機驗證

必測項目：

- 卡帶 SRAM 遊戲（例如《薩爾達》）存檔 → 拔電 → 開機 → 進度還在
- **換一款遊戲再換回來，兩款的存檔都還在** ← 這是這件事的重點
- FDS 遊戲的 `.FSV` 與 `.SAV` 不互相干擾
- 中文檔名的遊戲存得起來
- 舊 flash 存檔的 fallback 遷移

### Phase 4（可選）：清掉 flash 存檔區

確認遷移沒問題之後，`getCurrentNVRAMAddr()` 與相關的 slot 計算可以整組拿掉。
那會讓 §1.3 的危險徹底消失，`check_flash_layout.cmake` 的檢查也可以放寬。

---

## 5. 待決定

**① 存檔要不要跟著 SD 卡走**

搬到 SD 之後，換一張卡等於換一組存檔。這算特性還是問題？

**② 遊戲名相同的不同 ROM**

不同資料夾下的同名遊戲會共用同一個存檔檔案。要不要在檔名裡加上區別
（例如 ROM 的雜湊）？加了就不能一眼看出存檔屬於哪個遊戲。

**③ SD 卡不在或寫入失敗時**

目前 flash 寫入幾乎不會失敗。改成 SD 之後，卡片沒插、寫保護、空間不足
都是新的失敗模式。要不要退回 flash？還是顯示錯誤就好？

**④ 跟偏移編譯的關係**

`NES_FILE_ADDR` 在偏移模式下是 `0x10084000`。Phase 4 拿掉 flash 存檔之後，
兩種模式的佈局計算會簡單一些。這件事可以獨立於偏移模式進行，
但兩者都碰同一塊 flash 區域，改動時要一起看。

---

## 6. 相關文件

- `HANDOVER.md` — 專案整體交接
- `software/infones/README.md` §「搭配開機載入器」 — flash 佈局與
  `NES_FILE_ADDR` 位移的來龍去脈
- `software/tools/README.md` — 主機端測試工具的慣例
- `fds_plan.md` — FDS 日誌的格式與 8 KB 大小的由來
