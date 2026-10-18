cmake_minimum_required(VERSION 3.20)
project(EdgeTouch VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(EdgeTouch WIN32
    src/EdgeTouch.cpp
    src/EdgeTouch.rc
)

target_include_directories(EdgeTouch PRIVATE src)
target_compile_definitions(EdgeTouch PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(EdgeTouch PRIVATE user32 shell32 advapi32 gdi32 comctl32 comdlg32)

if(MSVC)
    target_compile_options(EdgeTouch PRIVATE /W4 /permissive- /utf-8)
    # The manifest is embedded from EdgeTouch.rc; don't let the linker add a second one.
    target_link_options(EdgeTouch PRIVATE /MANIFEST:NO)
    # Static CRT, so the exe runs without the Visual C++ redistributable.
    set_property(TARGET EdgeTouch PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
