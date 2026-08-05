# 開發工具 / Development tools

主機端跑的工具，不是韌體的一部分。

Host-side tools; none of this is part of the firmware.

---

## `make_cjk_font.py` — 中文字型打包器

把 PicoType 的字型資料重新打包成 `software/infones/font_cjk.h`。

```bash
python3 software/tools/make_cjk_font.py /path/to/picotype_data_optimized.h
```

來源資料一個像素用一個 byte 存（1.24 MB），像素值只有 `0x00` 與 `0xFF` 兩種，
所以改成一個像素一個 bit 是無損的，體積降到 212 KB。字形本身完全沒有改動。

Cubic 11 沒有收錄的字，來源資料裡是 2×2 點的 notdef 佔位符，打包時會剔除
（判定用的是精確圖樣簽章，不是著墨像素數 —— `'` 和 `;` 剛好也是 4 個像素，
用數量判斷會誤殺）。

只需要 Python 3，沒有第三方套件。細節見腳本本身的說明與
[`infones/README.md`](../infones/README.md#選單的中文顯示--chinese-text-in-the-menu)。

---

## `fatfs_utf8_test/` — FatFs UTF-8 回歸測試

```bash
cd software/tools/fatfs_utf8_test
make check
```

把韌體**自己那份** FatFs 原始碼、配**未經修改的** `ffconf.h` 編在主機上，
跑在 RAM disk 上。什麼都不複製，所以不會跟韌體走鐘。

涵蓋中文顯示所依賴的兩條 UTF-8 路徑：

1. **寫入 `currentloadedrom.txt`** —— 必須用 `f_write`。在 `FF_LFN_UNICODE 2`
   之下，`f_putc` / `f_puts` 那一家是編碼轉換器：`putc_bfd()` 會把多位元組序列的
   byte 暫存在 `putbuff` 裡等序列收完，而 `f_putc` 每次呼叫都重建一個 `putbuff`，
   所以逐 byte 寫 UTF-8 會把每個非 ASCII 字元丟光。全中文檔名的 ROM 因此寫出空檔，
   開機直接跳回選單。舊寫法保留在測試裡，好讓失效模式看得見。

2. **讀取中文檔名** —— 檢查的是**磁碟上**目錄項裡的 UTF-16 碼位，不是 FatFs
   自己來回一趟。後者證明不了任何事：`FF_LFN_UNICODE 0` 之下每個 UTF-8 byte
   都能對應到某個 CP437 字元再對應回來，名字看起來活著，但卡片上存的是亂碼。

失敗時回傳非零，可以直接接進 CI。把 `FF_LFN_UNICODE` 改回 0 會看到它抓出來：

```
  超級瑪利歐.nes      on-disk LFN  FAIL
    expected UTF-16: 8d85 7d1a 746a 5229 6b50 002e 006e 0065 0073
    on disk        : 03a6 2562 00e0 03c4 2524 00dc 03c4 00e6 ...
```

只需要主機端的 C 編譯器與 `make`。
