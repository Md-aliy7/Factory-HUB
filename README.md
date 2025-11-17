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
- Visual Studio 2019 or later with C++ support (MSVC compiler)
- Git for Windows (for cloning dependencies)
- OpenSSL development libraries
- OpenGL support (usually included with graphics drivers)

Install using one of these methods:

**Option 1: Using Chocolatey (Recommended)**

```powershell
# Install Chocolatey first (run PowerShell as Administrator)
Set-ExecutionPolicy Bypass -Scope Process -Force; [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072; iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))

# Install dependencies
choco install cmake git visualstudio2022buildtools -y
```

**Option 2: Manual Installation**

1. Download and install [CMake](https://cmake.org/download/) (Windows x64 Installer)
2. Download and install [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) with "Desktop development with C++" workload
3. Download and install [Git for Windows](https://git-scm.com/download/win)
4. Download OpenSSL from [Win32/Win64 OpenSSL](https://slproweb.com/products/Win32OpenSSL.html) or use vcpkg (see below)

## 🖥️ Building the Project

### Windows Build Instructions

**Method 1: Using Visual Studio (Recommended)**

1. **Open Developer Command Prompt:**

   - Press `Win + X` and select "Developer Command Prompt for VS 2022"
   - Or open "x64 Native Tools Command Prompt for VS 2022" from Start Menu

2. **Navigate to project directory:**

   ```cmd
   cd C:\path\to\Factory-HUB
   ```

3. **Create build directory and configure:**

   ```cmd
   mkdir build
   cd build
   cmake .. -G "Visual Studio 17 2022" -A x64
   ```

4. **Build the project:**

   ```cmd
   cmake --build . --config Release
   ```

5. **Run the application:**
   ```cmd
   .\bin\Release\Factory-HUB.exe
   ```

**Method 2: Using CMake GUI**

1. Open CMake GUI
2. Set "Where is the source code:" to your project directory
3. Set "Where to build the binaries:" to `build` subdirectory
4. Click "Configure" and select "Visual Studio 17 2022" as generator
5. Click "Generate"
6. Open the generated `Factory-HUB.sln` in Visual Studio
7. Build the solution (F7 or Build > Build Solution)
8. Run from Visual Studio (F5) or find the executable in `build\bin\Release\`

**Method 3: Using PowerShell/Terminal**

```powershell
# Navigate to project
cd C:\path\to\Factory-HUB

# Create build directory
mkdir build
cd build

# Configure (using Ninja for faster builds, optional)
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release

# Or use Visual Studio generator
cmake .. -G "Visual Studio 17 2022" -A x64

# Build
cmake --build . --config Release

# Run
.\bin\Release\Factory-HUB.exe
```

**Installing OpenSSL on Windows (if needed):**

If CMake fails to find OpenSSL, you have two options:

**Option A: Using vcpkg (Recommended for Windows)**

```powershell
# Clone vcpkg (one-time setup)
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat

# Install OpenSSL
.\vcpkg install openssl:x64-windows

# Integrate with Visual Studio (optional but recommended)
.\vcpkg integrate install

# When configuring CMake, specify vcpkg toolchain:
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
```

**Option B: Manual OpenSSL Installation**

1. Download OpenSSL from [Win32/Win64 OpenSSL](https://slproweb.com/products/Win32OpenSSL.html)
2. Install to `C:\OpenSSL-Win64` (or your preferred location)
3. Configure CMake with:
   ```cmd
   cmake .. -DOPENSSL_ROOT_DIR=C:\OpenSSL-Win64
   ```

### Linux Build Instructions

1. **Install system dependencies** (if not already installed):

```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install -y build-essential cmake libgl1-mesa-dev libssl-dev libmodbus-dev libglew-dev

# Arch Linux
sudo pacman -S base-devel cmake mesa openssl libmodbus glew
```

2. **Clone the repository:**

   ```bash
   git clone https://github.com/your-username/Factory-HUB.git
   cd Factory-HUB
   ```

3. **Create build directory and configure:**

   ```bash
   mkdir build
   cd build
   cmake ..
   ```

4. **Build the project:**

   ```bash
   cmake --build . -j$(nproc)
   ```

5. **Run the application:**
   ```bash
   ./bin/Factory-HUB
   ```

### macOS Build Instructions

1. **Install dependencies:**

   ```bash
   # Install Homebrew if not already installed
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

   # Install required packages
   brew install cmake openssl
   ```

2. **Build the project:**
   ```bash
   mkdir build && cd build
   cmake .. -DOPENSSL_ROOT_DIR=/usr/local/opt/openssl
   cmake --build . -j$(sysctl -n hw.ncpu)
   ./bin/Factory-HUB
   ```

---

## 📦 4. Dependency Analysis

### 1. Local Dependencies (Manual Build)

**ImGui** (including `imgui_impl_glfw`, `imgui_impl_opengl3`)

- **Purpose:** Core GUI rendering system.
- **Build:** Included manually in the project source folder.

### 2. CPM-Managed Dependencies (Automatically Downloaded & Built)

| Library           | Purpose                                                                 | Notes                              |
| ----------------- | ----------------------------------------------------------------------- | ---------------------------------- |
| **glfw3**         | Window creation & input handling; creates the OpenGL context for ImGui. | Built from source via CPM          |
| **glew**          | Loads modern OpenGL functions; required by ImGui's OpenGL3 backend.     | Uses system library if available   |
| **nlohmann_json** | JSON serialization (Header-only).                                       | Header-only, no compilation needed |
| **zeromq**        | High-performance pub/sub messaging; used for `inproc://data_ingress`.   | Built from source via CPM          |
| **paho-mqtt-c**   | MQTT client library.                                                    | Built from source via CPM          |
| **libmodbus**     | Modbus TCP/RTU communication; used in `ModbusTCPAdapter`.               | Uses system library if available   |
| **open62541**     | OPC-UA communication stack; used in `OpcuaAdapter`.                     | Built from source via CPM          |
| **boost-asio**    | Asynchronous networking & I/O operations.                               | Built from source via CPM          |
| **uWebSockets**   | High-performance WebSocket server/client library.                       | Built from source via CPM          |
| **zlib**          | Compression library (required by uWebSockets).                          | Built from source via CPM          |
| **uSockets**      | Low-level socket library (required by uWebSockets).                     | Built from uWebSockets submodule   |

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

   # Windows (Command Prompt)
   .\bin\Release\Factory-HUB.exe

   # Windows (PowerShell)
   .\bin\Release\Factory-HUB.exe
   ```

### Quick Build (One-liner)

**Linux/macOS:**

```bash
mkdir -p build && cd build && cmake .. && cmake --build . -j$(nproc) && ./bin/Factory-HUB
```

**Windows (PowerShell):**

```powershell
mkdir build; cd build; cmake .. -G "Visual Studio 17 2022" -A x64; cmake --build . --config Release; .\bin\Release\Factory-HUB.exe
```

**Windows (Command Prompt):**

```cmd
mkdir build && cd build && cmake .. -G "Visual Studio 17 2022" -A x64 && cmake --build . --config Release && .\bin\Release\Factory-HUB.exe
```

### Troubleshooting

**If CMake fails to find OpenSSL:**

- **Linux:** Install `libssl-dev` (Ubuntu/Debian) or `openssl` (Arch)
- **macOS:** `brew install openssl` then set `-DOPENSSL_ROOT_DIR=/usr/local/opt/openssl`
- **Windows:**
  - Install via vcpkg: `.\vcpkg install openssl:x64-windows` and use `-DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake`
  - Or download from [Win32/Win64 OpenSSL](https://slproweb.com/products/Win32OpenSSL.html) and set `-DOPENSSL_ROOT_DIR=C:\OpenSSL-Win64`

**Windows-specific issues:**

- **"CMake Error: Could not find CMAKE_ROOT"**: Make sure CMake is installed and added to PATH, or use full path to cmake.exe
- **"LNK1104: cannot open file 'libmodbus.lib'"**: Install libmodbus via vcpkg or ensure system libraries are available
- **"Cannot open include file: 'GL/glew.h'"**: Install GLEW via vcpkg or ensure OpenGL development libraries are installed
- **Build errors with uSockets**: The uSockets library is built automatically from the uWebSockets submodule. If it fails, ensure you have a C compiler (cl.exe) available in your PATH

**If build fails with linking errors:**

- Ensure all system dependencies are installed
- Try cleaning the build directory: `rm -rf build && mkdir build`
- Check that your compiler supports C++20

**If dependencies fail to download:**

- Check your internet connection
- Some dependencies are large (Boost, uWebSockets) - first build may take 10-20 minutes
- You can use a local cache by setting `CPM_SOURCE_CACHE` environment variable:
  - **Windows:** `set CPM_SOURCE_CACHE=C:\path\to\cache` (Command Prompt) or `$env:CPM_SOURCE_CACHE="C:\path\to\cache"` (PowerShell)
  - **Linux/macOS:** `export CPM_SOURCE_CACHE=/path/to/cache`

**Windows firewall/antivirus:**

- Some antivirus software may interfere with downloading dependencies
- If downloads fail, temporarily disable antivirus or add CMake build directory to exclusions
- Windows Defender may need to allow CMake and Git through the firewall
