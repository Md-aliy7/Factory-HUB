cmake_minimum_required(VERSION 3.20)
project(Factory-HUB VERSION 1.0.0 LANGUAGES CXX)

# Set minimum policy version for dependencies (allows older CMakeLists.txt files)
# This must be set before any dependencies are configured
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum CMake policy version" FORCE)
endif()

# Set policies to handle dependency issues
if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

# Include CPM
# Download CPM.cmake if not already available
set(CPM_DOWNLOAD_VERSION 0.38.7)
if(CPM_SOURCE_CACHE)
  set(CPM_DOWNLOAD_LOCATION "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
elseif(DEFINED ENV{CPM_SOURCE_CACHE})
  set(CPM_DOWNLOAD_LOCATION "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
else()
  set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
endif()

if(NOT (EXISTS ${CPM_DOWNLOAD_LOCATION}))
  message(STATUS "Downloading CPM.cmake to ${CPM_DOWNLOAD_LOCATION}")
  file(DOWNLOAD
    https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake
    ${CPM_DOWNLOAD_LOCATION}
  )
endif()

include(${CPM_DOWNLOAD_LOCATION})

# Find OpenGL
find_package(OpenGL REQUIRED)

# Find Threads
find_package(Threads REQUIRED)

# Find OpenSSL (required for uWebSockets and MQTT)
find_package(OpenSSL REQUIRED)

# CPM Dependencies

# nlohmann/json
CPMAddPackage(
    NAME nlohmann_json
    VERSION 3.11.3
    GITHUB_REPOSITORY nlohmann/json
    OPTIONS
        "JSON_BuildTests OFF"
        "JSON_Install OFF"
)

# ZeroMQ (libzmq) - using a version that works with modern CMake
# Disable linting to avoid target name conflicts
set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
CPMAddPackage(
    NAME libzmq
    VERSION 4.3.5
    GITHUB_REPOSITORY zeromq/libzmq
    OPTIONS
        "BUILD_TESTS OFF"
        "BUILD_SHARED OFF"
        "ENABLE_DRAFTS OFF"
        "ENABLE_CURVE OFF"
        "ENABLE_DOC OFF"
        "WITH_LIBSODIUM OFF"
        "CMAKE_POLICY_DEFAULT_CMP0144 NEW"
)

# cppzmq
CPMAddPackage(
    NAME cppzmq
    VERSION 4.10.0
    GITHUB_REPOSITORY zeromq/cppzmq
    OPTIONS
        "CPPZMQ_BUILD_TESTS OFF"
)

# Boost (Asio and System) - using header-only version via CPM
CPMAddPackage(
    NAME Boost
    VERSION 1.83.0
    GITHUB_REPOSITORY boostorg/boost
    GIT_TAG boost-1.83.0
    OPTIONS
        "BUILD_TESTING OFF"
)

# GLFW (needed before ImGui)
CPMAddPackage(
    NAME glfw
    VERSION 3.3.9
    GITHUB_REPOSITORY glfw/glfw
    GIT_TAG 3.3.9
    OPTIONS
        "GLFW_BUILD_DOCS OFF"
        "GLFW_BUILD_TESTS OFF"
        "GLFW_BUILD_EXAMPLES OFF"
        "GLFW_INSTALL OFF"
)

# Make sure GLFW is linked correctly
if(TARGET glfw)
    # GLFW target already exists
elseif(TARGET glfw_static)
    add_library(glfw ALIAS glfw_static)
endif()

# Dear ImGui (using local copy)
add_library(imgui STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_demo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends
)

target_link_libraries(imgui PUBLIC
    glfw
    OpenGL::GL
    Threads::Threads
)

# GLEW - try to find system package first, fallback to CPM
find_package(GLEW QUIET)
if(NOT GLEW_FOUND)
    CPMAddPackage(
        NAME glew
        VERSION 2.2.0
        GITHUB_REPOSITORY nigels-com/glew
        GIT_TAG glew-2.2.0
        OPTIONS
            "GLEW_BUILD_SHARED OFF"
            "GLEW_BUILD_UTILS OFF"
    )
endif()

# libmodbus - try to find system package first, fallback to CPM
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBMODBUS QUIET libmodbus)
endif()
if(NOT LIBMODBUS_FOUND)
    CPMAddPackage(
        NAME libmodbus
        VERSION 3.1.10
        GITHUB_REPOSITORY stephane/libmodbus
        OPTIONS
            "BUILD_TESTS OFF"
            "BUILD_SHARED_LIBS OFF"
    )
endif()

# open62541 - using FetchContent directly to patch before configure
FetchContent_Declare(
    open62541
    GIT_REPOSITORY https://github.com/open62541/open62541.git
    GIT_TAG v1.3.7
)
FetchContent_GetProperties(open62541)
if(NOT open62541_POPULATED)
    # Set options before populating
    set(UA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(UA_ENABLE_AMALGAMATION OFF CACHE BOOL "" FORCE)
    set(UA_ENABLE_SUBSCRIPTIONS ON CACHE BOOL "" FORCE)
    set(UA_ENABLE_METHODCALLS ON CACHE BOOL "" FORCE)
    set(UA_ENABLE_NODEMANAGEMENT ON CACHE BOOL "" FORCE)
    set(UA_ENABLE_SUBSCRIPTIONS_EVENTS ON CACHE BOOL "" FORCE)
    FetchContent_Populate(open62541)
    # Disable linting to avoid target name conflicts - replace with empty file
    if(EXISTS "${open62541_SOURCE_DIR}/tools/cmake/linting_target.cmake")
        file(WRITE "${open62541_SOURCE_DIR}/tools/cmake/linting_target.cmake" "# Linting disabled to avoid target name conflicts\n")
    endif()
    add_subdirectory(${open62541_SOURCE_DIR} ${open62541_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# Paho MQTT C
set(PAHO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Set C standard to avoid C23 bool keyword conflict
set(CMAKE_C_STANDARD 11 CACHE STRING "C standard" FORCE)
CPMAddPackage(
    NAME paho-mqtt-c
    VERSION 1.3.13
    GITHUB_REPOSITORY eclipse/paho.mqtt.c
    OPTIONS
        "PAHO_BUILD_DOCUMENTATION OFF"
        "PAHO_BUILD_SAMPLES OFF"
        "PAHO_BUILD_TESTS OFF"
        "PAHO_BUILD_STATIC ON"
        "PAHO_BUILD_SHARED OFF"
        "PAHO_WITH_SSL ON"
)
# Restore C++ standard after paho-mqtt-c
set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard" FORCE)

# zlib (required for uWebSockets compression)
CPMAddPackage(
    NAME ZLIB
    VERSION 1.3
    GITHUB_REPOSITORY madler/zlib
    OPTIONS
        "SKIP_INSTALL_ALL ON"
)

# uWebSockets (fetch first to get uSockets submodule)
CPMAddPackage(
    NAME uwebsockets
    VERSION 20.62.0
    GITHUB_REPOSITORY uNetworking/uWebSockets
    OPTIONS
        "UWS_USE_LIBUV OFF"
        "UWS_USE_ASAN OFF"
)

# uSockets (required by uWebSockets)
# uWebSockets includes uSockets as a submodule, but we need to build it separately
# uSockets doesn't have CMake, so we'll build it from the uWebSockets submodule using make
set(USOCKETS_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/uwebsockets-src/uSockets")

# Check if uSockets submodule exists and build it
if(EXISTS "${USOCKETS_SOURCE_DIR}/Makefile")
    # Build uSockets using make
    # uSockets needs to be built with WITH_LIBUV=0 and WITH_ASAN=0
    add_custom_target(build_usockets
        COMMAND ${CMAKE_COMMAND} -E chdir ${USOCKETS_SOURCE_DIR} make WITH_LIBUV=0 WITH_ASAN=0
        WORKING_DIRECTORY ${USOCKETS_SOURCE_DIR}
        COMMENT "Building uSockets library"
        BYPRODUCTS "${USOCKETS_SOURCE_DIR}/uSockets.a"
    )
    
    # Find the built library (it should be in the source directory after make)
    # uSockets builds uSockets.a (not libusockets.a)
    set(USOCKETS_LIB "${USOCKETS_SOURCE_DIR}/uSockets.a")
    
    # Create an imported static library target
    add_library(usockets STATIC IMPORTED GLOBAL)
    add_dependencies(usockets build_usockets)
    set_target_properties(usockets PROPERTIES
        IMPORTED_LOCATION ${USOCKETS_LIB}
        INTERFACE_INCLUDE_DIRECTORIES "${USOCKETS_SOURCE_DIR}/src"
    )
endif()

# Executable
add_executable(${PROJECT_NAME}
    main.cpp
    GatewayHub.cpp
    GatewayUI.cpp
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends
)

# Add paho-mqtt-c include directory (MQTTClient.h uses quotes, not angle brackets)
if(PAHO_MQTT_C_INCLUDE_DIR)
    target_include_directories(${PROJECT_NAME} PRIVATE "${PAHO_MQTT_C_INCLUDE_DIR}")
elseif(EXISTS "${CMAKE_BINARY_DIR}/_deps/paho-mqtt-c-src/src")
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_BINARY_DIR}/_deps/paho-mqtt-c-src/src")
endif()

# Add open62541 include directories (non-amalgamated build)
# config.h is in src_generated/open62541/, so we need src_generated in the path
if(EXISTS "${CMAKE_BINARY_DIR}/_deps/open62541-build/src_generated")
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_BINARY_DIR}/_deps/open62541-build/src_generated")
endif()
# Architecture headers (posix/ua_architecture.h) are in arch directory
if(EXISTS "${CMAKE_BINARY_DIR}/_deps/open62541-src/arch")
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_BINARY_DIR}/_deps/open62541-src/arch")
endif()
# Plugins directory contains client_config_default.h and other plugin headers
if(EXISTS "${CMAKE_BINARY_DIR}/_deps/open62541-src/plugins/include")
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_BINARY_DIR}/_deps/open62541-src/plugins/include")
endif()
# Source directory contains the actual headers (open62541/client.h, etc.)
if(EXISTS "${CMAKE_BINARY_DIR}/_deps/open62541-src/include")
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_BINARY_DIR}/_deps/open62541-src/include")
endif()

# Add uWebSockets include directory
# Headers are in src/, but code includes <uwebsockets/App.h>
# So we need to point to the parent directory and create uwebsockets -> src symlink
if(EXISTS "${CMAKE_BINARY_DIR}/_deps/uwebsockets-src/src")
    # Create a symlink so <uwebsockets/App.h> works
    if(NOT EXISTS "${CMAKE_BINARY_DIR}/_deps/uwebsockets-src/uwebsockets")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink "src" "${CMAKE_BINARY_DIR}/_deps/uwebsockets-src/uwebsockets"
        )
    endif()
    target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_BINARY_DIR}/_deps/uwebsockets-src")
