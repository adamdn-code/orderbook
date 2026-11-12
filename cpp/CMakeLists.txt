cmake_minimum_required(VERSION 3.16)
project(orderbook CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_library(orderbook src/orderbook.cpp src/engine.cpp)
target_include_directories(orderbook PUBLIC include)
target_link_libraries(orderbook PUBLIC Threads::Threads)
target_compile_options(orderbook PRIVATE -Wall -Wextra)

add_library(reference_book tests/reference_book.cpp)
target_include_directories(reference_book PUBLIC tests)
target_link_libraries(reference_book PUBLIC orderbook)

add_executable(tests tests/tests.cpp)
target_link_libraries(tests orderbook reference_book)

add_executable(bench bench/bench.cpp)
target_link_libraries(bench orderbook reference_book)

enable_testing()
add_test(NAME tests COMMAND tests)
