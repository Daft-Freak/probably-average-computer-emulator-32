cmake_minimum_required(VERSION 3.13.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PICO_SDK_PATH OR PICO_SDK_FETCH_FROM_GIT)
  include(pico_sdk_import.cmake)
endif()

project(ProbablyAverageComputorEmulator)

if(PICO_SDK_PATH) # set by import file
  set(IS_PICO true)
  pico_sdk_init()

  if(PICO_PLATFORM STREQUAL "rp2350-arm-s")
    set(IS_PICO2 true)
  else()
    message(FATAL_ERROR "This is not building on an RP2040")
  endif()
endif()

if(ESP_TARGET)
  include(esp32/setup.cmake)
  set(IS_ESP32 true)
endif()

if(MSVC)
  add_compile_options("/W4" "/wd4244" "/wd4324" "/wd4458" "/wd4100")
else()
  add_compile_options("-Wall" "-Wextra" "-Wdouble-promotion" "-Wno-unused-parameter")
endif()


include(CMakeDependentOption)
cmake_dependent_option(BUILD_SDL "Build minimal SDL UI" ON "NOT IS_PICO AND NOT IS_ESP32" OFF)
cmake_dependent_option(BUILD_PICO2 "Build Pico 2 UI" ON "IS_PICO2" OFF)
cmake_dependent_option(BUILD_ESP32 "Build ESP32 UI" ON "IS_ESP32" OFF)

add_subdirectory(core)

if(IS_PICO OR BUILD_ESP32)
  add_subdirectory(mcu-shared)
endif()

if(BUILD_SDL)
  add_subdirectory(minsdl)
endif()

if(BUILD_PICO2)
  add_subdirectory(pico2)
endif()

if(BUILD_ESP32)
  add_subdirectory(esp32)
endif()

# setup release packages
set(PROJECT_DISTRIBS LICENSE README.md)
install (FILES ${PROJECT_DISTRIBS} DESTINATION .)
set (CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set (CPACK_GENERATOR "ZIP" "TGZ")
include (CPack)
