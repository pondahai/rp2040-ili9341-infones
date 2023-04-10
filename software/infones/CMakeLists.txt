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

add_executable(infoNES InfoNES.cpp K6502.cpp InfoNES_Mapper.cpp InfoNES_pAPU.cpp tar.cpp audio.c main.cpp)

pico_set_program_name(infoNES "infoNES")
pico_set_program_version(infoNES "0.1")

pico_enable_stdio_uart(infoNES 1)
pico_enable_stdio_usb(infoNES 1)

# Add the standard library to the build
target_link_libraries(infoNES pico_stdlib)


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
        )

pico_add_extra_outputs(infoNES)

