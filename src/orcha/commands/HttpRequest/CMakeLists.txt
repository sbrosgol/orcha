cmake_minimum_required(VERSION 3.16)
project(HttpRequest)

set(CMAKE_CXX_STANDARD 20)
find_package(Boost REQUIRED COMPONENTS system)
find_package(boost_url CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(HttpRequest SHARED HttpRequest.cpp)
target_include_directories(HttpRequest PRIVATE ${CMAKE_SOURCE_DIR}/core)
target_link_libraries(HttpRequest PRIVATE
        Boost::system Boost::url OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
