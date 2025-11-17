# ⚙️ Hybrid-Gateway-Hub

**High-Performance Multiprotocol Gateway Hub with Integrated Dear ImGui UI**

[](https://www.google.com/search?q=https://github.com/your-username/your-repo)
[](https://www.google.com/search?q=https://github.com/your-username/your-repo/blob/main/LICENSE)
[](https://isocpp.org/std/status)

This project implements a robust, cross-platform gateway designed to bridge industrial protocols with modern, high-speed interfaces. It leverages a **hybrid asynchronous architecture** using **Boost.Asio** for scheduled I/O, a **decoupled multi-threaded ZeroMQ (ZMQ) pipeline** for data routing, and **uWS** for low-latency WebSocket distribution, all managed through an interactive **Dear ImGui** user interface.

---

## ✨ Key Features

- **Hybrid Asynchronous Core:** Combines a fixed-size **Boost.Asio Thread Pool** for efficient, non-blocking I/O and scheduled tasks (timers) with dedicated worker threads for core services.
- **Decoupled Data Pipeline (V3):** The architecture is built on a high-performance **ZMQ PUB/SUB proxy**. This design separates data flow into independent, lock-free paths:
  - **Hot Path:** For ultra-low-latency, real-time data delivery to web clients.
  - **Cold Path:** For throttled, low-priority data updates to the local ImGui UI, ensuring smoothness.
  - **Command Path:** For isolated, UI-to-device command routing.
- **Multiprotocol Support:** Abstracted via the `IProtocolAdapter` interface, allowing for seamless integration of various industrial and messaging protocols (**Modbus**, **OPC-UA**, **MQTT**, **ZMQ**).
- **Integrated ImGui UI:** A real-time user interface for device configuration, status monitoring, and live data logging, built directly into the application.
- **High-Speed Web Distribution:** Employs the highly efficient **uWS (µSockets)** library to host a WebSocket server, enabling data distribution to thousands of web clients with minimal overhead.

---

## 🏗️ System Architecture (V3)

The architecture is built upon a **decoupled, multi-threaded pipeline** that separates data ingestion, processing, and distribution.

| Component                | Responsibility                                                                                                                                   | Technologies                |
| :----------------------- | :----------------------------------------------------------------------------------------------------------------------------------------------- | :-------------------------- |
| **1. Main Thread**       | Executes the application's render and input loop.                                                                                                | **Dear ImGui**, GLFW/OpenGL |
| **2. Asio Thread Pool**  | A fixed-size pool running `io_context.run()`. Executes all posted non-blocking I/O and scheduled work for **polling adapters** (Modbus, OPC-UA). | **Boost.Asio**              |
| **3. Core Hub Threads**  | A set of dedicated threads forming the V3 data pipeline.                                                                                         | **ZMQ (inproc)**            |
| └── **RunDataProxy**     | The "heart" of the data plane. Runs a high-speed ZMQ PULL-to-PUB proxy to multiplex all incoming device data.                                    | ZMQ `PULL` -\> `PUB`        |
| └── **RunUwsSubscriber** | The **"Hot Path"**. Subscribes to the proxy and immediately pushes real-time data to the WebSocket server's publish queue.                       | ZMQ `SUB`                   |
| └── **RunAggregator**    | The **"Cold Path"**. Subscribes to the proxy on a slow, 100ms timer to update the local ImGui device map without blocking.                       | ZMQ `SUB`                   |
| └── **RunCommandBridge** | The **"Command Path"**. A ZMQ `ROUTER` that routes commands from the UI (web or local) to the correct protocol adapter.                          | ZMQ `ROUTER`                |
| **4. uWS Thread**        | Dedicated thread running the **uS::Loop** for the WebSocket server, handling all client connections and data broadcasts.                         | **uWS (µSockets)**          |

---

## 💾 Code Structure (MVC)

As a result of refactoring, the project follows a **Model-View-Controller (MVC)** pattern, separating the codebase into three logical components.

| Component        | Files                              | Responsibilities                                                                                                           |
| :--------------- | :--------------------------------- | :------------------------------------------------------------------------------------------------------------------------- |
| **Bootstrapper** | `main.cpp`                         | Initializes GLFW, ImGui, and the `GatewayHub`. Runs the main render loop.                                                  |
| **View**         | `GatewayUI.h`<br>`GatewayUI.cpp`   | Contains all Dear ImGui drawing logic. Declares and implements `DrawGatewayUI()`. Holds all UI-specific state and buffers. |
| **Model**        | `GatewayHub.h`<br>`GatewayHub.cpp` | The application "engine." Contains all backend logic, global variables, and third-party includes.                          |

### The "Mega-Header" Design

Currently, `GatewayHub.h` contains the definitions for the `GatewayHub` class, the `IProtocolAdapter` interface, and _all_ concrete adapter classes (Modbus, MQTT, etc.). `GatewayHub.cpp` contains the implementations for all of these classes.

**Future Improvement:** This design could be further modularized by splitting each adapter into its own file set (e.g., `MqttAdapter.h`, `MqttAdapter.cpp`).

---

## 🔌 Protocol Adapter Design (`IProtocolAdapter`)

The `IProtocolAdapter` defines a _service manager_ responsible for a group of devices. The architecture allows for two distinct I/O models based on the protocol requirements:

| Adapter Type                       | I/O Model                     | Implementation Approach                                                                                                                                                                                              | Key Technologies  |
| :--------------------------------- | :---------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :---------------- |
| **Polling** (e.g., Modbus, OPC-UA) | **Asynchronous / Task-Based** | Uses `boost::asio::steady_timer` to schedule poll intervals. The _blocking_ I/O operation is posted as a task to the **Asio Thread Pool**. **No dedicated threads** are created by the adapter.                      | **Boost.Asio**    |
| **Event-Based** (e.g., MQTT, ZMQ)  | **Thread-Per-Device**         | Uses a dedicated worker thread (`std::thread`) per device/connection. This thread blocks efficiently on network events (e.g., `recv` or message callbacks), which is a scalable approach for event-driven protocols. | C++ `std::thread` |

---

# 📚 Dependencies & Build Guide

This project uses **vcpkg** for C++ dependency management. Visual Studio or your preferred IDE will handle compilation directly — no `CMakeLists.txt` is required.

This project uses **CMake** with **CPM (C++ Package Manager)** to automatically download and build all dependencies. The following libraries are managed automatically:

- **Core:** Boost (Asio, System)
- **Messaging:** ZMQ (libzmq, cppzmq), Paho MQTT C
- **Web:** uWebSockets (uWS), OpenSSL
- **UI:** Dear ImGui (local copy), GLFW3, GLEW
- **Protocols:** libmodbus, open62541
- **Utilities:** nlohmann/json

### System Requirements

**Linux:**

- CMake 3.20 or higher
- C++17 compatible compiler (GCC 7+, Clang 5+)
- OpenGL development libraries
- OpenSSL development libraries
- Build tools (make, g++, etc.)

Install on Ubuntu/Debian:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libgl1-mesa-dev libssl-dev
```

Install on Arch Linux:

```bash
sudo pacman -S base-devel cmake mesa openssl
```

**macOS:**

- CMake 3.20 or higher
- Xcode Command Line Tools
- Homebrew (optional, for OpenSSL)

**Windows:**

- CMake 3.20 or higher
- Visual Studio 2019 or later with C++ support
- OpenSSL (can be installed via vcpkg or downloaded)

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

This step lets Visual Studio automatically find and use any libraries installed via vcpkg:

```dos
# Run this from the vcpkg folder
.\vcpkg integrate install
```

This avoids manually setting include/lib paths for your dependencies.

---

## 🚧 2. Install Project Dependencies

From your **vcpkg** directory, install all required libraries:

```bash
# Windows
.\vcpkg install glfw3 glew nlohmann-json zeromq paho-mqtt-c libmodbus open62541 boost-asio uwebsockets --triplet=x64-windows

# Linux
./vcpkg install glfw3 glew nlohmann-json zeromq paho-mqtt-c libmodbus open62541 boost-asio uwebsockets --triplet=x64-linux
```

---

## 🖥️ 3. Build the Project

### Windows (Visual Studio 2022)

1. Open Visual Studio 2022.
2. Go to `File > Open > Project/Solution` and select your `.sln` or `.vcxproj` file.
3. Visual Studio will detect vcpkg-managed libraries automatically.
4. Choose a build configuration (e.g., Release).
5. Press **F5** or click the green arrow to build and run.

### Linux (VS Code or Terminal)

1. Open the project folder in VS Code or your terminal.
2. Make sure you have a C++ compiler installed:

```bash
sudo apt update
sudo apt install build-essential git
```

3. Open your IDE and set the include/lib paths to point to your vcpkg folder if necessary.
4. Build your project using your IDE or a `makefile` if one exists:

```bash
g++ -std=c++20 -I/path/to/vcpkg/installed/x64-linux/include *.cpp -L/path/to/vcpkg/installed/x64-linux/lib -lglfw -lglew -lzmq -lpaho-mqtt3c -lmodbus -lopen62541 -lboost_system -luWS -o FactoryHub
```

5. Run the executable:

```bash
./FactoryHub
```

---

## 📦 4. Dependency Analysis

### 1. Local Dependencies (Manual Build)

**ImGui** (including `imgui_impl_glfw`, `imgui_impl_opengl3`)

* **Purpose:** Core GUI rendering system.
* **Build:** Included manually in the project source folder.

### 2. vcpkg-Managed Dependencies

| Library           | Purpose                                                                 |
| ----------------- | ----------------------------------------------------------------------- |
| **glfw3**         | Window creation & input handling; creates the OpenGL context for ImGui. |
| **glew**          | Loads modern OpenGL functions; required by ImGui’s OpenGL3 backend.     |
| **nlohmann_json** | JSON serialization (Header-only).                                       |
| **zeromq**        | High-performance pub/sub messaging; used for `inproc://data_ingress`.   |
| **paho-mqtt-c**   | MQTT client library.                                                    |
| **libmodbus**     | Modbus TCP/RTU communication; used in `ModbusTCPAdapter`.               |
| **open62541**     | OPC-UA communication stack; used in `OpcuaAdapter`.                     |
| **boost-asio**    | Asynchronous networking & I/O operations.                               |
| **uWebSockets**   | High-performance WebSocket server/client library.                       |

1. **Clone the repository:**

   ```bash
   git clone https://github.com/your-username/Factory-HUB.git
   cd Factory-HUB
   ```

2. **Create build directory and configure:**

   ```bash
   mkdir build
   cd build
   cmake ..
   ```

3. **Build the project:**

   ```bash
   cmake --build . --config Release
   ```

   Or on Linux/macOS:

   ```bash
   make -j$(nproc)
   ```

4. **Run the application:**

   ```bash
   # Linux/macOS
   ./bin/Factory-HUB

   # Windows
   .\bin\Release\Factory-HUB.exe
   ```

### Quick Build (One-liner)

```bash
mkdir -p build && cd build && cmake .. && cmake --build . -j$(nproc) && ./bin/Factory-HUB
```

### Troubleshooting

**If CMake fails to find OpenSSL:**

- Linux: Install `libssl-dev` (Ubuntu/Debian) or `openssl` (Arch)
- macOS: `brew install openssl` then set `-DOPENSSL_ROOT_DIR=/usr/local/opt/openssl`
- Windows: Install OpenSSL via vcpkg or download from https://slproweb.com/products/Win32OpenSSL.html

**If build fails with linking errors:**

- Ensure all system dependencies are installed
- Try cleaning the build directory: `rm -rf build && mkdir build`
- Check that your compiler supports C++17

**If dependencies fail to download:**

- Check your internet connection
- Some dependencies are large (Boost, uWebSockets) - first build may take 10-20 minutes
- You can use a local cache by setting `CPM_SOURCE_CACHE` environment variable
