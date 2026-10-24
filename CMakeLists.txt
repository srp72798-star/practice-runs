cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(PracticeRuns VERSION 1.0.0)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Servidor HTTP local para el AI Level Builder (solo cabecera)
CPMAddPackage(
    NAME httplib
    GITHUB_REPOSITORY yhirose/cpp-httplib
    GIT_TAG v0.18.7
    DOWNLOAD_ONLY YES
)
target_include_directories(${PROJECT_NAME} PRIVATE ${httplib_SOURCE_DIR})
# Server.cpp no usa Geode: sin la cabecera precompilada de Geode (trae
# windows.h, que choca con winsock2.h de cpp-httplib)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/ai/Server.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
if (WIN32)
    target_link_libraries(${PROJECT_NAME} ws2_32)
endif()

setup_geode_mod(${PROJECT_NAME})
