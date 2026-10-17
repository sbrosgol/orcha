cmake_minimum_required(VERSION 3.16)
project(PostgresCreator)

set(CMAKE_CXX_STANDARD 20)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)

add_library(PostgresCreator SHARED PostgresCreator.cpp)

target_include_directories(PostgresCreator PRIVATE ${CMAKE_SOURCE_DIR}/core)

target_link_libraries(PostgresCreator PRIVATE nlohmann_json::nlohmann_json libpqxx::pqxx)
