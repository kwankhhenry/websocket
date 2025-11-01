cmake_minimum_required(VERSION 3.20)
project(websocket_hft VERSION 1.0.0 LANGUAGES C)

# M4-specific compiler flags for Apple Silicon optimization
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Detect Apple Silicon (ARM64)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=apple-m4 -mtune=apple-m4")
    message(STATUS "Detected Apple Silicon - enabling M4 optimizations")
endif()

# Optimize for HFT (low latency, no dynamic allocations in hot path)
set(CMAKE_C_FLAGS_RELEASE "-O3 -ffast-math -fno-pie")
set(CMAKE_C_FLAGS_DEBUG "-O0 -g -fsanitize=address")

# Additional M4-specific flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter")

# Enable Neon SIMD
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a+simd")

# Group hot paths into separate section for L2 cache optimization
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-dead_strip")

# Source files
set(SOURCES
    src/ringbuffer.c
    src/io_kqueue.c
    src/ssl.c
    src/parser_neon.c
    src/websocket.c
)

set(HEADERS
    src/ringbuffer.h
    src/io_kqueue.h
    src/ssl.h
    src/parser_neon.h
    src/websocket.h
    src/os_macos.h
    src/internal.h
)

# Create static library
add_library(websocket_hft STATIC ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(websocket_hft PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link SecureTransport framework (primary TLS implementation)
find_library(SECURITY_FRAMEWORK Security)
find_library(SECURITYFOUNDATION_FRAMEWORK SecurityFoundation)
if(SECURITY_FRAMEWORK)
    target_link_libraries(websocket_hft PRIVATE ${SECURITY_FRAMEWORK})
endif()
if(SECURITYFOUNDATION_FRAMEWORK)
    target_link_libraries(websocket_hft PRIVATE ${SECURITYFOUNDATION_FRAMEWORK})
endif()

# Link system libraries
target_link_libraries(websocket_hft PRIVATE
    pthread
)

# Optional: Link OpenSSL 3.x if available (for fallback)
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    target_link_libraries(websocket_hft PRIVATE ${OPENSSL_LIBRARIES})
    target_include_directories(websocket_hft PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_compile_definitions(websocket_hft PRIVATE HAVE_OPENSSL)
    message(STATUS "OpenSSL found - fallback TLS support enabled")
else()
    # Try to find OpenSSL via pkg-config or common paths
    find_path(OPENSSL_INCLUDE_DIR openssl/ssl.h
        PATHS /usr/local/include /opt/homebrew/include
    )
    find_library(OPENSSL_SSL_LIB ssl
        PATHS /usr/local/lib /opt/homebrew/lib /usr/lib
    )
    find_library(OPENSSL_CRYPTO_LIB crypto
        PATHS /usr/local/lib /opt/homebrew/lib /usr/lib
    )
    if(OPENSSL_INCLUDE_DIR AND OPENSSL_SSL_LIB AND OPENSSL_CRYPTO_LIB)
        target_link_libraries(websocket_hft PRIVATE ${OPENSSL_SSL_LIB} ${OPENSSL_CRYPTO_LIB})
        target_include_directories(websocket_hft PRIVATE ${OPENSSL_INCLUDE_DIR})
        target_compile_definitions(websocket_hft PRIVATE HAVE_OPENSSL)
        message(STATUS "OpenSSL found via manual search - fallback TLS support enabled")
    else()
        message(STATUS "OpenSSL not found - using SecureTransport only (OpenSSL functions used in websocket.c require linking)")
        # Note: websocket.c uses OpenSSL for Base64 encoding, so we need at least that
        # For now, we'll rely on SecureTransport for TLS
    endif()
endif()

# OpenSSL is needed for Base64 encoding in websocket.c even if not using OpenSSL for TLS
# Try to find it in system paths
if(NOT OPENSSL_CRYPTO_LIB)
    find_library(CRYPTO_LIB crypto
        PATHS /usr/local/lib /opt/homebrew/lib /usr/lib
    )
    if(CRYPTO_LIB)
        target_link_libraries(websocket_hft PRIVATE ${CRYPTO_LIB})
        message(STATUS "Found crypto library for Base64 support")
    endif()
endif()

# Test executables
add_executable(test_ringbuffer tests/test_ringbuffer.c)
target_link_libraries(test_ringbuffer websocket_hft)

add_executable(test_parser_neon tests/test_parser_neon.c)
target_link_libraries(test_parser_neon websocket_hft)

add_executable(test_kqueue tests/test_kqueue.c)
target_link_libraries(test_kqueue websocket_hft)

add_executable(test_integration_binance tests/test_integration_binance.c)
target_link_libraries(test_integration_binance websocket_hft)

add_executable(benchmark tests/benchmark.c)
target_link_libraries(benchmark websocket_hft)
target_link_libraries(benchmark m)  # Link math library for sqrt()

# Example executable
add_executable(binance_ticker examples/binance_ticker.c)
target_link_libraries(binance_ticker websocket_hft)

# Installation
install(TARGETS websocket_hft
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(FILES ${HEADERS}
    DESTINATION include/websocket_hft
)

# CTest support (if tests are added)
enable_testing()
add_test(NAME test_ringbuffer COMMAND test_ringbuffer)
add_test(NAME test_parser_neon COMMAND test_parser_neon)
add_test(NAME test_kqueue COMMAND test_kqueue)

