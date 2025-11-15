# embed BIOS
# FIXME: apart from arch settings, this is duplicated with pico
set(BIOS_FILE bios.bin)
set(BIOS_PATH ${CMAKE_CURRENT_LIST_DIR}/../)
set(BIOS_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})

if(NOT EXISTS ${BIOS_PATH}${BIOS_FILE})
    message(FATAL_ERROR "Could not find ${BIOS_FILE}, go build SeaBIOS or copy it from a QEMU install")
endif()

add_custom_command(
    OUTPUT bios.o
    WORKING_DIRECTORY ${BIOS_PATH}
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littleriscv -B riscv --rename-section .data=.rodata,alloc,load,readonly,data,contents ${BIOS_FILE} ${CMAKE_CURRENT_BINARY_DIR}/bios.o
    DEPENDS ${BIOS_PATH}${BIOS_FILE}
)

add_library(PACE_BIOS bios.o)
target_include_directories(PACE_BIOS INTERFACE ${CMAKE_CURRENT_LIST_DIR})
set_target_properties(PACE_BIOS PROPERTIES LINKER_LANGUAGE C)

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
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littleriscv -B riscv --rename-section .data=.rodata,alloc,load,readonly,data,contents ${RENAME_ARG} ${VGA_BIOS_FILE} ${CMAKE_CURRENT_BINARY_DIR}/vga-bios.o
    DEPENDS ${BIOS_PATH}${VGA_BIOS_FILE}
)

target_sources(PACE_BIOS PRIVATE vga-bios.o)
