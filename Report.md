# **Multiprotocol Gateway Hub (V3): Architectural Analysis & Data Flow Report**

---

## **1. Executive Summary**

This report details the software architecture and data flow of the Multiprotocol Gateway Hub (V3). The system is a high-performance, multi-threaded C++ application designed to aggregate data from industrial protocols (**Modbus, OPC-UA, MQTT, ZMQ**) and serve it in real-time to a local **Dear ImGui GUI** and remote **web clients**.

The V3 architecture is a sophisticated **Hybrid Asynchronous Model** built on **Boost.Asio** and **ZeroMQ (ZMQ)**. Its defining feature is a fully *decoupled data pipeline* that uses a **central ZMQ PUB/SUB proxy**. This design removes mutex contention and eliminates the single-thread bottlenecks of previous versions. The system separates into three independent data-flow paths:

* **"Hot Path" (Web Data):** Lock-free, high-throughput real-time data delivery to web clients
* **"Cold Path" (GUI Data):** Low-priority, throttled updates for the local GUI
* **"Command Path" (Control):** Isolated command routing from UI to devices

V3 also uses a hybrid adapter model, combining:

* A **Boost.Asio task pool** for polling protocols
* A **thread-per-device model** (with an asynchronous "Reaper") for event-driven protocols

---

## **2. Core Architectural Components & Threading Model**

The application's scalability comes from dividing work across a dynamic collection of threads, each responsible for a single task.

---

### **2.1. Main Application Thread**

* **Thread Count:** 1
* **Role:** Runs `glfwPollEvents()` and renders the full Dear ImGui UI (`DrawGatewayUI`).

  * Acts as the *view layer*
  * Handles all user interactions

---

### **2.2. Asio Worker Pool**

* **Threads:** *N* (defaults to `hardware_concurrency()`)
* **Role:**

  * Executes asynchronous polling-based tasks (Modbus, OPC-UA)
  * Manages blocking I/O via `boost::asio::io_context`
  * Enables thousands of devices handled by few threads

---

### **2.3. Core Hub Threads (The “V3” Decoupled System)**

These threads form the backbone of the V3 architecture.

#### **• RunDataProxy (1 Thread)**

* Acts as the **heart** of the data plane
* Runs a high-performance, in-memory **ZMQ proxy**
* PULLs from `inproc://data_ingress`
* PUBs to `inproc://data_pubsub`
* Zero parsing, zero locks

#### **• RunUwsSubscriber (1 Thread — Hot Path)**

* High-priority web data path
* SUBscribes to `data_pubsub`
* Pushes messages to `m_data_to_publish`
* Wakes `RunUwsServer`

#### **• RunAggregator (1 Thread — Cold Path)**

* GUI data path
* SUBscribes to `data_pubsub`
* Runs on a 100ms timer
* Updates the central `m_devices` map in a single locked operation

#### **• RunCommandBridge (1 Thread — Command Path)**

* Simplified V2 ZmqBridge
* Polls `m_command_queue`
* Routes commands using a ZMQ ROUTER

#### **• RunUwsServer (1 Thread — Dynamic)**

* The WebSocket server
* On wake, publishes queued data to all web clients

---

### **2.4. Dynamic Adapter Threads**

#### **• IProtocolAdapter::Run (1 Thread per Adapter Service)**

* Listens for commands via a DEALER socket

#### **• Event-Driven Worker Threads (1 per MQTT/ZMQ Device)**

* Blocking loop threads (e.g., MQTT message waiting)
* Exit cleanly when stopped

---

## **3. Data Flow Analysis (The “V3” Model)**

The data flow is fully segregated into independent asynchronous pipelines.

---

## **3.1. Data Ingress (Device → UI) — Hot & Cold Paths**

The most important feature of V3: data is split at the earliest possible point into two priority paths.

### **Ingress Sequence**

1. **Poll/Receive:** Worker thread fetches device data
2. **Format:** Worker serializes data to Hub JSON standard
3. **ZMQ PUSH:** Sent to `inproc://data_ingress`
4. **ZMQ Proxy:** RunDataProxy re-broadcasts it to `data_pubsub`

### **Path A — Hot Path (Web UI)**

1. RunUwsSubscriber receives JSON
2. Pushes into `m_publish_queue`
3. Wakes the uWS loop
4. RunUwsServer flushes queue to all clients
5. **Result:** Ultra-low-latency web updates

### **Path B — Cold Path (Local GUI)**

1. RunAggregator receives JSON
2. Buffers until 100ms timer
3. Updates `m_devices` once per cycle
4. ImGui thread later renders the data
5. **Result:** Smooth & non-blocking GUI updates

---

## **3.2. Data Egress (UI → Device)**

The command path is fully isolated.

1. UI sends JSON command
2. Pushed into `m_command_queue`
3. RunCommandBridge dequeues and routes using ROUTER socket
4. Adapter thread receives via DEALER socket
5. Adapter finds target device
6. Issues protocol-specific command

---

## **4. Data Standards and Device Recognition**

V3 enforces strict separation of concerns via a unified JSON schema.

---

### **4.1. Hub Data Standard (JSON)**

```json
{
  "deviceId": "string",
  "adapterName": "string",
  "protocol": "string",
  "timestamp": 1678886400,
  "values": {
    "key1": "value1",
    "key2": 123.45,
    "key3": true
  }
}
```

---

### **4.2. Device Naming and Recognition**

#### **Hub is “Dumb”**

* Only indexes incoming data using `deviceId`
* Does *not* create identifiers

#### **Adapter is “Smart”**

* Responsible for:

  * Naming devices
  * Parsing protocol data
  * Creating valid JSON

#### **Examples**

**Modbus Worker:**

```
deviceId = workerName + ":reg_0_to_4"
```

**MQTT Worker:**

```
deviceId = adapterName + ":" + topicString
```

This enables thousands of unique data sources, even from a single adapter.

---

## **5. Web UI Structure and Flow (index.html)**

The Web UI is a minimal diagnostic interface for testing the Hub.

---

### **5.1. Structure**

* Read-only `#dataLog` textarea for incoming data
* Editable `#commandInput` textarea for outgoing commands
* One send button

---

### **5.2. Connection Management**

* Button disabled on load
* `ws.onopen` → enable button
* `ws.onclose` → disable button

---

### **5.3. Data Ingress (Web → Browser)**

1. uWS server publishes to:

   ```
   data/all
   ```
2. Client automatically SUBscribes
3. `onmessage` appends JSON to `dataLog`

---

### **5.4. Data Egress (Browser → Hub)**

Commands must follow this format:

```json
{
  "targetAdapter": "MqttService1",
  "targetDevice": "MyBroker",
  "command": "set_led",
  "state": "on"
}
```

---

## **🧱 Final Note — Longevity of Industrial Protocols**

These industrial technologies remain highly relevant:

* **EtherNet/IP** — strong and evolving
* **PROFINET** — strong future, TSN-enabled
* **EtherCAT** — long-term growth (EtherCAT-G emerging)
* **CAN / CANopen** — ubiquitous
* **UAVCAN** — rapidly growing in robotics/autonomy
* **BACnet** — permanent in building automation
* **RS-485** — still extremely common
* **libserialport / SOEM / OpENer** — highly relevant libraries
