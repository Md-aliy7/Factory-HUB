# ⚙️ Hybrid-Gateway-Hub

**High-Performance Multiprotocol Gateway Hub with Integrated Dear ImGui UI**

This project implements a robust, cross-platform gateway designed to bridge industrial protocols with modern, high-speed interfaces. It leverages a **hybrid asynchronous architecture** using **Boost.Asio** and **ZeroMQ (ZMQ)** for data handling, and **uWS** for low-latency WebSocket distribution, all managed through an interactive **Dear ImGui** user interface.

***

## ✨ Key Features

* **Hybrid Asynchronous Core:** Combines a fixed-size **Boost.Asio Thread Pool** for non-blocking I/O and scheduled tasks (timers) with dedicated worker threads for core services (**ZMQ Bridge**, **uWS server**).
* **Centralized ZeroMQ Bus:** Utilizes a **thread-safe ZMQ PUSH/PULL ("inproc://...")** mechanism to establish a high-throughput internal bus for adapters to send data streams to the central ZMQ Bridge.
* **Multiprotocol Support:** Abstracted via the `IProtocolAdapter` interface, allowing for seamless integration of various industrial and messaging protocols (e.g., **Modbus**, **OPC-UA**, **MQTT**, **ZMQ**).
* **Integrated UI & Logic:** The user interface, built with **Dear ImGui**, is merged with backend logic for real-time control, device configuration, integrated logging, and status monitoring.
* **High-Speed Distribution:** Employs the highly efficient **uWS (µSockets)** library to host a WebSocket server, enabling low-latency data distribution to web clients.

***

## 🏗️ System Architecture

The architecture is built upon a **One-to-Many** design, separating the UI, central management, concurrent processing, and I/O logic into distinct components.

| Component | Responsibility | Technologies / Concept |
| :--- | :--- | :--- |
| **1. Main Thread** | Executes the application's rendering and input loop. | **Dear ImGui**, GLFW/OpenGL |
| **2. GatewayHub Class** | Central manager. Owns the Asio `io_context` and orchestrates all services and protocol adapters. | C++, **Boost.Asio** |
| **3. Asio Thread Pool** | Fixed-size pool running `io_context.run()`. Executes all posted non-blocking I/O and scheduled work. | **Boost.Asio** |
| **4. ZMQ Bridge Thread** | The core data handler. **PULLs** all aggregated data from the ZMQ Internal Bus, performs state updates, and prepares data for WebSocket distribution. | **ZeroMQ (ZMQ)** |
| **5. uWS Thread** | Dedicated thread running the **uS::Loop** for the WebSocket server, handling all client connections and data broadcast. | **uWS (µSockets)** |

***

## 🔌 Protocol Adapter Design (`IProtocolAdapter`)

The `IProtocolAdapter` defines a *service manager* responsible for a group of devices. The architecture allows for two distinct I/O models based on the protocol requirements:

| Adapter Type | I/O Model | Implementation Approach | Key Technologies |
| :--- | :--- | :--- | :--- |
| **Polling** (e.g., Modbus, OPC-UA) | **Fully Asynchronous** | Uses `boost::asio::steady_timer` to schedule poll intervals. The *blocking* I/O operation is posted as work to the **Asio Thread Pool**. **No dedicated threads** are created by the adapter. | **Boost.Asio** |
| **Event-Based** (e.g., MQTT, ZMQ) | **Thread-Per-Device** | Uses a dedicated worker thread per device/connection. This thread blocks efficiently on network events (e.g., `recv` or message callbacks), which is a scalable approach for event-driven protocols. | C++ `std::thread` |
```
