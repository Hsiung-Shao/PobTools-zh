cmake_minimum_required(VERSION 3.18)
project(lzip CXX)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

find_package(ZLIB REQUIRED)

find_package(LuaJIT REQUIRED)

add_library(lzip SHARED lzip.cpp)
target_include_directories(lzip PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(lzip PRIVATE ZLIB::ZLIB ${LUA_LIBRARY})