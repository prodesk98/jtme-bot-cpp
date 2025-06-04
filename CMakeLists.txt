cmake_minimum_required(VERSION 3.31)
project(DLL_Example2)

set(CMAKE_CXX_STANDARD 20)

add_library(DLL_Example2 SHARED
    library.cpp

    MinHook/src/buffer.c
    MinHook/src/hook.c
    MinHook/src/trampoline.c
    MinHook/src/hde64.c
)

target_include_directories(DLL_Example2 PRIVATE MinHook/include)