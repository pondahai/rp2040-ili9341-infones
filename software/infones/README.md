## 切換不同的LCD  
遮蔽CMakeList.txt裡面的這兩行可以決定使用ILI9341或是ST7789。  
By blocking these two lines in CMakeList.txt, you can decide to use ILI9341 or ST7789.  

切換不同的LCD後，在編譯之前需要清空前一次編譯的全部中間碼（清空build資料夾裡面的全部檔案）  
After switching different LCDs, you need to empty all the middle code of the previous compilation (empty all the files in the build folder) before compilation.  
```
#set(LCD_CONTROLLER "ILI9341" CACHE STRING "Select the LCD controller type")
set(LCD_CONTROLLER "ST7789" CACHE STRING "Select the LCD controller type")
for ST7789

or

set(LCD_CONTROLLER "ILI9341" CACHE STRING "Select the LCD controller type")
#set(LCD_CONTROLLER "ST7789" CACHE STRING "Select the LCD controller type")
for ILI9341
```

## 編譯  
1.建立資料夾build  
2.cd到資料夾build。
3.執行 cmake.. 指令  
4.執行 make 最後會產生 infoNES.uf2  
5.拖拉到raspi pico的虛擬磁碟  
6.使用 picotool 上傳遊戲rom檔(以test.nes為例子)  
```
picotool load ../../../agnes_local/nes/test.nes -t bin -o 0x10080000
```

## Credits
Inspired by https://github.com/shuichitakano/pico-infones and https://github.com/fhoedemakers/pico-infonesPlus with thanks to "infones" https://github.com/jay-kumogata/InfoNES

