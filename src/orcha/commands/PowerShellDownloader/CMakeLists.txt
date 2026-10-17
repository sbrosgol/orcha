cmake_minimum_required(VERSION 3.22)
project(PowerShellDownloader)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg integration (if present)
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

find_package(Boost REQUIRED COMPONENTS system)
find_package(boost_url CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(PowerShellDownloader SHARED PowerShellDownloader.cpp)

target_include_directories(PowerShellDownloader PRIVATE ${CMAKE_SOURCE_DIR}/core ${CMAKE_SOURCE_DIR}/utils)

target_link_libraries(PowerShellDownloader PRIVATE
        Boost::system Boost::url OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
