# =========================================================================
# CMake Configuration for Factory-HUB
# =========================================================================
cmake_minimum_required(VERSION 3.16)
project(FactoryHub VERSION 1.0)

# -------------------------------------------------------------------------
# Project Settings
# -------------------------------------------------------------------------
# Set a modern C++ standard (required by your code)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -------------------------------------------------------------------------
# CPM Package Manager Setup
# -------------------------------------------------------------------------
# Include the CPM.cmake script from its folder
# This enables the CPMAddPackage function
include(cpm-cmake/CPM.cmake)

# -------------------------------------------------------------------------
# CPM-Managed Dependencies (External)
# -------------------------------------------------------------------------
# These will be automatically downloaded and configured by CMake.

# --- UI & Graphics Dependencies ---

# GLFW (Windowing and Input)
CPMAddPackage(
    NAME glfw
    GITHUB_REPOSITORY glfw/glfw
    VERSION 3.3.8 # Use a stable version
)

# GLEW (OpenGL Extension Wrangler)
CPMAddPackage(
    NAME glew
    GITHUB_REPOSITORY nigels-com/glew
    VERSION 2.2.0 # Use a stable version
)

# --- Core/Protocol Dependencies ---

# nlohmann::json (JSON library, header-only)
CPMAddPackage(
    NAME nlohmann_json
    GITHUB_REPOSITORY nlohmann/json
    VERSION 3.11.3 # Use a stable version
)

# ZeroMQ (libzmq)
CPMAddPackage(
    NAME libzmq
    GITHUB_REPOSITORY zeromq/libzmq
    VERSION 4.3.5 # Use a stable version
    OPTIONS
        "ZMQ_BUILD_TESTS OFF"
        "ZMQ_BUILD_EXAMPLES OFF"
        "ZMQ_BUILD_MANPAGES OFF"
)

# Paho MQTT C Client
CPMAddPackage(
    NAME paho-mqtt-c
    GITHUB_REPOSITORY eclipse/paho.mqtt.c
    VERSION 1.3.13 # Use a stable version
    OPTIONS
        "PAHO_WITH_SSL ON"       # Enable SSL (needed for secure connections)
        "PAHO_BUILD_SAMPLES OFF"
        "PAHO_BUILD_TESTS OFF"
)

# libmodbus
CPMAddPackage(
    NAME libmodbus
    GITHUB_REPOSITORY stephane/libmodbus
    VERSION 3.1.10 # Use a stable version
    OPTIONS
        "BUILD_TESTS OFF"
        "BUILD_EXAMPLES OFF"
)

# open62541 (OPC-UA)
CPMAddPackage(
    NAME open62541
    GITHUB_REPOSITORY open62541/open62541
    VERSION 1.3.8 # Use a stable version
    OPTIONS
        "UA_ENABLE_AMALGAMATION ON" # Easiest way to build/link
        "UA_BUILD_EXAMPLES OFF"
        "UA_BUILD_TESTS OFF"
        "UA_NAMESPACE_ZERO FULL" # Recommended
)

# -------------------------------------------------------------------------
# Local Dependencies (imgui)
# -------------------------------------------------------------------------
# As requested, we add ImGui from the local 'imgui' folder.

# Configure ImGui's build: We don't want its examples, just the library.
set(IMGUI_BUILD_EXAMPLES OFF)
set(IMGUI_BUILD_TESTS OFF)
set(IMGUI_BUILD_GLFW_EXAMPLE OFF)
set(IMGUI_BUILD_OPENGL3_EXAMPLE OFF)

# Add the imgui directory. This will find its own CMakeLists.txt
# and create the necessary targets (imgui, imgui_impl_glfw, imgui_impl_opengl3)
add_subdirectory(imgui)

# -------------------------------------------------------------------------
# Main Application Executable
# -------------------------------------------------------------------------
# Define the executable target and list all its source files.
add_executable(FactoryHub
    main.cpp
    GatewayHub.cpp
    GatewayHub.h
    GatewayUI.cpp
    GatewayUI.h
)

# -------------------------------------------------------------------------
# Include Directories
# -------------------------------------------------------------------------
# Tell the compiler where to find headers.
target_include_directories(FactoryHub PRIVATE
    # This allows #include "GatewayHub.h" to work from main.cpp
    ${PROJECT_SOURCE_DIR}
    
    # Add local imgui headers
    imgui # For imgui.h
    imgui/backends # For imgui_impl_glfw.h, etc.
)

# -------------------------------------------------------------------------
# Library Linking (The most important part!)
# -------------------------------------------------------------------------

# Link all the libraries (both CPM and local) to our application
target_link_libraries(FactoryHub PRIVATE
    # --- CPM Dependencies ---
    # These are the "target names" created by CPM
    glfw          # From CPMAddPackage(NAME glfw)
    glew          # From CPMAddPackage(NAME glew)
    nlohmann_json::nlohmann_json # Header-only target
    libzmq        # From CPMAddPackage(NAME libzmq)
    paho-mqtt-c   # From CPMAddPackage(NAME paho-mqtt-c)
    libmodbus     # From CPMAddPackage(NAME libmodbus)
    open62541     # From CPMAddPackage(NAME open62541)

    # --- Local ImGui Dependencies ---
    # These targets are created by add_subdirectory(imgui)
    imgui
    imgui_impl_glfw
    imgui_impl_opengl3
)

# -------------------------------------------------------------------------
# Platform-Specific Adjustments
# -------------------------------------------------------------------------

if(WIN32)
    # --- Windows ---
    # Define GLEW_STATIC (required on Windows when linking GLEW)
    target_compile_definitions(FactoryHub PRIVATE GLEW_STATIC)
    
    # Link necessary Windows system libraries for networking and security
    target_link_libraries(FactoryHub PRIVATE
        ws2_32   # Windows Sockets (for ZMQ, Paho, libmodbus)
        wsock32  # Windows Sockets
        iphlpapi # For libmodbus
        Crypt32  # For Paho (Windows SChannel SSL)
    )
else()
    # --- Linux ---
    # Find the standard OpenGL and Threads libraries
    find_package(OpenGL REQUIRED)
    find_package(Threads REQUIRED)

    # Link them
    target_link_libraries(FactoryHub PRIVATE
        ${OpenGL_LIBRARIES}
        Threads::Threads # For pthreads (used by ZMQ, Paho, etc.)
        m                # Math library (often needed)
    )
endif()

# --- End of CMakeLists.txt ---
