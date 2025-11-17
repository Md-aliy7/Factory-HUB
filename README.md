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

Here’s a clean, well-formatted **README-style version** of your content in repo format with headings, code blocks, and emoji for readability:

---

# 📚 Dependencies & Build Guide

This project uses **vcpkg** for C++ dependency management and **CMake** as its build system. The `CMakeLists.txt` file in the project root tells Visual Studio and other tools how to build the code.

Follow the instructions below to set up vcpkg and build the project.

---

## 🚀 1. Install vcpkg (One-Time Setup)

If you haven't already, install **vcpkg** in a permanent location.

### Clone and Bootstrap vcpkg

Choose a folder (e.g., `C:\dev\vcpkg` or `~/dev/vcpkg`):

```bash
# Clone the vcpkg repository
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
```

**Windows (PowerShell/CMD):**

```dos
.\bootstrap-vcpkg.bat
```

**Linux (Bash):**

```bash
./bootstrap-vcpkg.sh
```

### Integrate with Visual Studio (Windows Only)

This allows Visual Studio 2022 to automatically find and use any libraries installed via vcpkg:

```dos
# Run this from the vcpkg folder
.\vcpkg integrate install
```

This avoids manually passing the `CMAKE_TOOLCHAIN_FILE` path to Visual Studio.

---

## 🚧 2. Build the Project (Windows & Linux)

### 🖥️ Path A: Windows with Visual Studio 2022 (Recommended)

#### Install Project Dependencies

Open PowerShell or CMD from your vcpkg directory:

```dos
# Note: The package name is 'zeromq', not 'libzmq'
.\vcpkg install glfw3 glew nlohmann-json zeromq paho-mqtt-c libmodbus open62541 --triplet=x64-windows
```

#### Open the Project in Visual Studio

1. Start Visual Studio 2022.
2. Go to `File > Open > Folder...`
3. Select your project's root folder (e.g., `D:\trial\Factory-HUB-main`).

#### Configure and Build

* Visual Studio reads the `CMakeLists.txt` file automatically.
* Installed vcpkg packages are detected automatically.
* The "Output" window will show: `"CMake configuration finished."`
* Select the build type (e.g., Release) from the toolbar.
* Select the "Startup Item" `FactoryHub.exe`.
* Press the green "Run" arrow (or F5) to build, run, and debug.

Alternatively, right-click `CMakeLists.txt` in Solution Explorer and select **Build**.

---

### 🐧 Path B: Linux with VS Code (Recommended)

#### Install Build Tools

```bash
sudo apt update
sudo apt install build-essential cmake git
```

#### Install Project Dependencies

From your vcpkg directory:

```bash
# Note: The package name is 'zeromq', not 'libzmq'
./vcpkg install glfw3 glew nlohmann-json zeromq paho-mqtt-c libmodbus open62541 --triplet=x64-linux
```

#### Open Project in VS Code

Install **Visual Studio Code** and the following extensions:

* **C/C++ Extension Pack**
* **CMake Tools**

Open your project folder:

```bash
cd /path/to/Factory-HUB
code .
```

#### Configure VS Code

* When prompted by CMake Tools, configure and select a compiler kit (GCC or Clang).
* If configuration fails due to vcpkg, press `Ctrl+Shift+P` → **Preferences: Open User Settings (JSON)** and add:

```json
"cmake.configureSettings": {
    "CMAKE_TOOLCHAIN_FILE": "/home/your-user/dev/vcpkg/scripts/buildsystems/vcpkg.cmake"
}
```

* Save the settings file.

#### Build and Run

* Use the **Build** button in the VS Code status bar (or press F7).
* Click **Run** (or `Ctrl+F5`) to execute `FactoryHub`.

---

## 📦 3. Dependency Analysis

### 1. Local Dependency (Manual Build)

**ImGui** (including `imgui_impl_glfw`, `imgui_impl_opengl3`)

* **Purpose:** Core GUI rendering system.
* **Build:** Included manually via `add_subdirectory(imgui)`. CMake builds ImGui from its own `imgui/CMakeLists.txt`.

### 2. vcpkg-Managed Dependencies

All other libraries are found via `find_package(...)` in `CMakeLists.txt`:

| Library           | Purpose                                                                 |
| ----------------- | ----------------------------------------------------------------------- |
| **glfw3**         | Window creation & input handling; creates the OpenGL context for ImGui. |
| **glew**          | Loads modern OpenGL functions; required by ImGui’s OpenGL3 backend.     |
| **nlohmann_json** | JSON serialization (Header-only).                                       |
| **zeromq**        | High-performance pub/sub messaging; used for `inproc://data_ingress`.   |
| **paho-mqtt-c**   | MQTT client library.                                                    |
| **libmodbus**     | Modbus TCP/RTU communication; used in `ModbusTCPAdapter`.               |
| **open62541**     | OPC-UA communication stack; used in `OpcuaAdapter`.                     |

---

If you want, I can also **add a clean “Quick Start” section with 1–2 command blocks** that combines Windows and Linux setup for even faster onboarding.

Do you want me to do that?
