# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

# RP2354B is an RP2350 with 2 MB stacked flash.
set(PICO_PLATFORM rp2350)
set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(aim1k_tablet C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(aim1k
    src/main.c
    src/sensors.c
    src/solve.c
    src/filter.c
    src/usb_descriptors.c
)

target_include_directories(aim1k PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/src        # tusb_config.h
)

target_link_libraries(aim1k
    pico_stdlib
    pico_multicore
    hardware_adc
    hardware_dma
    tinyusb_device
)

target_link_libraries(aim1k m)   # sqrtf/lrintf in solve.c and filter.c

# The debug console is our own CDC interface (see usb_descriptors.c), NOT
# pico_stdio_usb — that library ships its own descriptors and would fight the
# composite device we build here. Leave stdio on UART.
pico_enable_stdio_uart(aim1k 1)
pico_enable_stdio_usb(aim1k 0)

pico_add_extra_outputs(aim1k)     # .uf2, .elf, .bin, map
