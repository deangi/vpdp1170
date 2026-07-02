cmake_minimum_required(VERSION 3.20)
project(kek_vs_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(KEK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../_upstream_kek")

add_executable(kek_vs_core
    kek_vs_core.cpp
    compat/kek_vs_console.cpp
    compat/kek_vs_log.cpp
    compat/kek_vs_utils.cpp

    "${KEK_ROOT}/breakpoint.cpp"
    "${KEK_ROOT}/breakpoint_and.cpp"
    "${KEK_ROOT}/breakpoint_instruction.cpp"
    "${KEK_ROOT}/breakpoint_memory.cpp"
    "${KEK_ROOT}/breakpoint_or.cpp"
    "${KEK_ROOT}/breakpoint_parser.cpp"
    "${KEK_ROOT}/breakpoint_register.cpp"
    "${KEK_ROOT}/bus.cpp"
    "${KEK_ROOT}/cpu.cpp"
    "${KEK_ROOT}/dc11.cpp"
    "${KEK_ROOT}/ddp.cpp"
    "${KEK_ROOT}/deqna.cpp"
    "${KEK_ROOT}/device.cpp"
    "${KEK_ROOT}/disk_backend.cpp"
    "${KEK_ROOT}/disk_backend_file.cpp"
    "${KEK_ROOT}/dz11.cpp"
    "${KEK_ROOT}/error.cpp"
    "${KEK_ROOT}/kw11-l.cpp"
    "${KEK_ROOT}/loaders.cpp"
    "${KEK_ROOT}/memory.cpp"
    "${KEK_ROOT}/mmu.cpp"
    "${KEK_ROOT}/my_lock.cpp"
    "${KEK_ROOT}/rk05.cpp"
    "${KEK_ROOT}/rl02.cpp"
    "${KEK_ROOT}/rp06.cpp"
    "${KEK_ROOT}/tm-11.cpp"
    "${KEK_ROOT}/tty.cpp"
)

target_include_directories(kek_vs_core PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/compat"
    "${KEK_ROOT}"
)

target_compile_definitions(kek_vs_core PRIVATE
    IS_POSIX=0
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(kek_vs_core PRIVATE /W3 /permissive- /FI"${CMAKE_CURRENT_LIST_DIR}/compat/kek_vs_compat.h")
else()
    target_compile_options(kek_vs_core PRIVATE -Wall -Wextra)
endif()

target_link_libraries(kek_vs_core PRIVATE ws2_32)
