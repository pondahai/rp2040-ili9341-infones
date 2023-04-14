# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)
set(PICO_SDK_PATH "../../../pico-sdk")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(infoNES C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)

add_executable(infoNES InfoNES.cpp K6502.cpp InfoNES_Mapper.cpp InfoNES_pAPU.cpp tar.cpp audio.c menu.cpp RomLister.cpp FrensHelpers.cpp main.cpp )

pico_set_program_name(infoNES "infoNES")
pico_set_program_version(infoNES "0.1")

pico_enable_stdio_uart(infoNES 1)
pico_enable_stdio_usb(infoNES 1)

# Add the standard library to the build
target_link_libraries(infoNES pico_stdlib)

set(SD_CS "13" CACHE STRING "Specify the Chip Select GPIO pin for the SD card")
set(SD_SCK "10" CACHE STRING "Specify de Clock GPIO pin for the SD card")
set(SD_MOSI "11" CACHE STRING "Select the Master Out Slave In GPIO pin for the SD card")
set(SD_MISO "12" CACHE STRING "Select the Master In Slave Out GPIO pin for the SD card")

#set(LCD_CONTROLLER "ILI9341" CACHE STRING "Select the LCD controller type")
set(LCD_CONTROLLER "ST7789" CACHE STRING "Select the LCD controller type")


message("SD card CS          : ${SD_CS}")
message("SD card SCK         : ${SD_SCK}")
message("SD card MOSI        : ${SD_MOSI}")
message("SD card MISO        : ${SD_MISO}")
message("LCD CONTROLLER      : ${LCD_CONTROLLER}")
target_compile_definitions(infoNES PRIVATE
    SDCARD_PIN_SPI0_CS=${SD_CS}
    SDCARD_PIN_SPI0_SCK=${SD_SCK}
    SDCARD_PIN_SPI0_MOSI=${SD_MOSI}
    SDCARD_PIN_SPI0_MISO=${SD_MISO}
)
if(LCD_CONTROLLER STREQUAL "ILI9341")
    add_compile_definitions(ILI9341)
elseif(LCD_CONTROLLER STREQUAL "ST7789")
    add_compile_definitions(ST7789)
endif()

# Add any user requested libraries
target_link_libraries(infoNES
        pico_stdlib
	pico_multicore
        hardware_spi
        hardware_dma
        hardware_pio
	hardware_i2c
	hardware_interp
    	hardware_timer
    	hardware_clocks
    	hardware_pwm
    	hardware_flash
    sdcard
    fatfs        
        )

pico_add_extra_outputs(infoNES)

