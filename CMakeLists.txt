cmake_minimum_required(VERSION 3.15)
project(OrderBookEngine VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler Optimizations for M2
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -mcpu=apple-m2")

set(CMAKE_PREFIX_PATH "/opt/homebrew")
find_package(TBB REQUIRED)

include_directories(include)

# 1. The Main Engine & Network Gateway
add_executable(orderbook src/main.cpp src/OrderBook.cpp src/TCPServer.cpp)
find_package(Threads REQUIRED)
target_link_libraries(orderbook PRIVATE TBB::tbb Threads::Threads)

# 2. The TCP Network Stress Test Client
add_executable(stress_test src/stress_test.cpp)