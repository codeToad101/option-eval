cmake_minimum_required(VERSION 3.18)
project(bs_pricer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimise for speed; keep debug info for profiling
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined")

add_executable(bs_pricer main.cpp)
target_include_directories(bs_pricer PRIVATE ${CMAKE_SOURCE_DIR})

# Link math library on Linux
if(UNIX)
    target_link_libraries(bs_pricer m)
endif()