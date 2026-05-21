cmake_minimum_required(VERSION 3.16)
project(HttpRequest)

set(CMAKE_CXX_STANDARD 20)
find_package(cpprestsdk REQUIRED)

add_library(HttpRequest SHARED HttpRequest.cpp)
target_include_directories(HttpRequest PRIVATE ${CMAKE_SOURCE_DIR}/core)
target_link_libraries(HttpRequest PRIVATE cpprestsdk::cpprest)
set_target_properties(HttpRequest PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/commands
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/commands
)