endif()

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    imgui
    glfw
    OpenGL::GL
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    Boost::asio
    Boost::system
    open62541
    nlohmann_json::nlohmann_json
)

# Link ZeroMQ - need both cppzmq and libzmq
# Check for targets in order of preference
if(TARGET libzmq-static)
    target_link_libraries(${PROJECT_NAME} PRIVATE libzmq-static)
elseif(TARGET libzmq)
    target_link_libraries(${PROJECT_NAME} PRIVATE libzmq)
endif()
if(TARGET cppzmq-static)
    target_link_libraries(${PROJECT_NAME} PRIVATE cppzmq-static)
elseif(TARGET cppzmq)
    target_link_libraries(${PROJECT_NAME} PRIVATE cppzmq)
endif()

# Link paho-mqtt-c - it creates multiple targets
if(TARGET paho-mqtt3c-static)
    target_link_libraries(${PROJECT_NAME} PRIVATE paho-mqtt3c-static)
elseif(TARGET paho-mqtt-c-static)
    target_link_libraries(${PROJECT_NAME} PRIVATE paho-mqtt-c-static)
elseif(TARGET paho-mqtt-c)
    target_link_libraries(${PROJECT_NAME} PRIVATE paho-mqtt-c)
endif()

# Link zlib (required for compression in uWebSockets)
if(TARGET zlibstatic)
    target_link_libraries(${PROJECT_NAME} PRIVATE zlibstatic)
