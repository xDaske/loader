cmake_minimum_required(VERSION 3.20)
project(loader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single portable binary: link the CRT statically so the release does not
# depend on vcruntime140.dll being present on the target machine.
if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

add_executable(loader WIN32
    loader.cpp
    cleaner.cpp
)

target_compile_definitions(loader PRIVATE UNICODE _UNICODE NOMINMAX)

target_link_libraries(loader PRIVATE
    user32
    gdi32
    advapi32
    shell32
    ole32
    uuid
    winhttp
)

if (MSVC)
    target_compile_options(loader PRIVATE /W4 /permissive- /utf-8)
    # The overlay needs to open other processes; ask for elevation up front
    # instead of failing halfway through the attach.
    target_link_options(loader PRIVATE /SUBSYSTEM:WINDOWS)
    # The linker does not always embed the manifest; stamp it after linking.
    find_program(MT_EXE mt.exe)
    if (MT_EXE)
        add_custom_command(TARGET loader POST_BUILD
            COMMAND "${MT_EXE}" -nologo -manifest "${CMAKE_CURRENT_SOURCE_DIR}/loader.manifest"
                    "-outputresource:$<TARGET_FILE:loader>;#1"
            VERBATIM)
    endif()
endif()

set_target_properties(loader PROPERTIES
    OUTPUT_NAME "loader"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
)
