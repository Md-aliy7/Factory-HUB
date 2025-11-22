/*
 * Multiprotocol Gateway Hub (V3 - fully cross-platform for Windows, Linux, and macOS)
 *
 * Architecture (Hub-Broker-Client Model):
 * 1. Main Thread: Runs Dear ImGui loop (Local Visualization).
 * 2. GatewayHub Class: Central manager. Owns Asio io_context.
 * 3. Asio Thread Pool: A fixed-size thread pool executes async work
 * for polling adapters (Modbus, OPC-UA).
 *
 * 4. Core Background Threads (DECOUPLED):
 *
 * - "Hot Path" (Data Uplink / Telemetry):
 * - RunDataProxy: Runs a fast ZMQ PULL->PUB proxy.
 * (PULL "inproc://data_ingress", PUB "inproc://data_pubsub")
 * - RunCloudLink: [NEW] Acts as an MQTT Client (Paho).
 * 1. Connects outbound to Central Broker (TCP/SSL).
 * 2. Subscribes to internal ZMQ "data_pubsub".
 * 3. Publishes data to "v1/hubs/<HUB_ID>/telemetry".
 * 4. Listens for commands on "v1/hubs/<HUB_ID>/rpc".
 *
 * - "Cold Path" (Local UI Visualization):
 * - RunAggregator: Subscribes to internal ZMQ "data_pubsub" on a
 * slow timer (500ms) to update the local ImGui device map.
 *
 * - "Command Path" (Cloud/UI to Device):
 * - RunCommandBridge: ZMQ Router. Handles commands from:
 * 1. Local ImGui inputs.
 * 2. Remote MQTT RPCs (via RouteCloudCommand).
 * 3. Adapter heartbeats.
 *
 * 5. Dynamic Adapters (IProtocolAdapter):
 * - Polling (Modbus, OPC-UA): Use Asio timers.
 * - Event-Based (MQTT, ZMQ): Use "thread-per-device" with Reaper.
 * - All adapters PUSH data to "inproc://data_ingress".
 *
 * 6. Scalability & Security:
 * - No open ports required on the Hub (Firewall friendly).
 * - Authentication via Mutual TLS (mTLS) or Token-based auth.
 * - Hub acts purely as an Edge Client.
 */

// --- Standard C++ Libraries ---
#include <iostream>
#include <string>
#include <thread>
#include <set>
#include <queue>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <memory>
#include <atomic>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cstddef>
#include <chrono>
#include <map>
#include <deque>
#include <nlohmann/json.hpp>

// --- Dear ImGui Includes ---
#include <GL/glew.h>
#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "GatewayUI.h"
#include "GatewayHub.h"

// =================================================================================
//
// Main Function
//
// =================================================================================

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "Glfw Error %d: %s\n", error, description);
    AddLog(std::string("Glfw Error: ") + description);
}

int main(int, char**) {
    // --- 1. Setup GLFW ---
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    // --- 2. Setup Window + OpenGL + GLEW ---
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Multiprotocol Gateway Hub", NULL, NULL);
    if (window == NULL)
        return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    if (glewInit() != GLEW_OK) {
        fprintf(stderr, "Failed to initialize GLEW\n");
        return 1;
    }

    // --- 3. Setup Dear ImGui ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // --- 4. Start the GatewayHub ---
    static GatewayHub hub;
    hub.Start();

    // --- 5. Main Render Loop ---
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        DrawGatewayUI(hub); // Render our UI

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // --- 6. Cleanup ---
    hub.Stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}



