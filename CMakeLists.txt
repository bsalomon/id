# Boilerplate.
cmake_minimum_required(VERSION 3.0)
project(idiff)

# libpng is a required dependency.
find_package(PNG REQUIRED)
include_directories(${PNG_INCLUDE_DIR})

# openmp is an optional dependency, but we prefer GCD when on Mac.
if (APPLE)
else()
    find_package(OpenMP)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
endif()

# Default to Release mode.
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Turn on as many warnings as we can stomach in Debug mode.
set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Werror")
if (CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Weverything -Wno-vla -Wno-cast-align")
else()
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Wall -Wextra")
endif()

# Turn on as many sanitizers as we can stomach in Debug mode.
if (CMAKE_C_COMPILER_ID MATCHES "AppleClang")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address")
elseif (CMAKE_C_COMPILER_ID MATCHES "GNU" OR CMAKE_COMPILER_ID MATCHES "Clang")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address,undefined")
endif()

# idiff.c uses _mm_crc32_u32, so we need SSE4.2+.
set_source_files_properties(idiff.c PROPERTIES COMPILE_FLAGS -msse4.2)

# Build it!
add_executable(idiff idiff.c)
target_link_libraries(idiff ${PNG_LIBRARY})
