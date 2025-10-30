add_library(PACEPicoShared INTERFACE)

target_sources(PACEPicoShared INTERFACE
    fatfs/ff.c
    fatfs/ffunicode.c

    #DiskIO.cpp
    Filesystem.cpp
    Storage.cpp
    USBHID.cpp
)

target_include_directories(PACEPicoShared INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(PACEPicoShared INTERFACE PACECore hardware_pio tinyusb_host)

pico_generate_pio_header(PACEPicoShared ${CMAKE_CURRENT_LIST_DIR}/spi.pio)

#embed BIOS
set(BIOS_FILE bios.bin)
set(BIOS_PATH ${CMAKE_CURRENT_LIST_DIR}/../)
set(BIOS_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})

if(NOT EXISTS ${BIOS_PATH}${BIOS_FILE})
    message(FATAL_ERROR "Could not find ${BIOS_FILE}, go build SeaBIOS or copy it from a QEMU install")
endif()

add_custom_command(
    OUTPUT bios.o
    WORKING_DIRECTORY ${BIOS_PATH}
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B armv6s-m --rename-section .data=.rodata,alloc,load,readonly,data,contents ${BIOS_FILE} ${CMAKE_CURRENT_BINARY_DIR}/bios.o
    DEPENDS ${BIOS_PATH}${BIOS_FILE}
)

add_library(PACEPicoBIOS bios.o)
target_include_directories(PACEPicoBIOS INTERFACE ${CMAKE_CURRENT_LIST_DIR})
set_target_properties(PACEPicoBIOS PROPERTIES LINKER_LANGUAGE C)

# embed VGA BIOS
set(VGA_BIOS_FILE vgabios.bin)
set(RENAME_ARG)

if(NOT EXISTS ${BIOS_PATH}${VGA_BIOS_FILE})
    message("Could not find ${VGA_BIOS_FILE}, trying vgabios-isavga.bin")
    set(VGA_BIOS_FILE vgabios-isavga.bin)
    set(RENAME_ARG --redefine-sym "_binary_vgabios_isavga_bin_start=_binary_vgabios_bin_start")
endif()

if(NOT EXISTS ${BIOS_PATH}${VGA_BIOS_FILE})
    message(FATAL_ERROR "Could not find ${VGA_BIOS_FILE}, go build SeaBIOS or copy it from a QEMU install")
endif()

add_custom_command(
    OUTPUT vga-bios.o
    WORKING_DIRECTORY ${BIOS_PATH}
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B armv6s-m --rename-section .data=.rodata,alloc,load,readonly,data,contents ${RENAME_ARG} ${VGA_BIOS_FILE} ${CMAKE_CURRENT_BINARY_DIR}/vga-bios.o
    DEPENDS ${BIOS_PATH}${VGA_BIOS_FILE}
)

target_sources(PACEPicoBIOS PRIVATE vga-bios.o)
