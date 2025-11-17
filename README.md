# ⚙️ Hybrid-Gateway-Hub

**High-Performance Multiprotocol Gateway Hub with Integrated Dear ImGui UI**

[](https://www.google.com/search?q=https://github.com/your-username/your-repo)
[](https://www.google.com/search?q=https://github.com/your-username/your-repo/blob/main/LICENSE)
[](https://isocpp.org/std/status)

This project implements a robust, cross-platform gateway designed to bridge industrial protocols with modern, high-speed interfaces. It leverages a **hybrid asynchronous architecture** using **Boost.Asio** for scheduled I/O, a **decoupled multi-threaded ZeroMQ (ZMQ) pipeline** for data routing, and **uWS** for low-latency WebSocket distribution, all managed through an interactive **Dear ImGui** user interface.

-----

## ✨ Key Features

  * **Hybrid Asynchronous Core:** Combines a fixed-size **Boost.Asio Thread Pool** for efficient, non-blocking I/O and scheduled tasks (timers) with dedicated worker threads for core services.
  * **Decoupled Data Pipeline (V3):** The architecture is built on a high-performance **ZMQ PUB/SUB proxy**. This design separates data flow into independent, lock-free paths:
      * **Hot Path:** For ultra-low-latency, real-time data delivery to web clients.
      * **Cold Path:** For throttled, low-priority data updates to the local ImGui UI, ensuring smoothness.
      * **Command Path:** For isolated, UI-to-device command routing.
  * **Multiprotocol Support:** Abstracted via the `IProtocolAdapter` interface, allowing for seamless integration of various industrial and messaging protocols (**Modbus**, **OPC-UA**, **MQTT**, **ZMQ**).
  * **Integrated ImGui UI:** A real-time user interface for device configuration, status monitoring, and live data logging, built directly into the application.
  * **High-Speed Web Distribution:** Employs the highly efficient **uWS (µSockets)** library to host a WebSocket server, enabling data distribution to thousands of web clients with minimal overhead.

-----

## 🏗️ System Architecture (V3)

The architecture is built upon a **decoupled, multi-threaded pipeline** that separates data ingestion, processing, and distribution.

| Component | Responsibility | Technologies |
| :--- | :--- | :--- |
| **1. Main Thread** | Executes the application's render and input loop. | **Dear ImGui**, GLFW/OpenGL |
| **2. Asio Thread Pool** | A fixed-size pool running `io_context.run()`. Executes all posted non-blocking I/O and scheduled work for **polling adapters** (Modbus, OPC-UA). | **Boost.Asio** |
| **3. Core Hub Threads** | A set of dedicated threads forming the V3 data pipeline. | **ZMQ (inproc)** |
| └── **RunDataProxy** | The "heart" of the data plane. Runs a high-speed ZMQ PULL-to-PUB proxy to multiplex all incoming device data. | ZMQ `PULL` -\> `PUB` |
| └── **RunUwsSubscriber** | The **"Hot Path"**. Subscribes to the proxy and immediately pushes real-time data to the WebSocket server's publish queue. | ZMQ `SUB` |
| └── **RunAggregator** | The **"Cold Path"**. Subscribes to the proxy on a slow, 100ms timer to update the local ImGui device map without blocking. | ZMQ `SUB` |
| └── **RunCommandBridge** | The **"Command Path"**. A ZMQ `ROUTER` that routes commands from the UI (web or local) to the correct protocol adapter. | ZMQ `ROUTER` |
| **4. uWS Thread** | Dedicated thread running the **uS::Loop** for the WebSocket server, handling all client connections and data broadcasts. | **uWS (µSockets)** |

-----

## 💾 Code Structure (MVC)

As a result of refactoring, the project follows a **Model-View-Controller (MVC)** pattern, separating the codebase into three logical components.

| Component | Files | Responsibilities |
| :--- | :--- | :--- |
| **Bootstrapper** | `main.cpp` | Initializes GLFW, ImGui, and the `GatewayHub`. Runs the main render loop. |
| **View** | `GatewayUI.h`<br>`GatewayUI.cpp` | Contains all Dear ImGui drawing logic. Declares and implements `DrawGatewayUI()`. Holds all UI-specific state and buffers. |
| **Model** | `GatewayHub.h`<br>`GatewayHub.cpp` | The application "engine." Contains all backend logic, global variables, and third-party includes. |

### The "Mega-Header" Design

Currently, `GatewayHub.h` contains the definitions for the `GatewayHub` class, the `IProtocolAdapter` interface, and *all* concrete adapter classes (Modbus, MQTT, etc.). `GatewayHub.cpp` contains the implementations for all of these classes.

**Future Improvement:** This design could be further modularized by splitting each adapter into its own file set (e.g., `MqttAdapter.h`, `MqttAdapter.cpp`).

-----

## 🔌 Protocol Adapter Design (`IProtocolAdapter`)

The `IProtocolAdapter` defines a *service manager* responsible for a group of devices. The architecture allows for two distinct I/O models based on the protocol requirements:

| Adapter Type | I/O Model | Implementation Approach | Key Technologies |
| :--- | :--- | :--- | :--- |
| **Polling** (e.g., Modbus, OPC-UA) | **Asynchronous / Task-Based** | Uses `boost::asio::steady_timer` to schedule poll intervals. The *blocking* I/O operation is posted as a task to the **Asio Thread Pool**. **No dedicated threads** are created by the adapter. | **Boost.Asio** |
| **Event-Based** (e.g., MQTT, ZMQ) | **Thread-Per-Device** | Uses a dedicated worker thread (`std::thread`) per device/connection. This thread blocks efficiently on network events (e.g., `recv` or message callbacks), which is a scalable approach for event-driven protocols. | C++ `std::thread` |

---

# 🛠️ Report: Dependencies & Building Guide

## 🚧 How to Build Your Project

With the provided `CMakeLists.txt` in your project root, any developer can build the project using the following steps:

### **Bash**

```bash
# 1. Create a build directory (out-of-source build)
mkdir build
cd build

# 2. Configure the project (CMake downloads all dependencies)

# On Windows (e.g., Visual Studio 2022)
cmake .. -G "Visual Studio 17 2022"

# On Linux
cmake ..

# 3. Build the project

# On Windows (via CMake)
cmake --build . --config Release

# On Linux
cmake --build .
# or simply:
make
```

---

## 📦 Dependency Analysis

Your project integrates several powerful libraries. Here's how each dependency is managed and used.

---

### **1. Local Dependency (Manually Managed)**

#### **ImGui** (including `imgui_impl_glfw`, `imgui_impl_opengl3`)

* **Purpose:** Provides the complete graphical user interface (GUI).
* **Build Method:**

  * Managed manually (not through CPM).
  * Included via:

    ```cmake
    add_subdirectory(imgui)
    ```
  * CMake loads `imgui/CMakeLists.txt` and builds it as part of the project.

---

### **2. CPM-Managed Dependencies (Automatically Downloaded by CMake)**

Using `CPMAddPackage`, CMake fetches specific versions directly from GitHub—no system installs required.

#### **glfw**

* **Purpose:** Creates the OS window, handles input, and initializes the OpenGL context for ImGui.

#### **glew**

* **Purpose:** Loads modern OpenGL functions; required by the ImGui OpenGL3 backend.

#### **nlohmann_json**

* **Purpose:** JSON parsing/serialization (e.g., `SendTestCommand`).
* **Type:** Header-only.

#### **libzmq (ZeroMQ)**

* **Purpose:** High-performance PUB/SUB messaging (e.g., `inproc://data_ingress`).

#### **paho-mqtt-c**

* **Purpose:** Eclipse Paho MQTT C client for broker communication.

#### **libmodbus**

* **Purpose:** Modbus TCP/RTU communication used in `ModbusTCPAdapter`.

#### **open62541**

* **Purpose:** OPC-UA implementation used by your `OpcuaAdapter`.
* **Build Options:**

  * `UA_ENABLE_AMALGAMATION ON` for a single generated C file.
