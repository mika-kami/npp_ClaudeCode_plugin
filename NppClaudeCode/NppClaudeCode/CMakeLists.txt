cmake_minimum_required(VERSION 3.16)
project(ClaudeCode LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(ClaudeCode SHARED
    src/Plugin.cpp
    src/ClaudeRunner.cpp
    src/ClaudeCode.rc
)

target_compile_definitions(ClaudeCode PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS)

if(MSVC)
    # Static CRT: the DLL has no Visual C++ Redistributable dependency
    set_property(TARGET ClaudeCode PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(ClaudeCode PRIVATE /W4 /utf-8 /EHsc)
else()
    target_link_options(ClaudeCode PRIVATE -static -static-libgcc -static-libstdc++)
    set_target_properties(ClaudeCode PROPERTIES PREFIX "")
endif()

target_link_libraries(ClaudeCode PRIVATE user32 kernel32)
