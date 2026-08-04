# rp2040-ili9341-infones 技術分析

分析對象：`software/infones/`（RP2040 + ILI9341 掌機上的 InfoNES 移植）
分析日期：2026-08-04

---

## 1. 專案定位

RP2040 搭配 ILI9341 的掌上遊戲機，把 InfoNES（NES 模擬器）移植到超頻的 RP2040 上，以超頻 SPI 直接推送 LCD。

程式碼基礎來自 [pico-infones](https://github.com/shuichitakano/pico-infones) 與
[pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus)，但**顯示層被完全重寫**成
「無 framebuffer、逐掃描線 DMA」的架構，這是本專案與其他移植最大的差異。

---

## 2. 系統架構

### 2.1 時脈與電壓

| 項目 | 設定 | 位置 |
|---|---|---|
| 核心電壓 | `VREG_VOLTAGE_1_20` | `main.cpp:1523` |
| 系統時脈 | 252 MHz | `main.cpp:220`, `main.cpp:1525` |
| `clk_peri` 來源 | 改接為 `clk_sys` | `main.cpp:1334-1340` |
| LCD SPI | 63 MHz (ILI9341) / 33 MHz (ST7789) | `main.cpp:69`, `main.cpp:73` |

`clk_peri` 預設掛在 48 MHz 的 USB PLL 上，SPI 最高只能到 24 MHz 左右。這裡把它改接到
`clk_sys`，才有辦法讓 SPI 跑到 63 MHz。

63 MHz 這個數字不是隨便選的：**252 / 63 = 4**，分頻器整除（prescale 2 × postdiv 2），
不會有時脈誤差或抖動。

### 2.2 雙核分工

```
Core0 ── 6502 CPU 模擬 ── PPU 逐線繪製 ── 逐線 DMA 送屏
   │
   └── InfoNES_SoundOutput ──> [8 KB spinlock ring buffer]
                                        │
Core1 ── core1_main() 迴圈 <────────────┘
              └── audio_play_once() ── audio_mixer_step() ── PWM + DMA 鏈
```

- **Core0**：模擬與顯示（`InfoNES_Main()` 由 `main()` 呼叫）
- **Core1**：`core1_main()`（`main.cpp:832`）純音訊迴圈

### 2.3 顯示路徑（本專案核心設計）

沒有 framebuffer——RP2040 的 264 KB SRAM 放不下 320×240×2 = 150 KB 再加上模擬器狀態。
取而代之的做法：

1. **開機時設定一次視窗**
   `ili9341_infones_frame_timing_register_init()`（`main.cpp:761`）設定
   x = 32..287、y = 4..235，發出 `WRITE_MEMORY_START`，然後
   **把 CS 一直壓住不放**（`main.cpp:778-779`）。

2. **之後每條掃描線只做資料續傳**
   `InfoNES_PostDrawLine()`（`main.cpp:1110`）只呼叫
   `dma_channel_set_read_addr()` 續送 512 bytes（`main.cpp:1200-1203`），
   完全不再發任何 SPI 命令、不再切換 DC/CS。

3. **奇偶線雙緩衝**
   `InfoNES_PreDrawLine()`（`main.cpp:1103-1107`）依 `line % 2` 交替使用
   `scanline_buf_internal_1` / `_2`，讓上一線的 DMA 傳輸與下一線的 PPU 運算重疊。

4. **靠 GRAM 位址自動遞增 + 視窗尾端回捲**來對齊整個畫面。

> **關鍵細節：視窗高度與繪製行數必須精確對上。**
> 這個 fork 把 `PPU_ScanTable` 全部填成 `SCAN_ON_SCREEN`（`InfoNES.cpp:334-340`），
> 配合 `InfoNES_HSync()` 的 `line >= 4 && line < 240-4`（`InfoNES.cpp:748`），
> 每幀剛好送出 **232 行**，等於視窗的 232 列。
>
> 原版 InfoNES 的 ScanTable 只把 8..231 標為 `SCAN_ON_SCREEN`（224 行）。
> 若沿用原版，每幀會少送 8 行，GRAM 寫入指標每幀漂移 8 列，畫面會持續向上捲動。
> 這個改動看似隨意，實際上是這套「不重設位址」架構能成立的前提。

### 2.4 調色盤

```c
#define CC(x) (x & 32767)          // main.cpp:293
const WORD __not_in_flash_func(NesPalette)[64] = { ... };
```

表中已經是**位元組交換過**的 RGB565 常數，可直接以 8-bit DMA 餵給 SPI（小端序送出時
順序正好符合 ILI9341 的期待），省掉每個像素的位元組交換。`& 0x7FFF` 清掉最高位。

調色盤加了 `__not_in_flash_func` 放進 SRAM，避免每個像素查表都走 XIP flash cache。

### 2.5 音訊路徑

```
InfoNES_pAPU (22050 Hz, 5 聲道) 
   └── InfoNES_SoundOutput()          main.cpp:626   混音成 8-bit
         └── AudioRingBuffer (8 KB, spinlock)   main.cpp:134
               └── core1: 每次取 ≤1024 bytes    main.cpp:864
                     └── audio_play_once()      audio.c:138
                           └── audio_mixer_step()  audio.c:174
                                 └── PWM + 三通道 DMA 鏈  audio.c:42
```

`audio.c` 的 DMA 鏈設計（`pwm_dma_chan` → chain → `sample_dma_chan`，
外加 `trigger_dma_chan` 重新觸發）讓 PWM 輸出完全不需要 CPU 介入，
每個取樣重複 `REPETITION_RATE = 4` 次 PWM 週期。

---

## 3. 頻寬與效能的實際數字

每幀 SPI 傳輸量：

```
232 行 × 256 像素 × 2 bytes = 118,784 bytes = 950,272 bits
950,272 bits ÷ 63,000,000 Hz ≈ 15.1 ms
```

60 fps 的週期是 16.67 ms。**光是 SPI 傳輸就吃掉 90%**，剩下不到 1.6 ms 給 6502 模擬、
PPU 繪製與音訊混音——這在 RP2040 上完全不夠。

這正是 README 所說「只顯示三分之一幀才能到可接受速度」的根本原因。

> **但目前程式碼裡的跳幀機制已經失效。**
> `frame_skip_counter`（`main.cpp:923`）只是自增後歸零，所有真正用到它的分支都被
> `#if 0` 包起來（`main.cpp:940-958` 的 `#if 0`，以及 `main.cpp:1140`、`main.cpp:1154-1157` 的註解），
> 而 `InfoNES.cpp:457` 又把 `FrameSkip = 0`。
> 現況是**每一幀都完整送屏**，README 描述的功能在程式碼中不存在。

### 3.1 跳幀機制的演變考證

跳幀不是從來沒有過，而是**在達成足夠吞吐量之後被刻意移除**的。git 歷史如下：

| 日期 | commit | 事件 |
|---|---|---|
| 2023-04-10 | `dd60662` software add folder | **跳幀誕生**：`BYTE frame_skip;` 加上 `InfoNES_PostDrawLine()` 開頭的 `if(frame_skip) return;`，三幀畫一幀 |
| 2023-04-12 | `565a03e` | 音訊搭上同一節拍：`if(frame_skip_counter == 0){ ... }` 每三幀送一次音訊 |
| **2023-04-18** | **`43a143f` speed up to 53 FPS** | **顯示端跳幀被註解掉** |
| 2024-01-12 | `9dd8b3a` | 收尾：`BYTE frame_skip;`、`frame_skip = true/false;` 一併註解，變數正式報廢 |
| 2024-01-15 | `a7b9069` | core1 仍以 `frame_skip_counter` 當音訊節拍（配合 `SoundOutputBuilding` 旗標） |
| 2026-01-03 | `c2f2c2f` | 改用 ring buffer + `TARGET_LATENCY_BYTES` 節流，**最後一個使用者被移除**，只剩空轉的計數器 |

關鍵是 `43a143f`，它把逐線 DMA 從同步改成非同步：

```diff
                 dma_channel_wait_for_finish_blocking(display_dma_channel);
+        memcpy(scanline_buf_outgoing, scanline_buf_internal, sizeof(scanline_buf_outgoing));
                 dma_channel_set_trans_count(display_dma_channel, 256*2, false);
-                dma_channel_set_read_addr(display_dma_channel, (uint8_t *)scanline_buf_internal, true);
-                dma_channel_wait_for_finish_blocking(display_dma_channel);
+                dma_channel_set_read_addr(display_dma_channel, (uint8_t *)scanline_buf_outgoing, true);
+                // dma_channel_wait_for_finish_blocking(display_dma_channel);
```

原本是「設定 DMA → 等它跑完 → 才回去模擬下一線」，DMA 完全沒有省到 CPU 時間。
改成寫入 outgoing 緩衝區後**不等待**，SPI 傳輸才真正與下一條掃描線的 PPU 運算重疊。
同一個 commit 裡 `if(frame_skip) return;` 被註解掉——吞吐量夠了，就不必再靠丟幀換速度。
commit message 的「53 FPS」正是移除跳幀後的實測值。

（目前的奇偶線雙緩衝 `scanline_buf_internal_1/_2` 是更後期的改良，連這裡的 `memcpy`
都省掉了。）

> **結論**：計數器邏輯是歷史殘留，不是壞掉的功能。若要重新啟用跳幀（見第 6 節），
> 應該視為新增功能來設計，而不是「修復」既有程式碼。

---

## 4. 問題清單

### 4.1 音訊取樣率被當成速度節流器 ⚠️ 中（原判定為「不匹配」，經 git 考證確認為刻意設計）

- pAPU 設定 `#define pAPU_QUALITY 2`（`InfoNES_pAPU.h:171`）
  → `ApuQuality = 1`（`InfoNES_pAPU.cpp:1208`）
  → 查 `ApuQual[1]` 得 **22050 Hz**
- 播放端 `audio_init(7, 19654)`（`main.cpp:842`）→ **19654 Hz**

比值 22050 / 19654 ≈ **1.122**。而 `InfoNES_SoundOutput()` 裡有延遲節流：

```c
while (audioRing.readable_size() > TARGET_LATENCY_BYTES)  // main.cpp:639
{
    sleep_us(100);
}
```

於是**整個模擬速度被音訊消耗端反向拖住約 12%**（約 53 fps、音高偏低約兩個半音）。

**git 歷史證實這是刻意的。** `audio_init()` 的取樣率一直被當成速度旋鈕在調校：

| 日期 | commit | 取樣率 | 同期事件 |
|---|---|---|---|
| 2023-04-10 | `dd60662` | 22050 | 初版，與 APU 產生率一致 |
| 2023-04-12 | `565a03e` | 17159 | 音訊改為每三幀送一次 |
| 2023-04-18 | `43a143f` | 19477 | **同一 commit 移除跳幀，畫面變快，取樣率跟著上調** |
| 2023-04-19 | `76c092f` | **19654** | 細調定案，之後兩年多未再變動 |

（ST7789 分支另有 20050 → 20000 的平行調校軌跡，見 `a7b9069`。）

每次顯示效能改變，這個數字就跟著微調，方向完全一致：畫面變快 → 取樣率上調 →
節流放鬆。19654 是 2023-04-19 手工調出來的定值，不是筆誤。

因此**不應**直接把它改成 22050——那會解除節流，讓模擬跑到超過顯示能力的速度。
正確的處理是：

1. 在 `audio_init(7, 19654)` 處加註解，說明這是速度節流器而非單純的取樣率；
2. 若要正音（消除 12% 的音高偏低），必須先建立獨立的幀率節流機制
   （或修好 `speed_control()`，見 4.5），再把取樣率還原成 22050。

在那之前，這個「不匹配」是系統正常運作的一部分。

### 4.2 Ring buffer 寫入端無溢位保護，且會永久阻塞 ⚠️ 高（**待修**，觸發條件已成立，見文末決議）

這是兩個互相關聯的缺陷。

**缺陷一：`write()` 不檢查剩餘空間**（`main.cpp:174-181`）

```cpp
void write(const uint8_t* data, int len) {
    uint32_t saved_irq = spin_lock_blocking(lock);
    for(int i=0; i<len; ++i) {
         buffer[head] = data[i];
         head = (head + 1) % AUDIO_RING_BUFFER_SIZE;   // ← 只推 head，不看 tail
    }
    spin_unlock(lock, saved_irq);
}
```

`head` 可以直接輾過 `tail`。一旦越過，`readable_size()` 的計算會從「快滿」瞬間變成
「幾乎空」，8 KB 待播音訊蒸發、新舊資料播放順序顛倒，聽感上是一次爆音加一段跳針。

防護所需的資訊其實已經具備：`writable_size()`（`main.cpp:155`）算得出剩餘空間，
還透過 `InfoNES_GetSoundBufferSize()`（`main.cpp:616`）導出給 InfoNES 核心——
但 `write()` 自己沒有呼叫它。

**缺陷二：節流迴圈沒有逾時**（`main.cpp:639-642`）

```cpp
while (audioRing.readable_size() > TARGET_LATENCY_BYTES)
{
    sleep_us(100);
}
```

平常這就是 4.1 所述的節流器，運作正常；但它沒有任何脫身條件。
具體的死鎖路徑確實存在：core1 主迴圈（`main.cpp:876-880`）是

```cpp
while(audio_is_source_active(id)) { audio_mixer_step(); }
```

而 `audio_mixer_step()` 只有在 `audio_get_buffer()` 回傳非 NULL 時才推進
`source->pos`（`audio.c:176`），這又取決於 `cur_audio_buffer` 在 DMA 中斷裡翻面
（`audio.c:33-39`）。只要那個 DMA 中斷停了（DMA 鏈卡住、通道被誤搶、IRQ 被關閉），
source 永遠 `active` → core1 出不來 → ring buffer 永遠不被讀取 →
core0 永遠卡在 `sleep_us(100)`。

結果是畫面、聲音、按鍵全部凍結。且遊戲執行期間**沒有啟用看門狗**——
`watchdog_enable()` 全專案只出現一次（`menu.cpp:730`），那是用來觸發重開機的技巧，
不是保護機制。因此只能斷電重開。

**建議修法**（約十行，不改變正常路徑行為）：

```cpp
// 缺陷一：依剩餘空間截斷
int w = writable_size();
if (len > w) len = w;

// 缺陷二：加上最大等待時間
uint64_t deadline = time_us_64() + 50000;   // 50 ms
while (audioRing.readable_size() > TARGET_LATENCY_BYTES && time_us_64() < deadline)
{
    sleep_us(100);
}
```

節流照舊生效，只是在異常情況下退化成掉音訊，而不是整機當掉。

### 4.3 `audio_claim_unused_source()` 的競態 ⚠️ 中

```c
static int audio_claim_unused_source(void)   // audio.c:127
{
  for (int i = 0; i < AUDIO_MAX_SOURCES; i++) {
    if (! mixer_sources[i].active) {
      mixer_sources[i].active = true;    // ← 先標記為使用中
      return i;
    }
  }
  return -1;
}
```

`active = true` 在 `samples` / `len` / `pos` 被填入之前就設定了。
目前 mixer 只在 core1 的主迴圈呼叫，所以實際上安全；但 `dma_handler`（`audio.c:33`）
也跑在 core1，結構上很脆弱——之後若把 `audio_mixer_step()` 移進 IRQ，
就會讀到未初始化的 source 指標。

### 4.4 DMA 完成不等於 SPI 送完 ⚠️ 中

`InfoNES_PostDrawLine()` 只等 `dma_channel_wait_for_finish_blocking()`（`main.cpp:1200`），
沒有等 SPI TX FIFO 排空（`spi_is_busy()`）。

在正常遊戲迴圈中因為 CS 永遠壓低、且只送資料，所以無害。
但 `saveNVRAM()`、`menu()`、`display_write_command()` 這些路徑會在 DMA 可能仍在飛的情況下
切換 DC/CS，存在畫面損毀或指令被當成資料寫入 GRAM 的風險。

### 4.5 `speed_control()` 是純忙等，且與音訊節流互相打架 ⚠️ 中

```c
static void speed_control(void)   // main.cpp:806
{
  static uint64_t last_blink = 0;
  uint64_t cur_time = time_us_64();
  uint64_t diff_time = cur_time - last_blink;   // 計算後未使用
  while (last_blink + (16666) > cur_time) { cur_time = time_us_64(); }
  last_blink = cur_time;
}
```

- 純忙等，不讓出 CPU
- 只設速度上限，不做落後追趕
- 首次呼叫時 `last_blink = 0`，等待條件直接不成立
- `diff_time` 算了但沒用到

它與 4.1 的音訊節流是**兩套互相干擾的節流機制**，實際幀率由兩者中較慢的決定，
且互動難以預測。建議只保留一套。

### 4.6 程式碼衛生 ℹ️ 低

- `main.cpp` 共 1691 行，其中有大量 `#if 0` 區塊：DVI/HDMI 殘留
  （`main.cpp:1617-1634`，另有 EDID/I2C 實驗 `main.cpp:1581-1616`）、逐欄掃描實驗（`main.cpp:978-1028`）、
  hagl 呼叫註解。
- `InfoNES_PadState()` 前 80 行整段是 `#if 0` 的舊 USB 手把邏輯（`main.cpp:377-463`）。
- `snd_drum.h` 有 379 KB（4628 行），但除了存在於目錄中以外沒有任何 `#include` 引用它。
- `InfoNES.cpp:334-340` 的 ScanTable 初始化三個分支填相同的值，
  可以合併並加註解說明「本移植刻意讓全部 240 行都視為可繪製」。

### 4.7 建置設定 ℹ️ 低

```cmake
set(PICO_SDK_PATH "../../../pico-sdk")    # CMakeLists.txt:10
```

硬編相對路徑並覆蓋環境變數，換機器就得改檔案。建議：

```cmake
if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH "../../../pico-sdk")
endif()
```

另外 `LCD_*` 的 pin 定義沒有加 `CACHE STRING`（`CMakeLists.txt:44-51`），
與 `SD_*` 的寫法不一致，無法從命令列覆寫。

---

## 5. 硬體接腳總覽

| 功能 | 腳位 | 來源 |
|---|---|---|
| LCD SPI | spi0，CLK 18 / MOSI 19 / CS 17 / DC 20 / RST 21 / BL 22 | `CMakeLists.txt:44-51` |
| SD 卡 SPI | spi1，SCK 10 / MOSI 11 / MISO 12 / CS 13 | `CMakeLists.txt:36-40` |
| 音訊 PWM | GPIO 7 | `main.cpp:842` |
| 方向鍵 | UP 9 / DOWN 5 / LEFT 8 / RIGHT 6 | `main.cpp:99-102` |
| 按鍵 | A 2 / B 3 / SELECT 28 / START 4 | `main.cpp:103-106` |

注意 LCD 的 DC 用了 GPIO 20，而 SD 卡在 spi1 上，兩條匯流排完全分離，
不會互相搶佔——這對「CS 永遠壓住」的顯示設計是必要條件。

---

## 6. 建議的修復優先順序

1. **修 4.2（無限等待 + 無溢位保護）**
   唯一會造成整機凍結的問題，且改動範圍小。優先度最高。
2. **加上 SPI 同步防護（4.4）**
   在所有 `display_write_command()` 之前先等 DMA 與 SPI FIFO 都空。
3. **統一節流機制（4.5 + 4.1）**
   目前音訊節流（實際生效）與 `speed_control()`（幾乎無效）並存。
   建議把速度控制收斂成單一機制，之後才有可能把取樣率還原成 22050 正音。
4. **決定跳幀的去留（3.1）**
   計數器是歷史殘留而非壞掉的功能——`43a143f` 之後靠非同步 DMA 就達到 53 fps。
   若要進一步提速再考慮重新設計，否則應直接刪除殘留變數。
   同時建議更新 README，目前的描述與程式碼現況不符。
5. **程式碼清理與建置設定（4.6、4.7）**

---

## 7. 值得肯定的設計

- **63 MHz = 252/4 的整除選擇**，避免 SPI 時脈誤差
- **CS 常壓 + 位址自動遞增**，把每條掃描線的開銷壓到只剩一次 DMA 設定
- **奇偶線雙緩衝**，讓 SPI 傳輸與 PPU 運算真正重疊
- **調色盤預先做好位元組交換並放進 SRAM**，省掉每像素的轉換成本
- **ScanTable 改成 232 行以對齊 LCD 視窗**，讓「不重設位址」的策略得以成立
- **音訊 DMA 三通道鏈**，PWM 輸出完全零 CPU 介入

這些都是在 264 KB SRAM、無硬體顯示控制器的條件下，把 NES 模擬硬擠進 RP2040 的合理取捨。

---

## 8. 決議紀錄

### 2026-08-04：4.2 暫不處理

> ⚠️ **本決議的觸發條件已於同日成立，見下一則。**

**決定**：不修改，僅記錄待辦。

**理由**：實機至今未觀察到凍結現象。4.2 描述的是一條**理論上存在但尚未被觸發**的
死鎖路徑——它需要 DMA 中斷先停止運作才會發生，而目前沒有證據顯示那件事發生過。
在沒有實際症狀的情況下改動音訊路徑，反而可能擾動已經手工調校穩定的節流平衡
（見 4.1 的取樣率調校史）。

**觸發條件**：若日後出現以下任一症狀，應立即回頭實作 4.2 的修法：

- 遊玩中畫面與聲音同時凍結、按鍵無反應，且需斷電才能恢復
- 音訊出現規律性的爆音後跳針（缺陷一的溢位特徵）
- 更換 LCD、調整 SPI 時脈或改動 DMA 通道配置之後（這些都可能改變 DMA 中斷的時序）

**備註**：修法本身已在 4.2 節寫好，約十行，屆時可直接套用。

### 2026-08-04（同日稍晚）：觸發條件成立，4.2 改列為待修

**觀察到的症狀**：FDS 模擬（見 [`fds_plan.md`](fds_plan.md) 7.7）在**磁碟讀取期間**
持續出現間歇性的尖銳嗶嗶聲，遊戲進行中沒有。

這符合上一則列出的第二個觸發條件——「音訊出現規律性的爆音後跳針」。

**相關性佐證**：磁碟動作是唯一會讓 mapper 20 每秒送出約 6000 次 IRQ 的時段。

| 狀態 | 磁碟 IRQ／秒（實測） | 嗶聲 |
|---|---|---|
| 磁碟讀取中 | ≈5900 | **有** |
| 遊戲進行中 | 0 | 無 |

**推測的機制**：IRQ 風暴讓 core0 的模擬速率不穩，`InfoNES_SoundOutput()` 供應樣本的
節奏跟著抖動，於是 4.2 的兩個缺陷（`write()` 無溢位保護、節流迴圈無逾時）在負載下
顯現為規律性的破音。

> **尚未驗證是溢位還是欠載，而兩者的修法不同。**
> 4.2 缺陷一的修法（依剩餘空間截斷）針對的是**溢位**——core0 產出快過 core1 消耗。
> 但磁碟讀取時 core0 是**變慢**的，比較像欠載。
> 因此**不要直接套用 4.2 的修法就當作解決**，先量 `audioRing.readable_size()`
> 在磁碟動作期間是週期性歸零（欠載）還是 head 越過 tail（溢位）。

**狀態改為**：4.2 不再是「理論上存在但尚未被觸發」。第一個觸發條件（凍結）
仍未出現，但第二個已經出現。修法仍未實作。

**另一個加重因素不在本文件範圍內**：mapper 20 的磁碟傳輸採需求驅動模型，
實測 298 cycles/byte，真機為 149，載入時間與 IRQ 風暴的持續時間都約為真機的兩倍
（見 `fds_plan.md` 7.5）。縮短它可以減輕症狀，但那是治標。

---

*本文件為程式碼分析紀錄，不含任何實際程式碼改動。*
