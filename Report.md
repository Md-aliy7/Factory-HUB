# Multiprotocol Gateway Hub (V3)

## Architectural Analysis & Data Flow Report

---

## 1. Executive Summary

This report details the software architecture and data flow of the **Multiprotocol Gateway Hub (V3)**.
The system is a high-performance, multi-threaded C++ application designed to aggregate data from industrial protocols (**Modbus, OPC-UA, MQTT, ZMQ**) and bridge the gap between the physical edge and the cloud.

V3 transitions from a P2P server model to a **Centralized Broker Architecture (Hub-Broker-Client)**.
In this *Middleman Topology*, the Hub acts exclusively as an **edge client**, connecting outbound via TCP to a central **MQTT Broker**.
This design:

* Removes inbound port requirements on the device
* Solves NAT/Firewall traversal issues
* Centralizes authentication and security

Three distinct data paths define runtime behavior:

* **Hot Path (Data Uplink):** High-throughput telemetry → Central Broker
* **Cold Path (GUI Data):** Throttled local visualization updates
* **Command Path (RPC):** Remote command execution using a “Target + Strip” routing protocol

---

## 2. Core Architectural Components & Threading Model

Work is divided across a dynamic thread set, using **Boost.Asio** for event polling and **ZeroMQ (ZMQ)** for lock-free internal message passing.

### 2.1. Main Application Thread

* **Count:** 1
* **Role:**

  * Runs `glfwPollEvents()`
  * Renders the local dashboard (`DrawGatewayUI`)
  * Acts as the local view layer

### 2.2. Asio Worker Pool

* **Threads:** *N* (defaults to `hardware_concurrency()`)
* **Role:**
  Executes async tasks for polling-based adapters (Modbus, OPC-UA), allowing a small pool to manage thousands of I/O operations.

### 2.3. Core Hub Threads (Decoupled Runtime System)

---

#### **RunDataProxy — 1 Thread**

* **Role:** Internal high-speed data plane heart
* **Mechanism:** In-memory **ZMQ Proxy**
* **Flow:**
  `PULL (inproc://data_ingress)` → `PUB (inproc://data_pubsub)`

---

#### **RunCloudLink — 1 Thread (Uplink)**

* **Technology:** Paho MQTT C++ Client
* **Responsibilities:**

  * Connects to Central Broker (TCP 1883/8883)
  * Subscribes to internal `data_pubsub`
  * Publishes telemetry → `v1/hubs/<HUB_ID>/telemetry`
  * Subscribes to cloud RPC → `v1/hubs/<HUB_ID>/rpc`

---

#### **RunAggregator — 1 Thread (Cold Path)**

* **Role:** Feeds the GUI
* **Mechanism:**
  SUBscribe to `data_pubsub` with a throttled timer (e.g., 100 ms) to update `m_devices`
* **Purpose:**
  Does **not** interfere with high-throughput paths

---

#### **RunCommandBridge — 1 Thread (Command Router)**

* **Role:** Internal command dispatcher
* **Mechanism:**
  Reads from `m_command_queue` (fed by CloudLink or UI)
  → Uses ZMQ ROUTER socket to deliver to target Adapter

---

## 3. Data Flow Analysis

### The **Wrap vs. Strip** Protocol

The architecture strictly separates:

* **Routing Logic** (Hub-level)
* **Business Logic** (Device-level)

---

### 3.1. Data Ingress (Device → WebUI)

### **“Wrap and Tag”**

Raw device output must be transported with identity metadata.

1. **Device Output:**

   ```json
   { "temp": 25 }
   ```

2. **Hub Wraps (Metadata Envelope):**

   ```json
   {
     "deviceId": "Sensor_01",
     "timestamp": 1715000000,
     "payload": { "temp": 25 }
   }
   ```

3. **Cloud Uplink:**
   Published to: `v1/hubs/<HUB_ID>/telemetry`

4. **WebUI:**

   * Reads `deviceId` to route
   * Displays `payload.temp`

---

### 3.2. Data Egress (WebUI → Device)

### **“Target and Strip”**

Commands include routing data, which the Hub removes before reaching the device.

1. **WebUI Sends Flat Command:**

   ```json
   {
     "deviceID": "Sensor_01",
     "reset": true
   }
   ```

2. **Broker:**
   Publishes to `v1/hubs/<HUB_ID>/rpc`

3. **Hub (CloudLink → CommandBridge):**
   Reads `deviceID` → dispatches to correct Adapter

4. **Strip Stage:**
   Device receives only clean payload:

   ```json
   { "reset": true }
   ```

---

## 4. Web UI Structure & Flow

The Web UI acts as a **parallel client** to the Hub, connecting directly to the Central Broker via WebSockets.

### 4.1. Connection Topology

* **Protocol:** MQTT over WebSockets (WSS)
* **Ports:** 8083 (standard) / 443 (SSL)
* **Auth:** OAuth/Token-based with Broker (never with Hub)

### 4.2. Data Ingress (Telemetry)

* **Subscription:** `v1/hubs/+/telemetry`
* **Flow:**

  1. Receive wrapped JSON
  2. Parse `deviceId`
  3. Display `payload`

### 4.3. Data Egress (Commands)

* **Publish to:** `v1/hubs/<TARGET_HUB>/rpc`
* **UI Logic:**

  ```json
  { "deviceID": "PLC_1", "val": 1 }
  ```

---

## 🧱 Final Note — Longevity of Industrial Protocols

These industrial technologies remain highly relevant and are supported by the Gateway’s hybrid adapter approach:

* **EtherNet/IP** — established & evolving
* **PROFINET** — strong future, TSN-enabled
* **EtherCAT** — continued growth (EtherCAT-G)
* **CAN / CANopen** — foundational in embedded systems
* **UAVCAN** — expanding rapidly in robotics/autonomy
* **BACnet** — cornerstone of building automation
* **RS-485** — still critical for legacy integration
