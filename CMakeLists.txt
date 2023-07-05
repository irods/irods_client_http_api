cmake_minimum_required(VERSION 3.18.0 FATAL_ERROR)

set(IRODS_HTTP_BINARY_NAME "irods_http_api")

set(IRODS_HTTP_API_VERSION_MAJOR 0)
set(IRODS_HTTP_API_VERSION_MINOR 9)
set(IRODS_HTTP_API_VERSION_PATCH 5)
set(IRODS_HTTP_API_VERSION "${IRODS_HTTP_API_VERSION_MAJOR}.${IRODS_HTTP_API_VERSION_MINOR}.${IRODS_HTTP_API_VERSION_PATCH}")

find_package(IRODS REQUIRED CONFIG)

include(RequireOutOfSourceBuild)
include(IrodsCXXCompiler)

set(CMAKE_CXX_STANDARD ${IRODS_CXX_STANDARD})
# export-dynamic so stacktrace entries from executables have function names.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-rdynamic -Wl,--export-dynamic -Wl,--enable-new-dtags -Wl,--as-needed -Wl,-z,defs")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT "-Wl,--gc-sections -Wl,-z,combreloc")

# TODO Rename binary.
project(${IRODS_HTTP_BINARY_NAME} VERSION "${IRODS_VERSION}" LANGUAGES CXX)

find_package(Threads REQUIRED)

set(CMAKE_SKIP_BUILD_RPATH OFF)
set(CMAKE_SKIP_INSTALL_RPATH OFF)
set(CMAKE_SKIP_RPATH OFF)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)

# TODO Remove once project is ready for the public.
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

include(${IRODS_TARGETS_PATH})
include(GNUInstallDirs)
include(UseLibCXX)
    
if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build {Debug, Release}." FORCE)
  message(STATUS "Setting unspecified CMAKE_BUILD_TYPE to '${CMAKE_BUILD_TYPE}'. This is the correct setting for normal builds.")
endif()

find_package(CURL REQUIRED)
find_package(nlohmann_json "3.6.1" REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto SSL)


include(FetchContent)
FetchContent_Declare(
  jwt
  GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git
  GIT_TAG        aca3217a3e0d78979e3c906aed20170971eb8c98 # 0.7.0-rc.0
)

FetchContent_MakeAvailable(jwt)


if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fpermissive>)
endif()

add_compile_options(-Wall -Wextra -Werror -Wpedantic)

add_subdirectory(src)
