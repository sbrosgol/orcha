cmake_minimum_required(VERSION 3.22)

# Suppress FindBoost deprecation warning (CMake 3.30+)
# This must be set before project() for vcpkg compatibility
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(orcha VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg integration (if present)
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

find_package(Boost REQUIRED COMPONENTS thread system)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(boost_url CONFIG REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

include_directories(${CMAKE_SOURCE_DIR})

# Generate version header from template
configure_file(
    ${CMAKE_SOURCE_DIR}/core/Version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/core/Version.hpp
    @ONLY
)
include_directories(${CMAKE_BINARY_DIR}/generated)

# Core libraries
add_library(core STATIC
        core/ICommand.hpp
        core/CommandRegistry.cpp
        core/CommandRegistry.hpp
        core/PluginManager.cpp
        core/PluginManager.hpp
        core/PluginDenylist.hpp
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

target_link_libraries(core PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(config PUBLIC yaml-cpp::yaml-cpp nlohmann_json::nlohmann_json)

# Header-only utils library (modern CMake!)
add_library(utils INTERFACE)
target_include_directories(utils INTERFACE ${CMAKE_SOURCE_DIR}/utils)

add_library(workflow STATIC
        workflow/WorkflowEngine.cpp
        workflow/WorkflowEngine.hpp
        workflow/IWorkflowEngine.hpp
)
target_link_libraries(workflow PUBLIC core utils yaml-cpp::yaml-cpp logger)

# Jobs persistence (SQLite-backed job/run store + execution service)
add_library(jobs STATIC
        jobs/IJobStore.hpp
        jobs/SqliteJobStore.cpp
        jobs/SqliteJobStore.hpp
        jobs/JobService.hpp
        jobs/RunJobCommand.hpp
        jobs/CronExpr.hpp
        jobs/JobScheduler.cpp
        jobs/JobScheduler.hpp
)
target_link_libraries(jobs PUBLIC core workflow logger
        unofficial::sqlite3::sqlite3)

add_library(agent STATIC
        agent/CommandAgent.cpp
        agent/CommandAgent.hpp
        agent/HttpServer.cpp
        agent/HttpServer.hpp
        agent/Http.hpp
        agent/HttpJson.hpp
        agent/IRouteHandler.hpp
        agent/AuthMiddleware.hpp
        agent/dashboard_embedded.hpp
        agent/routes/HealthRoute.hpp
        agent/routes/WorkflowRoute.hpp
        agent/routes/SwaggerRoute.hpp
        agent/routes/PluginAdminRoute.hpp
        agent/routes/JobRoute.hpp
        agent/routes/DashboardRoute.hpp
)



# Boost.Beast (header-only) powers the HTTP server; needs Boost::system + threads.
target_link_libraries(agent PUBLIC core workflow jobs Boost::thread Boost::system logger)

# Main executable
add_executable(orcha
        main.cpp
)
target_link_libraries(orcha PRIVATE agent config)

target_precompile_headers(core PRIVATE ${CMAKE_SOURCE_DIR}/orcha_pch.hpp)
target_precompile_headers(agent PRIVATE ${CMAKE_SOURCE_DIR}/orcha_pch.hpp)
target_precompile_headers(workflow PRIVATE ${CMAKE_SOURCE_DIR}/orcha_pch.hpp)
target_precompile_headers(config PRIVATE ${CMAKE_SOURCE_DIR}/orcha_pch.hpp)
target_precompile_headers(jobs PRIVATE ${CMAKE_SOURCE_DIR}/orcha_pch.hpp)
target_precompile_headers(orcha PRIVATE ${CMAKE_SOURCE_DIR}/orcha_pch.hpp)
# `logger` is a small, dependency-light library; skip PCH there (it's only
# two TUs, so the missed savings are negligible).

# --- Automatically add all command plugins from /commands ---
# Each plugin builds into ${CMAKE_BINARY_DIR}/commands/<Name>/ so its
# manifest.json sits beside the shared library (the plugin discovery
# matches them by directory).
file(GLOB COMMAND_DIRS RELATIVE "${CMAKE_SOURCE_DIR}/commands" "${CMAKE_SOURCE_DIR}/commands/*")

foreach(subdir ${COMMAND_DIRS})
    if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/commands/${subdir}")
        if(EXISTS "${CMAKE_SOURCE_DIR}/commands/${subdir}/CMakeLists.txt")
            add_subdirectory(commands/${subdir})
            message(STATUS "Added command plugin: ${subdir}")
            if(TARGET ${subdir})
                set_target_properties(${subdir} PROPERTIES
                    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/commands/${subdir}"
                    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/commands/${subdir}")
                # Reuse the project PCH: every plugin pulls in nlohmann/json via
                # core/ICommand.hpp, which is the single biggest parse cost.
                target_precompile_headers(${subdir} PRIVATE
                    ${CMAKE_SOURCE_DIR}/orcha_pch.hpp)
            endif()
            if(EXISTS "${CMAKE_SOURCE_DIR}/commands/${subdir}/manifest.json")
                configure_file(
                    "${CMAKE_SOURCE_DIR}/commands/${subdir}/manifest.json"
                    "${CMAKE_BINARY_DIR}/commands/${subdir}/manifest.json"
                    COPYONLY)
            endif()
        endif()
    endif()
endforeach()

message(STATUS "Registered command plugins from commands/ directory.")

if(NOT WIN32)
    target_link_libraries(core PUBLIC dl)
endif()

# Test executable for dependency resolver
# core exposes nlohmann_json transitively (PUBLIC), so it is not listed again.
add_executable(test_dependency_resolver
        tests/test_dependency_resolver_main.cpp
)
target_link_libraries(test_dependency_resolver PRIVATE core)

# Test executable for new architecture features.
# workflow brings core (+nlohmann_json, yaml-cpp, logger, utils) and config
# brings nlohmann_json/yaml-cpp transitively, so only the direct deps are listed
# here to avoid duplicate-library link warnings.
add_executable(test_new_features
        tests/test_new_features_main.cpp
)
# jobs brings workflow/core/nlohmann_json/sqlite transitively; config brings yaml-cpp.
target_link_libraries(test_new_features PRIVATE jobs config)