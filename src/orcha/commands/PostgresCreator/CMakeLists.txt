cmake_minimum_required(VERSION 3.16)
project(PostgresCreator)

set(CMAKE_CXX_STANDARD 20)
find_package(cpprestsdk REQUIRED)

add_library(PostgresCreator SHARED PostgresCreator.cpp)

target_include_directories(PostgresCreator PRIVATE ${CMAKE_SOURCE_DIR}/core)

target_link_libraries(PostgresCreator PRIVATE cpprestsdk::cpprest)

set_target_properties(PostgresCreator PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/commands
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/commands
)
