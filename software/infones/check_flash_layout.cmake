# check_flash_layout.cmake - 擋下 image 尾巴壓到 ROM/存檔區的情況
#
# infoNES 在 flash 裡有三塊各自為政的區域:
#
#   BASE .. binary 結束      程式本體
#   NES_FILE_ADDR - 8KB      NVRAM 存檔槽 slot0 (往下長)
#   NES_FILE_ADDR ..         menu 燒進來的 ROM
#
# 這三者的相對位置從來沒有被檢查過,只是「剛好」不重疊。偏移編譯模式
# (LOADER_OFFSET_BUILD)把本體往後推 16KB 之後就撞上了,症狀是開機把自己的
# .data 初值讀成 ROM、選遊戲時 erase 掉那份初值 —— 畫面全黑,而且很難查。
#
# 所以在 build 期算一次。image 變大或位址改動時,這裡會先報錯。

if(NOT EXISTS "${IMAGE}")
    message(FATAL_ERROR "check_flash_layout: 找不到 ${IMAGE}")
endif()

file(SIZE "${IMAGE}" image_size)

math(EXPR image_end   "${BASE} + ${image_size}"      OUTPUT_FORMAT HEXADECIMAL)
math(EXPR nvram_start "${NES_FILE_ADDR} - ${SRAM_SIZE}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR headroom    "${NES_FILE_ADDR} - ${SRAM_SIZE} - ${BASE} - ${image_size}")

if(headroom LESS 0)
    math(EXPR over "0 - ${headroom}")
    message(FATAL_ERROR
        "\n"
        "  flash 佈局重疊: image 尾巴壓進 NVRAM/ROM 區 ${over} bytes。\n"
        "\n"
        "    image      ${BASE} .. ${image_end}\n"
        "    NVRAM slot0 ${nvram_start}\n"
        "    ROM         ${NES_FILE_ADDR}\n"
        "\n"
        "  這樣開機會把自己的 .data 初值讀成 ROM,選遊戲時還會 erase 掉它。\n"
        "  請把 NES_FILE_ADDR 往上移(偏移模式在 CMakeLists.txt 的\n"
        "  NES_FILE_ADDR_OVERRIDE),或縮小 image。\n"
    )
endif()

message(STATUS
    "flash 佈局 OK: image ${BASE}..${image_end}, NVRAM slot0 ${nvram_start}, 餘裕 ${headroom} bytes")
