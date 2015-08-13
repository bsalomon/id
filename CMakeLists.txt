cmake_minimum_required(VERSION 3.3)
project(idiff)

set(CMAKE_BUILD_TYPE Release)

set(SRCS idiff.c)
add_executable(idiff ${SRCS})

set_source_files_properties(idiff.c PROPERTIES COMPILE_FLAGS -msse4.2)

find_package(PNG REQUIRED)
include_directories(${PNG_INCLUDE_DIR})
target_link_libraries(idiff ${PNG_LIBRARY})
