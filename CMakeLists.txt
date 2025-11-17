# =========================================================================
# CMake Configuration for Factory-HUB (vcpkg Version)
# =========================================================================
cmake_minimum_required(VERSION 3.19) # vcpkg works best with 3.19+
project(FactoryHub VERSION 1.0)

# -------------------------------------------------------------------------
# Project Settings
# -------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -------------------------------------------------------------------------
# External Dependencies (via vcpkg)
# -------------------------------------------------------------------------
# These must be installed via vcpkg *before* running cmake.
#
# Run this command in your vcpkg directory:
# ./vcpkg install glfw3 glew nlohmann-json zeromq paho-mqtt-c libmodbus open62541
#
# -------------------------------------------------------------------------

find_package(glfw3 CONFIG REQUIRED)
find_package(GLEW CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZeroMQ CONFIG REQUIRED)
find_package(PahoMqttC CONFIG REQUIRED) # Package name for paho-mqtt-c
find_package(Modbus CONFIG REQUIRED)    # Package name for libmodbus
find_package(open62541 CONFIG REQUIRED)

# -------------------------------------------------------------------------
# Local Dependencies (imgui)
# -------------------------------------------------------------------------
# As requested, we add ImGui from the local 'imgui' folder.
set(IMGUI_BUILD_EXAMPLES OFF)
set(IMGUI_BUILD_TESTS OFF)
set(IMGUI_BUILD_GLFW_EXAMPLE OFF)
set(IMGUI_BUILD_OPENGL3_EXAMPLE OFF)
add_subdirectory(imgui)

# -------------------------------------------------------------------------
# Main Application Executable
# -------------------------------------------------------------------------
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
target_include_directories(FactoryHub PRIVATE
    # This allows #include "GatewayHub.h" to work from main.cpp
    ${PROJECT_SOURCE_DIR}
    
    # Add local imgui headers
    imgui # For imgui.h
    imgui/backends # For imgui_impl_glfw.h, etc.
)

# -------------------------------------------------------------------------
# Library Linking
# -------------------------------------------------------------------------
# Link all the libraries (both vcpkg and local) to our application
target_link_libraries(FactoryHub PRIVATE
    # --- vcpkg Dependencies ---
    # These are the modern CMake target names provided by vcpkg
    glfw3::glfw
    GLEW::glew
    nlohmann_json::nlohmann_json
    ZeroMQ::zmq
    PahoMqttC::paho-mqtt-c
    Modbus::modbus
    open62541::open62541

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
    # Define GLEW_STATIC if linking the static version from vcpkg
    # vcpkg's default is static, so this is usually needed.
    target_compile_definitions(FactoryHub PRIVATE GLEW_STATIC)
endif()

# Note: All system libraries (ws2_32, Crypt32, pthreads, OpenGL, etc.)
# are automatically handled by linking the vcpkg targets.
# No manual platform-specific linking is needed.

# --- End of CMakeLists.txt ---
