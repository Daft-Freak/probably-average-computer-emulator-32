# minimal SDL shell

add_executable(PACE_SDL
    DiskIO.cpp
    Main.cpp
)

find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)

target_link_libraries(PACE_SDL PACECore SDL3::SDL3)

install(TARGETS PACE_SDL)

# install SDL3.dll on windows for convenience
if(WIN32)
    get_target_property(SDL3_LOCATION SDL3::SDL3 IMPORTED_LOCATION_RELEASE)
    if(NOT SDL3_LOCATION)
        get_target_property(SDL3_LOCATION SDL3::SDL3 IMPORTED_LOCATION)
    endif()

    if(SDL3_LOCATION MATCHES ".dll$")
        install(FILES ${SDL3_LOCATION} DESTINATION bin)
    endif()
endif()