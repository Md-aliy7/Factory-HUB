# **Multiprotocol Gateway Hub (V3) – Architectural Analysis Report**

This document details the software architecture of the **Multiprotocol Gateway Hub (V3)**, a high-performance, multi-threaded C++ application designed for industrial data aggregation.

---

## **1. 📝 Executive Summary**

The V3 architecture is a sophisticated **Hybrid Asynchronous Model** built on **Boost.Asio** and **ZeroMQ (ZMQ)**. Its core innovation is a fully **decoupled data pipeline** utilizing a central **ZMQ PUB/SUB proxy**.

This design eliminates the data-path mutexes and single-thread bottlenecks of previous versions, enabling massive scalability. The system separates the data flow into three distinct, non-blocking paths:

* **Hot Path (Web Data):** A lock-free, high-throughput path for real-time device data to web clients.
* **Cold Path (GUI Data):** A low-priority, throttled path for updating the local Dear ImGui GUI.
* **Command Path (Control):** An isolated path for commands from the UI to devices.

The architecture also employs a hybrid adapter model, using the efficient **Boost.Asio task pool** for polling protocols (Modbus, OPC-UA) and a robust **thread-per-device** model with an asynchronous **Reaper** for event-driven protocols (MQTT, ZMQ).

---

## **2. ⚙️ Core Architectural Components & Threading Model**

The application's scalability relies on dividing work across a dynamic set of threads, each with a single, dedicated responsibility.

### **2.1. Main Application Thread**

* **Thread:** 1
* **Role:** Runs `glfwPollEvents()` and renders the Dear ImGui UI. Represents the “view” and handles all user input.

### **2.2. Asio Worker Pool**

* **Threads:** *N* (typically `hardware_concurrency`)
* **Role:** Worker pool for asynchronous/polling-based tasks (Modbus, OPC-UA). Executes `DoBlockingPoll` without requiring per-device threads.

### **2.3. Core Hub Threads (V3 Decoupled System)**

| Thread Name          | Count | Data Path | Description             | Role                                                            |
| -------------------- | ----- | --------- | ----------------------- | --------------------------------------------------------------- |
| **RunDataProxy**     | 1     | Hot/Cold  | Heart of the data plane | Runs `zmq::proxy` bridging ingress → pubsub with **zero locks** |
| **RunUwsSubscriber** | 1     | Hot Path  | High-priority web path  | SUB from pubsub → queue → wakes uWS                             |
| **RunAggregator**    | 1     | Cold Path | Low-priority GUI path   | SUB from pubsub → 100 ms timer → updates `m_devices`            |
| **RunCommandBridge** | 1     | Cmd Path  | Command router          | Forwards UI commands to adapter DEALER sockets                  |
| **RunUwsServer**     | 1     | Hot Path  | WebSocket server        | Publishes queued hot-path data to clients                       |

### **2.4. Dynamic Adapter Threads**

| Thread Name                     | Count         | Purpose        | Role                                  |
| ------------------------------- | ------------- | -------------- | ------------------------------------- |
| **IProtocolAdapter::Run**       | 1 per Service | Command intake | Listens for DEALER messages           |
| **Event-Driven Worker Threads** | 1 per Device  | Data ingress   | Blocking MQTT/ZMQ loops → ZMQ ingress |
| **Reaper Threads**              | 1 per Service | Cleanup        | Joins stopped workers asynchronously  |

---

## **3. 📊 Data Flow Analysis (V3 Model)**

### **3.1. Data Ingress (Device → UI)**

The system bifurcates the data plane into **Hot (Web)** and **Cold (GUI)** paths.

1. Device worker formats JSON.
2. Worker **PUSHes** to `inproc://data_ingress`.
3. **RunDataProxy** PULLs and **PUBs** to `inproc://data_pubsub`.
4. Data fans out:

| Hot Path (Web)                               | Cold Path (Local GUI)                  |
| -------------------------------------------- | -------------------------------------- |
| 5a. `RunUwsSubscriber` receives instantly    | 5b. `RunAggregator` receives instantly |
| 6a. Pushes into `m_publish_queue` + wake uWS | 6b. Buffers until 100 ms timer         |
| 7a. Notifies server via `loop->defer()`      | 7b. Updates `m_devices` with one lock  |
| 8a. `RunUwsServer` broadcasts to clients     | 8b. Main UI reads `m_devices`          |

**Result:**

* Hot path: lock-free, real-time.
* Cold path: decoupled, low-frequency UI-safe updates.

### **3.2. Command Egress (UI → Device)**

1. UI pushes JSON into `m_command_queue`.
2. **RunCommandBridge** dequeues.
3. Sends 3-part ZMQ message to `m_cmd_router_socket`.
4. Target adapter's **Run** thread receives via DEALER socket.
5. Adapter dispatches to device via `HandleCommand()`.

---

## **4. 🔀 Protocol Adapter Architecture**

### **4.1. Type A – Asynchronous Polling (Modbus, OPC-UA)**

* No thread per device.
* Uses `steady_timer` → posts `DoBlockingPoll` to Asio pool.
* Completion returns to strand (`OnPollComplete`).
* Publishes data via ZMQ.

### **4.2. Type B – Event-Driven (MQTT, ZMQ)**

* **Thread-per-device** model.
* Worker loop blocks waiting for data.
* Publishes directly to ZMQ ingress.

---

## **5. 🛡️ Robustness & Error Handling**

### **5.1. Reaper Pattern (Asynchronous Deletion)**

Prevents UI stalls during removal of blocking worker threads.

* `RemoveDevice` sets `should_stop = true`.
* Worker moved into `m_reaper_queue`.
* Returns immediately.
* Reaper thread joins workers safely in background.

### **5.2. Logs & Performance**

* Hot-path log messages are filtered **before** allocation.
* Log vector pruned at 10 000 entries.

---

## **6. 💻 User Interface (Dear ImGui)**

* **uWS Server Control**: Start/stop/port, disabled during active services.
* **Live Log**: Timestamped, filtered, capped.
* **Device Status**: Renders the decoupled `m_devices` map.
* **Adapter Management**: Add/remove services/devices with Reaper cleanup.
* **Send Test Command**: Inject raw JSON into the Command Path.
