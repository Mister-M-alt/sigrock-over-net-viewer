cmake_minimum_required(VERSION 3.16)
project(sonview LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_SERVER "Build sond (Pi server)"          ON)
option(BUILD_CLIENT "Build sonview (desktop client)"  OFF)

# Shared, header-only wire protocol contract + vendored nlohmann/json.
add_library(son_proto INTERFACE)
target_include_directories(son_proto INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/proto
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)

if(BUILD_SERVER)
  add_subdirectory(server)
endif()
if(BUILD_CLIENT)
  add_subdirectory(client)
endif()