elseif(TARGET zlib)
    target_link_libraries(${PROJECT_NAME} PRIVATE zlib)
elseif(TARGET ZLIB::ZLIB)
    target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
endif()

# Link uSockets (must come before uWebSockets)
if(TARGET usockets)
    target_link_libraries(${PROJECT_NAME} PRIVATE usockets)
    # Ensure usockets is built before linking
    add_dependencies(${PROJECT_NAME} usockets)
elseif(TARGET uSockets)
    target_link_libraries(${PROJECT_NAME} PRIVATE uSockets)
endif()

# Link uWebSockets
if(TARGET uwebsockets)
    target_link_libraries(${PROJECT_NAME} PRIVATE uwebsockets)
elseif(TARGET uws)
    target_link_libraries(${PROJECT_NAME} PRIVATE uws)
endif()

# libmodbus linking - check which target name is available
if(TARGET libmodbus::libmodbus)
    target_link_libraries(${PROJECT_NAME} PRIVATE libmodbus::libmodbus)
elseif(TARGET libmodbus)
    target_link_libraries(${PROJECT_NAME} PRIVATE libmodbus)
elseif(TARGET modbus)
    target_link_libraries(${PROJECT_NAME} PRIVATE modbus)
elseif(LIBMODBUS_FOUND)
    # Use system libmodbus
    target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBMODBUS_LIBRARIES})
    target_include_directories(${PROJECT_NAME} PRIVATE ${LIBMODBUS_INCLUDE_DIRS})
