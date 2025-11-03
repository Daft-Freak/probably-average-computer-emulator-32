add_executable(PACEPico2
    Main.cpp

    psram.c
)

# some additional target-specific options
option(PICO2_CPU_IN_RAM "Move main CPU interpreter+memory access code to RAM" ON)

if(PICO2_CPU_IN_RAM)
    target_compile_definitions(PACEPico2 PRIVATE PICO_CPU_IN_RAM=1)
endif()


# "extra" board config (the thing the pico is plugged into)
if(NOT EXTRA_BOARD)
    if(PICO_BOARD STREQUAL "solderparty_rp2350_stamp_xl")
        set(EXTRA_BOARD stamp_carrier)
    else()
        set(EXTRA_BOARD none)
    endif()
    message("No EXTRA_BOARD set, assuming ${EXTRA_BOARD}")
endif()

string(TOUPPER ${EXTRA_BOARD} EXTRA_BOARD_UPPER)
target_compile_definitions(PACEPico2 PRIVATE EXTRA_BOARD_${EXTRA_BOARD_UPPER}=1 VGA_RGB565)

# driver selection based on boards
set(AUDIO_DRIVER none)

if(EXTRA_BOARD STREQUAL "stamp_carrier")
    set(DISPLAY_DRIVER "hstx")
endif()

if(EXTRA_BOARD STREQUAL "vgaboard")
    set(AUDIO_DRIVER "i2s")
    set(DISPLAY_DRIVER "dpi")
endif()

# select HSTX anyway, but complain about it
if(NOT DISPLAY_DRIVER)
    set(DISPLAY_DRIVER "hstx")

    message("No DISPLAY_DRIVER set, assuming HSTX")
endif()

target_sources(PACEPico2 PRIVATE
    Audio_${AUDIO_DRIVER}.cpp
    Display_${DISPLAY_DRIVER}.cpp
)

# faster flash access
target_compile_definitions(PACEPico2 PRIVATE PICO_EMBED_XIP_SETUP=1)
# for config.h
target_include_directories(PACEPico2 PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# PIO headers
pico_generate_pio_header(PACEPico2 ${CMAKE_CURRENT_LIST_DIR}/dpi.pio)
pico_generate_pio_header(PACEPico2 ${CMAKE_CURRENT_LIST_DIR}/i2s.pio)

#custom linker script (puts all of CPU.cpp in RAM)
set_target_properties(PACEPico2 PROPERTIES PICO_TARGET_LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/memmap.ld)
pico_add_link_depend(PACEPico2 ${CMAKE_CURRENT_LIST_DIR}/memmap.ld)

# usual setup
target_link_libraries(PACEPico2 PACECore PACEPicoShared PACEPicoBIOS hardware_dma hardware_riscv_platform_timer hardware_spi pico_multicore pico_stdlib)

pico_enable_stdio_uart(PACEPico2 1)
pico_add_extra_outputs(PACEPico2)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/PACEPico2.uf2
	DESTINATION bin
)