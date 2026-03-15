cmake_minimum_required(VERSION 3.18)
project(cryptobot CXX)

# =============================================================================
# CryptoBot C++ — CMake build configuration
# =============================================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Build type defaults
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Release: full optimisation, native CPU, LTO
    set(CMAKE_CXX_FLAGS_RELEASE
        "-O3 -march=native -mtune=native -DNDEBUG -flto")

    # Debug: sanitizers for memory/UB detection
    set(CMAKE_CXX_FLAGS_DEBUG
        "-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer")

    # Common flags
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        # Favour performance
        -ffast-math               # allow reordering FP ops
    )
endif()

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
include_directories(${CMAKE_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(cryptobot ${SOURCES})

# ---------------------------------------------------------------------------
# OpenSSL (required for HTTPS market data access)
# ---------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)
target_link_libraries(cryptobot PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(cryptobot PRIVATE BOT_USE_OPENSSL)
message(STATUS "OpenSSL found — HTTPS support enabled")

# ---------------------------------------------------------------------------
# Threads
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(cryptobot PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# Optional: Link-time optimisation
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_ok)
    if(ipo_ok)
        set_property(TARGET cryptobot PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "LTO enabled")
    endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS cryptobot DESTINATION bin)
install(FILES config/config.ini DESTINATION etc/cryptobot)

# ---------------------------------------------------------------------------
# Unit test target (optional — add test files to tests/)
# ---------------------------------------------------------------------------
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    # Add test targets here
    # add_executable(test_ring_buffer tests/test_ring_buffer.cpp)
    # target_include_directories(test_ring_buffer PRIVATE include)
    # add_test(NAME ring_buffer COMMAND test_ring_buffer)
endif()

# ---------------------------------------------------------------------------
# Print summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "OpenSSL    : ${OpenSSL_FOUND}")
message(STATUS "")
message(STATUS "Build with:")
message(STATUS "  mkdir build && cd build")
message(STATUS "  cmake .. -DCMAKE_BUILD_TYPE=Release")
message(STATUS "  make -j$(nproc)")
message(STATUS "")
message(STATUS "Run (shadow mode):")
message(STATUS "  ./cryptobot ../config/config.ini")
message(STATUS "")
