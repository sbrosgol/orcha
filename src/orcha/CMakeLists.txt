cmake_minimum_required(VERSION 3.22)
project(orcha LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg integration (if present)
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

find_package(Boost REQUIRED COMPONENTS thread system)
find_package(OpenSSL REQUIRED)
find_package(cpprestsdk REQUIRED)
find_package(yaml-cpp REQUIRED)

include_directories(${CMAKE_SOURCE_DIR})

# Core libraries
add_library(core STATIC
        core/ICommand.hpp
        core/CommandRegistry.cpp
        core/CommandRegistry.hpp
        core/PluginManager.cpp
        core/PluginManager.hpp
)

add_library(logger STATIC
        utils/Logger.cpp
        utils/Logger.hpp
)

add_library(config STATIC
        config/YamlConfiguration.cpp
        config/YamlConfiguration.hpp
        config/IConfiguration.hpp
)

target_link_libraries(core PUBLIC cpprestsdk::cpprest)
target_link_libraries(config PUBLIC yaml-cpp::yaml-cpp cpprestsdk::cpprest)

# Header-only utils library (modern CMake!)
add_library(utils INTERFACE)
target_include_directories(utils INTERFACE ${CMAKE_SOURCE_DIR}/utils)

add_library(workflow STATIC
        workflow/WorkflowRunner.cpp
        workflow/WorkflowRunner.hpp
        workflow/WorkflowEngine.cpp
        workflow/WorkflowEngine.hpp
        workflow/IWorkflowEngine.hpp
)
target_link_libraries(workflow PUBLIC core utils yaml-cpp::yaml-cpp logger)

add_library(agent STATIC
        agent/CommandAgent.cpp
        agent/CommandAgent.hpp
)



target_link_libraries(agent PUBLIC core workflow Boost::thread Boost::system cpprestsdk::cpprest logger)

# Main executable
add_executable(orcha
        main.cpp
)
target_link_libraries(orcha PRIVATE agent config)

target_precompile_headers(core PRIVATE ${CMAKE_SOURCE_DIR}/orcha_pch.hpp)
target_precompile_headers(agent PRIVATE ${CMAKE_SOURCE_DIR}/orcha_pch.hpp)
target_precompile_headers(workflow PRIVATE ${CMAKE_SOURCE_DIR}/orcha_pch.hpp)
target_precompile_headers(orcha PRIVATE ${CMAKE_SOURCE_DIR}/orcha_pch.hpp)

# --- Automatically add all command plugins from /commands ---
file(GLOB COMMAND_DIRS RELATIVE "${CMAKE_SOURCE_DIR}/commands" "${CMAKE_SOURCE_DIR}/commands/*")

foreach(subdir ${COMMAND_DIRS})
    if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/commands/${subdir}")
        if(EXISTS "${CMAKE_SOURCE_DIR}/commands/${subdir}/CMakeLists.txt")
            add_subdirectory(commands/${subdir})
            message(STATUS "Added command plugin: ${subdir}")
        endif()
    endif()
endforeach()

message(STATUS "Registered command plugins from commands/ directory.")

if(NOT WIN32)
    target_link_libraries(core PUBLIC dl)
endif()

# Test executable for dependency resolver
add_executable(test_dependency_resolver
        tests/test_dependency_resolver_main.cpp
)
target_link_libraries(test_dependency_resolver PRIVATE core cpprestsdk::cpprest)