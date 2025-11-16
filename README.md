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

-----

## 🛠️ Dependencies & Building

### Dependencies

This project is built using **CMake** and requires several libraries, preferably managed by a package manager like **vcpkg**.

  * **Core:** Boost (Asio, System)
  * **Messaging:** ZMQ (libzmq, cppzmq), Paho MQTT C
  * **Web:** uWebSockets (uWS), OpenSSL
  * **UI:** Dear ImGui, GLFW3, GLEW
  * **Protocols:** libmodbus, open62541
  * **Utilities:** nlohmann/json

### Build Instructions

1.  Clone the repository and its submodules (if any):
    ```bash
    git clone --recurse-submodules https://github.com/your-username/Hybrid-Gateway-Hub.git
    cd Hybrid-Gateway-Hub
    ```
2.  Install dependencies (e.g., using vcpkg):
    ```bash
    vcpkg install boost-asio zmq cppzmq uwebsockets paho-mqtt-c libmodbus open62541 nlohmann-json glfw3 glew
    ```
3.  Configure and build with CMake:
    ```bash
    cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake
    cmake --build build --config Release
    ```
4.  Run the application:
    ```bash
    ./build/Release/HybridGatewayHub
    ```
