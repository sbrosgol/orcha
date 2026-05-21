cmake_minimum_required(VERSION 3.16)
project(ShellExec)

set(CMAKE_CXX_STANDARD 20)
find_package(cpprestsdk REQUIRED)

add_library(ShellExec SHARED ShellExec.cpp)
target_include_directories(ShellExec PRIVATE ${CMAKE_SOURCE_DIR}/core)
target_link_libraries(ShellExec PRIVATE cpprestsdk::cpprest)
set_target_properties(ShellExec PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/commands
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/commands
)
