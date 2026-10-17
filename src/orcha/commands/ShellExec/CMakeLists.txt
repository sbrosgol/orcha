cmake_minimum_required(VERSION 3.16)
project(ShellExec)

set(CMAKE_CXX_STANDARD 20)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(ShellExec SHARED ShellExec.cpp)
target_include_directories(ShellExec PRIVATE ${CMAKE_SOURCE_DIR}/core)
target_link_libraries(ShellExec PRIVATE nlohmann_json::nlohmann_json)
