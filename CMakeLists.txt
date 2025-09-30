cmake_minimum_required(VERSION 3.31)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508") # @_@
project(pubsub-uring LANGUAGES CXX)

set(CMAKE_CXX_MODULE_STD ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

find_package(liburing REQUIRED)

include(FetchContent)
set(BOOST_INCLUDE_LIBRARIES program_options)
FetchContent_Declare(
  boost
  URL https://github.com/boostorg/boost/releases/download/boost-1.89.0/boost-1.89.0-cmake.tar.gz
)

FetchContent_MakeAvailable(boost)

add_subdirectory(lib)
add_subdirectory(src)