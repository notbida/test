cmake_minimum_required(VERSION 3.15)
project(OffsetDumper CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Zydis dependency (header-only mode or fetched)
include(FetchContent)

FetchContent_Declare(
    zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG v4.1.0
)

set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(zydis)

# Source files
add_executable(offset_dumper
    src/main.cpp
    src/memory.cpp
    src/pe_parser.cpp
    src/sig_scanner.cpp
    src/instruction_decoder.cpp
    src/offset_dumper.cpp
)

target_include_directories(offset_dumper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(offset_dumper PRIVATE
    Zydis
)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(offset_dumper PRIVATE PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(offset_dumper PRIVATE PLATFORM_LINUX)
endif()
