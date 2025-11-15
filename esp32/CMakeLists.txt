
include(bios.cmake)

add_executable(PACE_ESP32 Main.cpp)

target_compile_definitions(PACE_ESP32 PRIVATE ESP_BUILD)

target_link_libraries(PACE_ESP32 PUBLIC
    idf::newlib
    idf::fatfs
    idf::esp_timer
    idf::esp_lcd
    idf::esp_driver_ppa
    idf::esp_wifi
    idf::usb
    idf::usb_host_hid

    PACECore
    PACE_BIOS
)

if(ESP_TARGET STREQUAL "esp32p4")
    target_link_libraries(PACE_ESP32 PUBLIC idf::esp_hosted)
    target_compile_definitions(PACE_ESP32 PRIVATE ESP_HOSTED)
endif()

idf_build_executable(PACE_ESP32)

# install... something
install(FILES ${CMAKE_BINARY_DIR}/PACE_ESP32.bin
    DESTINATION bin
)
