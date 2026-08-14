cmake_minimum_required(VERSION 3.20)
project(a2m-sender LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(a2m-sender
    src/main.cpp
    src/capture.cpp
    src/cursor.cpp
    src/encoder.cpp
    src/sender.cpp
    src/protocol.cpp
    src/stats.cpp
    src/util.cpp
)

target_include_directories(a2m-sender PRIVATE src)

target_compile_definitions(a2m-sender PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    # /utf-8: sources are UTF-8 and quote Japanese section titles from docs/
    target_compile_options(a2m-sender PRIVATE /W3 /permissive- /EHsc /utf-8)
    # Release: optimise for speed, no incremental linking noise
    target_compile_options(a2m-sender PRIVATE $<$<CONFIG:Release>:/O2>)
endif()

# Windows SDK / Media Foundation only - no third-party dependencies.
# (d3dcompiler is a Windows SDK / OS component, used for the cursor blend shader.)
target_link_libraries(a2m-sender PRIVATE
    d3d11
    d3dcompiler
    dxgi
    dxguid
    mf
    mfplat
    mfuuid
    mfreadwrite
    strmiids
    ws2_32
    ole32
    oleaut32
)
