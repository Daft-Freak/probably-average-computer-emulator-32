add_library(PACEPicoShared INTERFACE)

target_sources(PACEPicoShared INTERFACE
    fatfs/ff.c
    fatfs/ffunicode.c

    DiskIO.cpp
    Filesystem.cpp
    KeyMap.cpp
    Storage.cpp
    USBHID.cpp
)

target_include_directories(PACEPicoShared INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(PACEPicoShared INTERFACE PACECore hardware_pio tinyusb_host)

pico_generate_pio_header(PACEPicoShared ${CMAKE_CURRENT_LIST_DIR}/spi.pio)

#embed BIOS
generate_bios(PACEPicoBIOS elf32-littlearm armv6s-m)