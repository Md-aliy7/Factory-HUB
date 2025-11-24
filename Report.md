# Multiprotocol Gateway Hub (V3)

## Architectural Analysis & Data Flow Report (Sparkplug B Edition)

### 1\. Executive Summary

This report details the software architecture of the **Multiprotocol Gateway Hub (V3)**. The system is a high-performance, multi-threaded C++ application designed to aggregate data from industrial protocols (Modbus, OPC-UA, MQTT, ZMQ) and function as a **Sparkplug B Edge Node**.

V3 utilizes a **Centralized Broker Architecture**. The Hub acts strictly as an **Edge Node**, maintaining a single outbound TCP/SSL connection to a central MQTT Broker. This design ensures security by requiring no inbound ports.

**Key Architectural Changes in this Version:**

  * **Protocol:** Transitioned from generic JSON to **Sparkplug B (Protobuf)** for bandwidth efficiency and state management.
  * **State Management:** Implements `NBIRTH`, `NDEATH` (Last Will), and `NDATA` lifecycles.
  * **Data Paths:**
      * **Hot Path (Uplink):** ZMQ Stream $\to$ Sparkplug B Protobuf Encoder $\to$ MQTT.
      * **Cold Path (Local UI):** Throttled ZMQ Subscriber $\to$ Dear ImGui Dashboard.
      * **Command Path (RPC):** Sparkplug `DCMD`/`NCMD` $\to$ ZMQ Router $\to$ Device Adapter.

-----

### 2\. Core Architectural Components & Threading Model

The application uses a **Hybrid Threading Model**: `Boost.Asio` handles I/O polling for adapters, while `ZeroMQ (ZMQ)` manages lock-free internal data distribution.

#### 2.1. Main Application Thread

  * **Count:** 1
  * **Role:** Runs the `Dear ImGui` render loop (`DrawGatewayUI`) and handles `glfwPollEvents`. Acts as the **Local View Layer**.

#### 2.2. Asio Worker Pool

  * **Threads:** Configurable (defaults to hardware concurrency).
  * **Role:** Executes asynchronous tasks (`poll_timer`) for polling adapters (Modbus, OPC-UA).

#### 2.3. Core Hub Threads (The V3 Pipeline)

**A. RunDataProxy — 1 Thread**

  * **Role:** The central data bus.
  * **Mechanism:** ZMQ `PULL` (ingress) $\to$ `PUB` (internal broadcast).
  * **Address:** `inproc://data_ingress` $\to$ `inproc://data_pubsub`.

**B. RunCloudLink — 1 Thread (Sparkplug B Uplink)**

  * **Technology:** Paho MQTT C Client + Google Protobuf.
  * **Responsibilities:**
    1.  Maintains Session State (`NBIRTH` on connect, `NDEATH` LWT).
    2.  Subscribes to Command Topics: `spBv1.0/<Group>/DCMD/<NodeID>/+`.
    3.  Consumes internal ZMQ JSON, **encodes to Protobuf**, and publishes to `NDATA`.

**C. RunAggregator — 1 Thread (Cold Path)**

  * **Role:** Feeds the Local ImGui Dashboard.
  * **Mechanism:** ZMQ `SUB` to `data_pubsub` with a throttled timer (100ms).
  * **Purpose:** Updates the `m_devices` map for local visualization without blocking the high-speed uplink.

**D. RunCommandBridge — 1 Thread (Router)**

  * **Role:** Internal RPC dispatcher.
  * **Mechanism:** ZMQ `ROUTER` socket bound to `inproc://command_stream`.
  * **Flow:** Receives commands from CloudLink (or UI), routes them to the specific Adapter thread using the Adapter Name as the routing ID.

-----

### 3\. Data Flow Analysis

The V3 architecture uses a **"JSON Internally, Protobuf Externally"** strategy.

#### 3.1. Data Ingress (Device $\to$ Cloud)

**Step 1: Internal Normalization (JSON)**
Adapters (Modbus/OPC-UA) generate a "Wrapped" JSON packet pushed to ZMQ:

```json
{
  "deviceId": "Pump_A",
  "adapterName": "Modbus_Service",
  "payload": { "rpm": 1200, "temp": 45 }
}
```

**Step 2: Sparkplug Encoding (Protobuf)**
The `RunCloudLink` thread captures this JSON, creates a `SparkplugB::Payload`, and maps fields:

  * **Metric A:** `Meta/DeviceID` = "Pump\_A"
  * **Metric B:** `Data/Payload` =Stringified JSON (`"{ 'rpm': 1200... }"`).

**Step 3: Uplink Transmission**

  * **Topic:** `spBv1.0/<GroupID>/NDATA/<NodeID>`
  * **Payload:** Binary Protobuf.

#### 3.2. Data Egress (Cloud $\to$ Device)

**Step 1: Cloud Command**
The Cloud publishes a Sparkplug `DCMD` message:

  * **Topic:** `spBv1.0/<GroupID>/DCMD/<NodeID>/<DeviceID>`
  * **Payload:** Protobuf Metric containing the command string.

**Step 2: Hub Decoding**
`RunCloudLink` intercepts the message via `MessageArrived` callback:

1.  Parses Topic to extract Target Device ID.
2.  Parses Protobuf to extract the Command String.

**Step 3: Internal Routing**
The command is wrapped in an internal JSON envelope and pushed to the `CommandQueue`. The `RunCommandBridge` routes it to the correct Adapter (e.g., ModbusTCP) which executes the write operation.

-----

### 4\. Remote Web UI / SCADA Structure

Unlike the Local ImGui Dashboard, remote visualization now acts as a **Sparkplug B Host Application**.

#### 4.1. Connection Topology

  * **Protocol:** MQTT (TCP/WSS).
  * **Role:** Acts as the "Primary Host" (or SCADA).

#### 4.2. Data Consumption

Instead of reading raw JSON, the Remote UI must:

1.  Subscribe to `spBv1.0/#`.
2.  Decode `NBIRTH` to discover devices.
3.  Decode `NDATA` Protobufs to update live values.

#### 4.3. Command Issuance

To control a device, the Remote UI does not send raw JSON to a generic topic. It must publish a strictly formatted **Sparkplug DCMD** message to the specific device topic defined in Section 3.2.

-----

### 5\. Supported Protocols (Current Implementation)

The following protocols are currently implemented in the V3 codebase:

1.  **Modbus TCP:** Polling-based via `libmodbus`.
2.  **OPC-UA:** Polling-based via `open62541` (supports NodeID read/write).
3.  **MQTT (Device Side):** Event-based ingress for bridging 3rd party MQTT devices.
4.  **ZeroMQ (Device Side):** Supports `SUB`, `PULL`, `REP`, and `ROUTER` patterns for high-speed IPC.
