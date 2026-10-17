cmake_minimum_required(VERSION 3.16)
project(EchoCommand)

set(CMAKE_CXX_STANDARD 20)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(EchoCommand SHARED EchoCommand.cpp)
target_include_directories(EchoCommand PRIVATE ${CMAKE_SOURCE_DIR}/core)
target_link_libraries(EchoCommand PRIVATE nlohmann_json::nlohmann_json)