else()
    # Fallback: try to link directly
    target_link_libraries(${PROJECT_NAME} PRIVATE modbus)
endif()

# GLEW linking - check which target name is available
if(TARGET GLEW::GLEW)
    target_link_libraries(${PROJECT_NAME} PRIVATE GLEW::GLEW)
elseif(TARGET GLEW)
    target_link_libraries(${PROJECT_NAME} PRIVATE GLEW)
elseif(TARGET glew)
    target_link_libraries(${PROJECT_NAME} PRIVATE glew)
elseif(TARGET glew_s)
    target_link_libraries(${PROJECT_NAME} PRIVATE glew_s)
elseif(GLEW_FOUND)
    # Use system GLEW
    target_link_libraries(${PROJECT_NAME} PRIVATE ${GLEW_LIBRARIES})
    target_include_directories(${PROJECT_NAME} PRIVATE ${GLEW_INCLUDE_DIRS})
else()
    # Fallback: try to link directly
    target_link_libraries(${PROJECT_NAME} PRIVATE GLEW)
endif()

# Fix potential linking issues
if(TARGET glfw_static)
    target_link_libraries(${PROJECT_NAME} PRIVATE glfw_static)
endif()

# Platform-specific settings
if(UNIX AND NOT APPLE)
    # Linux specific
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dl
        pthread
    )
elseif(APPLE)
    # macOS specific
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${COCOA_FRAMEWORK}
        ${IOKIT_FRAMEWORK}
        ${COREVIDEO_FRAMEWORK}
    )
elseif(WIN32)
    # Windows specific
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ws2_32
        winmm
    )
endif()

# Compiler-specific options
if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4
        /permissive-
    )
else()
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

# Set output directory
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Installation
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

