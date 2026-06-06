cmake_minimum_required(VERSION 3.18)
project(bs_pricer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined")

# ── nlohmann/json (header-only) ───────────────────────────────────────────────
# brew install nlohmann-json
find_package(nlohmann_json REQUIRED)

# ── Main executable ───────────────────────────────────────────────────────────
# No TWS sources. No protobuf. No absl. No libbid.
add_executable(bs_pricer
    src/main.cpp
)

target_include_directories(bs_pricer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(bs_pricer PRIVATE
    nlohmann_json::nlohmann_json
)