/*
 * Multiprotocol Gateway Hub (V3 - ZMQ Proxy Architecture)
 *
 * Architecture (Decoupled PUB/SUB):
 * 1. Main Thread: Runs Dear ImGui loop.
 * 2. GatewayHub Class: Central manager. Owns Asio io_context.
 * 3. Asio Thread Pool: A fixed-size thread pool executes async work
 * for polling adapters (Modbus, OPC-UA).
 *
 * 4. Core Background Threads (DECOUPLED):
 *
 * - "Hot Path" (Data to Web):
 * - RunDataProxy: New thread. Runs a fast ZMQ PULL->PUB proxy.
 * (PULL "inproc://data_ingress", PUB "inproc://data_pubsub")
 * - RunUwsSubscriber: New thread. SUBscribes to "data_pubsub",
 * logs Ingress, and pushes data to the uWS queue.
 * - RunUwsServer: Runs the uWS::Loop, publishing data from the queue.
 *
 * - "Cold Path" (Data to UI):
 * - RunAggregator: New thread. SUBscribes to "data_pubsub" on a
 * slow timer (500ms) to update the ImGui device map.
 *
 * - "Command Path" (Web to Device):
 * - RunCommandBridge: (Old ZmqBridge) Now *only* handles commands
 * from the UI and adapter heartbeats.
 *
 * 5. Dynamic Adapters (IProtocolAdapter):
 * - Polling (Modbus, OPC-UA): Use Asio timers.
 * - Event-Based (MQTT, ZMQ): Use "thread-per-device" with Reaper.
 * - All adapters PUSH data to "inproc://data_ingress".
 */

 // Define this globally to suppress security warnings for standard C functions (localtime, getenv, etc.) on MSVC.
#define _CRT_SECURE_NO_WARNINGS

// --- Temporarily disable CRT secure warnings ---
#ifdef _WIN32
#pragma warning(disable: 4996) // Disable specific deprecation warning number
#endif

// --- Standard C++ Libraries ---
#include <iostream>
#include <string>
#include <thread>
#include <set>
#include <queue>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <memory>
#include <atomic>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cstddef>
#include <chrono>
#include <map>
#include <deque>
#include <nlohmann/json.hpp>

// --- C/C++ Libraries for Adapters ---
#include <zmq.hpp> // ZeroMQ C++ Wrapper
#include "MQTTClient.h" // Paho MQTT C Client
#include "modbus/modbus.h"
#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/client_highlevel.h>
#include <open62541/client_subscriptions.h>

// --- Dear ImGui Includes ---
#include <GL/glew.h>
#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#ifdef _WIN32
#include <GLFW/glfw3native.h>
#endif

// --- uWebSockets ---
#include <uwebsockets/App.h>
#include <uwebsockets/WebSocket.h>

// --- Boost.Asio ---
// (We only need the main header for post, strands, and timers)
#include <boost/asio.hpp>

// --- Helper Types ---
using DeviceConfig = std::map<std::string, std::string>;

// --- DeviceData struct ---
struct DeviceData {
    std::string id;
    std::string protocol;
    std::string adapter_name;
    std::string last_value_json; // Store values as string
    time_t last_seen;
    int message_count = 0;
};

// --- Static global variables ---
static unsigned int s_hardware_cores = (std::thread::hardware_concurrency() > 0) ? std::thread::hardware_concurrency() : 4;
static int s_worker_pool_size = (int)s_hardware_cores; // Default to the number of logical cores
static int s_ws_port = 9001; // Default WebSocket port
static std::string s_ws_host = "0.0.0.0"; // Default host (all interfaces)

// --- FORWARD DECLARATIONS (for circular dependency fix) ---
class ModbusTCPAdapter;
class OpcuaAdapter;
class GatewayHub; // Forward declare GatewayHub for global logger

// --- Global Logging & Notification System ---
static std::mutex g_log_mutex;
static std::vector<std::string> g_logs;
static bool g_log_show_ingress = true; // Show (Device -> WebUI)
static bool g_log_show_egress = true;  // Show (WebUI -> Device)
static const size_t g_log_max_lines = 10000; // Max log lines
static const int g_zmq_rcvhwm = 10000;        // Max queued ZMQ messages

struct Notification {
    std::string message;
    bool is_success;
    double expiry_time;
};
static std::vector<Notification> g_notifications;
static std::mutex g_notification_mutex;

enum class LogType {
    SYSTEM,  // Default, always show
    INGRESS, // Device -> WebUI
    EGRESS   // WebUI -> Device
};

void AddLog(const std::string& msg, LogType type = LogType::SYSTEM) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (type == LogType::INGRESS && !g_log_show_ingress) return;
    if (type == LogType::EGRESS && !g_log_show_egress) return;
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf; // Structure to hold the time
#ifdef _WIN32
    // Use the thread-safe version for Windows (MSVC)
    localtime_s(&tm_buf, &in_time_t);
#else
    // Use the thread-safe version for POSIX (Linux, macOS)
    localtime_r(&in_time_t, &tm_buf);
#endif
    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%H:%M:%S");
    g_logs.push_back("[" + ss.str() + "] " + msg);    if (g_logs.size() > g_log_max_lines) {
        size_t erase_count = g_logs.size() - g_log_max_lines + (g_log_max_lines / 10);
        g_logs.erase(g_logs.begin(), g_logs.begin() + erase_count);
    }
}

void PushNotification(const std::string& message, bool is_success) {
    std::lock_guard<std::mutex> lock(g_notification_mutex);
    g_notifications.push_back({ message, is_success, ImGui::GetTime() + 5.0 });
}


// --- IProtocolAdapter Interface ---
class IProtocolAdapter {
public:
    IProtocolAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx)
        : m_name(name),
        m_zmq_context(ctx),
        m_io_context(io_ctx),
        m_data_push_socket(ctx, zmq::socket_type::push),
        m_cmd_socket(ctx, zmq::socket_type::dealer),
        m_should_stop(false)
    {
        try {
            m_data_push_socket.connect("inproc://data_ingress");
            m_cmd_socket.set(zmq::sockopt::routing_id, m_name);
            m_cmd_socket.connect("inproc://command_stream");
            m_cmd_socket.set(zmq::sockopt::rcvhwm, g_zmq_rcvhwm);  // Limit queue to 10000 messages
        }
        catch (const zmq::error_t& e) {
            AddLog("Adapter connect error: " + std::string(e.what()));
        }
    }

    virtual ~IProtocolAdapter() {
        Stop();
        Join();
        m_data_push_socket.close();
        m_cmd_socket.close();
    }

    void Start() {
        m_thread = std::thread(&IProtocolAdapter::Run, this);
    }

    void Stop() {
        m_should_stop = true;
    }

    void Join() {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    virtual void Run() {
        try {
            m_cmd_socket.set(zmq::sockopt::rcvtimeo, 1000); // 1s timeout
            while (!m_should_stop) {
                zmq::message_t empty_msg;
                zmq::message_t payload_msg;
                std::optional<size_t> empty_size = m_cmd_socket.recv(empty_msg, zmq::recv_flags::none);
                if (!empty_size.has_value()) {
                    continue; // Timeout
                }
                if (!m_cmd_socket.get(zmq::sockopt::rcvmore)) {
                    if (g_log_show_egress) AddLog("Adapter " + m_name + " received bad ZMQ command (missing payload frame).", LogType::EGRESS);
                    continue;
                }
                std::optional<size_t> payload_size = m_cmd_socket.recv(payload_msg, zmq::recv_flags::none);
                if (payload_size.has_value() && payload_size.value() > 0) {
                    nlohmann::json cmd = nlohmann::json::parse(payload_msg.to_string());
                    std::string device_name = cmd.value("targetDevice", "");
                    if (!device_name.empty()) {
                        HandleCommand(device_name, cmd);
                    }
                    else {
                        if (g_log_show_egress) AddLog("Adapter " + m_name + " received command with no targetDevice.", LogType::EGRESS);
                    }
                }
            }
        }
        catch (const std::exception& e) {
            if (m_should_stop) return;
            AddLog("Adapter " + m_name + " Run() error: " + std::string(e.what()));
        }
    }

    virtual bool AddDevice(const std::string& device_name, const DeviceConfig& config) = 0;
    virtual bool RemoveDevice(const std::string& device_name) = 0;
    virtual bool RestartDevice(const std::string& device_name) = 0;
    virtual bool IsDeviceActive(const std::string& device_name) = 0;
    virtual std::map<std::string, std::string> GetDeviceStatuses() = 0;
    virtual std::string GetName() const { return m_name; }
    virtual std::string GetProtocol() = 0;
    virtual std::string GetStatus() { return "Running"; }

protected:
    virtual void HandleCommand(const std::string& device_name, const nlohmann::json& cmd) = 0;
    void PushData(const std::string& json_payload) {
        std::lock_guard<std::mutex> lock(m_zmq_push_mutex);
        m_data_push_socket.send(zmq::buffer(json_payload), zmq::send_flags::none);
    }
    std::string m_name;
    zmq::context_t& m_zmq_context;
    boost::asio::io_context& m_io_context;
    zmq::socket_t m_data_push_socket;
    zmq::socket_t m_cmd_socket;
    std::mutex m_zmq_push_mutex;
    std::thread m_thread;
    std::atomic<bool> m_should_stop;
};


// --- ModbusTCPAdapter ---

class ModbusDeviceWorker : public std::enable_shared_from_this<ModbusDeviceWorker> {
public:
    std::string name;
    std::string ip;
    int port;
    std::atomic<bool> is_connected;
    modbus_t* ctx;
    std::string status;
    std::atomic<bool> m_stopped;
    boost::asio::io_context& m_io_context;
    boost::asio::strand<boost::asio::io_context::executor_type> m_strand;
    boost::asio::steady_timer m_poll_timer;
    IProtocolAdapter* m_adapter;

    ModbusDeviceWorker(std::string n, std::string i, int p, boost::asio::io_context& io_ctx, IProtocolAdapter* adapter)
        : name(n), ip(i), port(p), is_connected(false), ctx(nullptr), status("Idle"), m_stopped(false),
        m_io_context(io_ctx),
        m_strand(boost::asio::make_strand(io_ctx)),
        m_poll_timer(io_ctx),
        m_adapter(adapter)
    {
    }

    ~ModbusDeviceWorker() {
        if (ctx) {
            modbus_close(ctx);
            modbus_free(ctx);
        }
    }

    void StartPoll() {
        boost::asio::post(m_strand,
            std::bind(&ModbusDeviceWorker::OnTimer, shared_from_this(), boost::system::error_code()));
    }

    void StopPoll() {
        boost::asio::post(m_strand, [this, self = shared_from_this()]() {
            m_stopped = true;
            m_poll_timer.cancel();
            });
    }

private:
    void OnTimer(const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted || m_stopped) {
            status = "Stopped";
            return;
        }
        boost::asio::post(m_io_context,
            std::bind(&ModbusDeviceWorker::DoBlockingPoll, shared_from_this()));
    }

    void DoBlockingPoll() {
        if (m_stopped) return;
        nlohmann::json j_values;
        std::string poll_status = "Error: Poll Failed";
        bool connection_ok = true;
        try {
            if (!is_connected) {
                if (ctx) { modbus_close(ctx); modbus_free(ctx); }
                ctx = modbus_new_tcp(ip.c_str(), port);
                if (!ctx) {
                    poll_status = "Error: Failed context";
                    connection_ok = false;
                }
                else {
                    modbus_set_response_timeout(ctx, 1, 0); // 1s
                    if (modbus_connect(ctx) == -1) {
                        poll_status = "Error: Connection Failed";
                        modbus_free(ctx);
                        ctx = nullptr;
                        connection_ok = false;
                    }
                    else {
                        is_connected = true;
                    }
                }
            }
            if (connection_ok) {
                uint16_t regs[10];
                int rc = modbus_read_registers(ctx, 0, 10, regs);
                if (rc == -1) {
                    poll_status = "Error: Read Failed";
                    is_connected = false;
                }
                else {
                    poll_status = "Running";
                    for (int i = 0; i < 10; ++i) {
                        j_values["reg_" + std::to_string(i)] = regs[i];
                    }
                }
            }
        }
        catch (const std::exception& e) {
            poll_status = std::string("Error: ") + e.what();
            is_connected = false;
            if (ctx) { modbus_free(ctx); ctx = nullptr; }
        }
        boost::asio::post(m_strand,
            std::bind(&ModbusDeviceWorker::OnPollComplete, shared_from_this(), poll_status, j_values));
    }
    void OnPollComplete(const std::string& poll_status, const nlohmann::json& j_values);
};

class ModbusTCPAdapter : public IProtocolAdapter {
public:
    ModbusTCPAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx)
        : IProtocolAdapter(name, ctx, io_ctx) {
    }
    ~ModbusTCPAdapter() {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        for (auto& [name, worker] : m_devices) {
            worker->StopPoll();
        }
        m_devices.clear();
    }
    std::string GetProtocol() override { return "ModbusTCP"; }
    std::map<std::string, std::string> GetDeviceStatuses() override {
        std::map<std::string, std::string> statuses;
        std::lock_guard<std::mutex> lock(m_device_mutex);
        for (const auto& [name, worker] : m_devices) {
            statuses[name] = worker->status;
        }
        return statuses;
    }
    bool AddDevice(const std::string& device_name, const DeviceConfig& config) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (m_devices.count(device_name)) return false;
        try {
            std::string ip = config.at("ip");
            int port = std::stoi(config.at("port"));
            auto worker = std::make_shared<ModbusDeviceWorker>(device_name, ip, port, m_io_context, this);
            m_devices[device_name] = worker;
            worker->StartPoll();
            AddLog("ModbusTCP: Added device " + device_name);
            return true;
        }
        catch (const std::exception& e) {
            AddLog("Modbus AddDevice error: " + std::string(e.what()));
            return false;
        }
    }
    bool RemoveDevice(const std::string& device_name) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (!m_devices.count(device_name)) return false;
        m_devices[device_name]->StopPoll();
        m_devices.erase(device_name);
        AddLog("ModbusTCP: Removed device " + device_name);
        return true;
    }
    bool RestartDevice(const std::string& device_name) override {
        DeviceConfig config;
        {
            std::lock_guard<std::mutex> lock(m_device_mutex);
            auto it = m_devices.find(device_name);
            if (it == m_devices.end()) return false;
            config["ip"] = it->second->ip;
            config["port"] = std::to_string(it->second->port);
        }
        RemoveDevice(device_name);
        return AddDevice(device_name, config);
    }
    bool IsDeviceActive(const std::string& device_name) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        return m_devices.count(device_name);
    }
    friend class ModbusDeviceWorker;
protected:
    void HandleCommand(const std::string& device_name, const nlohmann::json& cmd) override {
        if (g_log_show_egress) AddLog("Modbus HandleCommand: " + cmd.dump(), LogType::EGRESS);
    }
    std::map<std::string, std::shared_ptr<ModbusDeviceWorker>> m_devices;
    std::mutex m_device_mutex;
};

void ModbusDeviceWorker::OnPollComplete(const std::string& poll_status, const nlohmann::json& j_values) {
    if (m_stopped) return;
    status = poll_status;
    if (status == "Running") {
        nlohmann::json j;
        j["deviceId"] = name;
        j["adapterName"] = m_adapter->GetName();
        j["protocol"] = m_adapter->GetProtocol();
        j["timestamp"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        j["values"] = j_values;
        static_cast<ModbusTCPAdapter*>(m_adapter)->PushData(j.dump());
    }
    m_poll_timer.expires_after(std::chrono::seconds(1));
    m_poll_timer.async_wait(boost::asio::bind_executor(m_strand,
        std::bind(&ModbusDeviceWorker::OnTimer, shared_from_this(), std::placeholders::_1)));
}


// --- OpcuaAdapter ---

class OpcuaDeviceWorker : public std::enable_shared_from_this<OpcuaDeviceWorker> {
public:
    std::string name;
    std::string endpoint;
    std::vector<std::string> nodeIds;
    UA_Client* client;
    std::atomic<bool> is_connected;
    std::string status;
    std::atomic<bool> m_stopped;
    boost::asio::io_context& m_io_context;
    boost::asio::strand<boost::asio::io_context::executor_type> m_strand;
    boost::asio::steady_timer m_poll_timer;
    IProtocolAdapter* m_adapter;

    OpcuaDeviceWorker(std::string n, std::string ep, std::vector<std::string> nodes, boost::asio::io_context& io_ctx, IProtocolAdapter* adapter)
        : name(n), endpoint(ep), nodeIds(nodes), client(nullptr), is_connected(false), status("Idle"), m_stopped(false),
        m_io_context(io_ctx),
        m_strand(boost::asio::make_strand(io_ctx)),
        m_poll_timer(io_ctx),
        m_adapter(adapter)
    {
    }

    ~OpcuaDeviceWorker() {
        if (client) {
            UA_Client_disconnect(client);
            UA_Client_delete(client);
        }
    }

    void StartPoll() {
        boost::asio::post(m_strand,
            std::bind(&OpcuaDeviceWorker::OnTimer, shared_from_this(), boost::system::error_code()));
    }

    void StopPoll() {
        boost::asio::post(m_strand, [this, self = shared_from_this()]() {
            m_stopped = true;
            m_poll_timer.cancel();
            });
    }

private:
    void OnTimer(const boost::system::error_code& ec) {
        if (ec || m_stopped) {
            status = "Stopped";
            return;
        }
        boost::asio::post(m_io_context,
            std::bind(&OpcuaDeviceWorker::DoBlockingPoll, shared_from_this()));
    }

    void DoBlockingPoll() {
        if (m_stopped) return;
        nlohmann::json j_values;
        std::string poll_status = "Error: Poll Failed";
        bool connection_ok = true;
        try {
            if (!is_connected) {
                if (client) { UA_Client_disconnect(client); UA_Client_delete(client); }
                client = UA_Client_new();
                UA_ClientConfig* cc = UA_Client_getConfig(client);
                UA_ClientConfig_setDefault(cc);
                cc->timeout = 1000;
                UA_StatusCode retval = UA_Client_connect(client, endpoint.c_str());
                if (retval != UA_STATUSCODE_GOOD) {
                    poll_status = "Error: Connection Failed";
                    UA_Client_delete(client);
                    client = nullptr;
                    connection_ok = false;
                }
                else {
                    is_connected = true;
                }
            }
            if (connection_ok) {
                bool read_ok = true;
                for (const std::string& node_str : nodeIds) {
                    UA_NodeId nodeId = UA_NODEID_STRING_ALLOC(1, node_str.c_str());
                    UA_Variant value;
                    UA_StatusCode retval = UA_Client_readValueAttribute(client, nodeId, &value);
                    if (retval == UA_STATUSCODE_GOOD && UA_Variant_isScalar(&value)) {
                        if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_INT32])) j_values[node_str] = *(UA_Int32*)value.data;
                        else if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_DOUBLE])) j_values[node_str] = *(UA_Double*)value.data;
                        else if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_BOOLEAN])) j_values[node_str] = *(UA_Boolean*)value.data;
                        else j_values[node_str] = "Unsupported type";
                    }
                    else {
                        j_values[node_str] = "Read error";
                        read_ok = false;
                    }
                    UA_NodeId_clear(&nodeId);
                    UA_Variant_clear(&value);
                }
                if (!read_ok) {
                    poll_status = "Error: Read Failed";
                    is_connected = false;
                }
                else {
                    poll_status = "Running";
                }
            }
        }
        catch (const std::exception& e) {
            poll_status = std::string("Error: ") + e.what();
            is_connected = false;
            if (client) { UA_Client_delete(client); client = nullptr; }
        }
        boost::asio::post(m_strand,
            std::bind(&OpcuaDeviceWorker::OnPollComplete, shared_from_this(), poll_status, j_values));
    }
    void OnPollComplete(const std::string& poll_status, const nlohmann::json& j_values);
};

class OpcuaAdapter : public IProtocolAdapter {
public:
    OpcuaAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx)
        : IProtocolAdapter(name, ctx, io_ctx) {
    }
    ~OpcuaAdapter() {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        for (auto& [name, worker] : m_devices) {
            worker->StopPoll();
        }
        m_devices.clear();
    }
    std::string GetProtocol() override { return "OPC-UA"; }
    std::map<std::string, std::string> GetDeviceStatuses() override {
        std::map<std::string, std::string> statuses;
        std::lock_guard<std::mutex> lock(m_device_mutex);
        for (const auto& [name, worker] : m_devices) {
            statuses[name] = worker->status;
        }
        return statuses;
    }
    bool AddDevice(const std::string& device_name, const DeviceConfig& config) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (m_devices.count(device_name)) return false;
        try {
            std::string endpoint = config.at("endpoint");
            std::string nodes_str = config.at("nodeIds");
            std::vector<std::string> nodeIds;
            std::stringstream ss(nodes_str);
            std::string node;
            while (std::getline(ss, node, ',')) nodeIds.push_back(node);
            if (nodeIds.empty()) return false;
            auto worker = std::make_shared<OpcuaDeviceWorker>(device_name, endpoint, nodeIds, m_io_context, this);
            m_devices[device_name] = worker;
            worker->StartPoll();
            AddLog("OPC-UA: Added device " + device_name);
            return true;
        }
        catch (const std::exception& e) {
            AddLog("OPC-UA AddDevice error: " + std::string(e.what()));
            return false;
        }
    }
    bool RemoveDevice(const std::string& device_name) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (!m_devices.count(device_name)) return false;
        m_devices[device_name]->StopPoll();
        m_devices.erase(device_name);
        AddLog("OPC-UA: Removed device " + device_name);
        return true;
    }
    bool RestartDevice(const std::string& device_name) override {
        DeviceConfig config;
        {
            std::lock_guard<std::mutex> lock(m_device_mutex);
            auto it = m_devices.find(device_name);
            if (it == m_devices.end()) return false;
            config["endpoint"] = it->second->endpoint;
            std::stringstream ss;
            for (size_t i = 0; i < it->second->nodeIds.size(); ++i) {
                ss << it->second->nodeIds[i];
                if (i < it->second->nodeIds.size() - 1) ss << ",";
            }
            config["nodeIds"] = ss.str();
        }
        RemoveDevice(device_name);
        return AddDevice(device_name, config);
    }
    bool IsDeviceActive(const std::string& device_name) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        return m_devices.count(device_name);
    }
    friend class OpcuaDeviceWorker;
protected:
    void HandleCommand(const std::string& device_name, const nlohmann::json& cmd) override {
        if (g_log_show_egress) AddLog("OPC-UA HandleCommand: " + cmd.dump(), LogType::EGRESS);
    }
    std::map<std::string, std::shared_ptr<OpcuaDeviceWorker>> m_devices;
    std::mutex m_device_mutex;
};

void OpcuaDeviceWorker::OnPollComplete(const std::string& poll_status, const nlohmann::json& j_values) {
    if (m_stopped) return;
    status = poll_status;
    if (status == "Running") {
        nlohmann::json j;
        j["deviceId"] = name;
        j["adapterName"] = m_adapter->GetName();
        j["protocol"] = m_adapter->GetProtocol();
        j["timestamp"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        j["values"] = j_values;
        static_cast<OpcuaAdapter*>(m_adapter)->PushData(j.dump());
    }
    m_poll_timer.expires_after(std::chrono::seconds(1));
    m_poll_timer.async_wait(boost::asio::bind_executor(m_strand,
        std::bind(&OpcuaDeviceWorker::OnTimer, shared_from_this(), std::placeholders::_1)));
}


// --- MqttAdapter (PATCHED) ---

struct MqttDeviceWorker {
    std::string name;
    std::string brokerUri;
    std::string clientId;
    std::string commandTopic;
    std::vector<std::string> subscribeTopics;
    MQTTClient client;
    std::thread thread;
    std::mutex client_mutex;
    std::atomic<bool> should_stop;
    std::string status;
    IProtocolAdapter* adapter;

    // --- Robust Connection Parameters ---
    std::string username;
    std::string password;

    bool useTls;
    std::string caFile;
    std::string certFile;
    std::string keyFile;

    bool cleanSession;
    int keepAlive;
    int connectTimeout;

    std::string lwtTopic;
    std::string lwtPayload;
    int lwtQos;
    bool lwtRetain;

    int subscribeQos;

    MqttDeviceWorker(std::string n, std::string uri, std::string cid, std::string cmd_topic, std::vector<std::string> sub_topics, IProtocolAdapter* parent)
        : name(n), brokerUri(uri), clientId(cid), commandTopic(cmd_topic), subscribeTopics(sub_topics),
        client(nullptr), should_stop(false), status("Idle"), adapter(parent),
        useTls(false),
        cleanSession(true),
        keepAlive(60),
        connectTimeout(5),
        lwtQos(1),
        lwtRetain(false),
        subscribeQos(1)
    {
    }
};

class MqttAdapter : public IProtocolAdapter {
public:
    MqttAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx)
        : IProtocolAdapter(name, ctx, io_ctx) {
        m_reaper_thread = std::thread(&MqttAdapter::ReaperLoop, this);
    }
    ~MqttAdapter() {
        {
            std::lock_guard<std::mutex> lock(m_device_mutex);
            for (auto& [name, worker] : m_devices) {
                worker->should_stop = true;
            }
        }
        if (m_reaper_thread.joinable()) {
            m_reaper_thread.join();
        }
        for (auto& [name, worker] : m_devices) {
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
        }
        m_devices.clear();
    }
    std::string GetProtocol() override { return "MQTT"; }
    std::map<std::string, std::string> GetDeviceStatuses() override {
        std::map<std::string, std::string> statuses;
        std::lock_guard<std::mutex> lock(m_device_mutex);
        for (const auto& [name, worker] : m_devices) {
            statuses[name] = worker->status;
        }
        return statuses;
    }
    bool AddDevice(const std::string& device_name, const DeviceConfig& config) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (m_devices.count(device_name)) return false;

        // --- Helper lambda to safely get optional config values ---
        auto getConfigOr = [&](const std::string& key, const std::string& defaultValue) -> std::string {
            auto it = config.find(key);
            if (it != config.end() && !it->second.empty()) {
                return it->second;
            }
            return defaultValue;
            };

        try {
            // --- Required ---
            std::string brokerUri = config.at("brokerUri");
            std::string sub_topics_str = config.at("subscribeTopics");

            // --- Optional ---
            std::string clientId = getConfigOr("clientId", device_name + "_client");
            std::string commandTopic = getConfigOr("commandTopic", "");

            std::vector<std::string> sub_topics;
            std::stringstream ss(sub_topics_str);
            std::string topic;
            while (std::getline(ss, topic, ',')) {
                if (!topic.empty()) sub_topics.push_back(topic);
            }
            if (sub_topics.empty()) return false;

            auto worker = std::make_unique<MqttDeviceWorker>(device_name, brokerUri, clientId, commandTopic, sub_topics, this);

            // --- Populate all worker fields from config ---
            worker->username = getConfigOr("username", "");
            worker->password = getConfigOr("password", "");

            worker->useTls = (getConfigOr("useTls", "false") == "true");
            worker->caFile = getConfigOr("caFile", "");
            worker->certFile = getConfigOr("certFile", "");
            worker->keyFile = getConfigOr("keyFile", "");

            worker->cleanSession = (getConfigOr("cleanSession", "true") == "true");
            worker->lwtRetain = (getConfigOr("lwtRetain", "false") == "true");

            try { worker->keepAlive = std::stoi(getConfigOr("keepAlive", "60")); }
            catch (...) { worker->keepAlive = 60; }
            try { worker->connectTimeout = std::stoi(getConfigOr("connectTimeout", "5")); }
            catch (...) { worker->connectTimeout = 5; }
            try { worker->subscribeQos = std::stoi(getConfigOr("subscribeQos", "1")); }
            catch (...) { worker->subscribeQos = 1; }
            try { worker->lwtQos = std::stoi(getConfigOr("lwtQos", "1")); }
            catch (...) { worker->lwtQos = 1; }

            worker->lwtTopic = getConfigOr("lwtTopic", "");
            worker->lwtPayload = getConfigOr("lwtPayload", "");
            // --- End of NEW Population ---

            worker->thread = std::thread(&MqttAdapter::DevicePollLoop, this, worker.get());
            m_devices[device_name] = std::move(worker);
            AddLog("MQTT: Added device " + device_name);
            return true;
        }
        catch (const std::exception& e) {
            AddLog("MQTT AddDevice error: " + std::string(e.what()));
            return false;
        }
    }
    bool RemoveDevice(const std::string& device_name) override {

        std::unique_ptr<MqttDeviceWorker> worker_to_destroy;

        // --- Step 1: Atomically move the worker out of the map ---
        {
            std::lock_guard<std::mutex> lock(m_device_mutex); // Lock adapter's map

            auto it = m_devices.find(device_name);
            if (it == m_devices.end()) return false; // Not found

            it->second->should_stop = true; // Signal thread to stop
            worker_to_destroy = std::move(it->second); // Move ownership
            m_devices.erase(it); // Remove from map
        }
        // --- m_device_mutex is now UNLOCKED ---

        // --- Step 2: Join the thread *outside* the lock ---
        if (worker_to_destroy) {
            // Now we wait for the thread to actually die.
            // This is safe and won't deadlock because no locks are held.
            if (worker_to_destroy->thread.joinable()) {
                worker_to_destroy->thread.join();
            }
        }
        // The worker's thread is now 100% dead.
        // worker_to_destroy's destructor runs, cleaning up the worker.

        return true;
    }

    bool RestartDevice(const std::string& device_name) override {
        DeviceConfig config;
        {
            std::lock_guard<std::mutex> lock(m_device_mutex);
            auto it = m_devices.find(device_name);
            if (it == m_devices.end()) return false;

            auto& worker = it->second;

            // --- Base settings ---
            config["brokerUri"] = worker->brokerUri;
            config["clientId"] = worker->clientId;
            config["commandTopic"] = worker->commandTopic;

            std::stringstream ss;
            for (size_t i = 0; i < worker->subscribeTopics.size(); ++i) {
                ss << worker->subscribeTopics[i];
                if (i < worker->subscribeTopics.size() - 1) ss << ",";
            }
            config["subscribeTopics"] = ss.str();

            // --- NEW: Add all new settings to config ---
            config["username"] = worker->username;
            config["password"] = worker->password; // Note: storing pass in map
            config["useTls"] = worker->useTls ? "true" : "false";
            config["caFile"] = worker->caFile;
            config["certFile"] = worker->certFile;
            config["keyFile"] = worker->keyFile;
            config["cleanSession"] = worker->cleanSession ? "true" : "false";
            config["keepAlive"] = std::to_string(worker->keepAlive);
            config["connectTimeout"] = std::to_string(worker->connectTimeout);
            config["subscribeQos"] = std::to_string(worker->subscribeQos);
            config["lwtTopic"] = worker->lwtTopic;
            config["lwtPayload"] = worker->lwtPayload;
            config["lwtQos"] = std::to_string(worker->lwtQos);
            config["lwtRetain"] = worker->lwtRetain ? "true" : "false";
            // --- End of NEW settings ---
        }
        RemoveDevice(device_name);
        return AddDevice(device_name, config);
    }

    bool IsDeviceActive(const std::string& device_name) override {
        // Lock this adapter's device map and check for the key
        std::lock_guard<std::mutex> lock(m_device_mutex);
        return m_devices.count(device_name);
    }

protected:
    void HandleCommand(const std::string& device_name, const nlohmann::json& cmd) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (m_devices.count(device_name)) {
            auto& worker = m_devices[device_name];
            std::string topic = worker->commandTopic;
            std::string payload = cmd.dump();
            if (!topic.empty() && worker->client) {
                if (g_log_show_egress) AddLog("MQTT: Publishing to " + topic + ": " + payload, LogType::EGRESS);
                MQTTClient_message pubmsg = MQTTClient_message_initializer;
                pubmsg.payload = (void*)payload.c_str();
                pubmsg.payloadlen = (int)payload.length();
                pubmsg.qos = 1;
                pubmsg.retained = 0;
                MQTTClient_publishMessage(worker->client, topic.c_str(), &pubmsg, NULL);
            }
        }
    }

    static int on_message_arrived(void* context, char* topicName, int topicLen, MQTTClient_message* message) {
        MqttDeviceWorker* worker = static_cast<MqttDeviceWorker*>(context);

        // Context should always be valid if Paho is working
        if (worker == nullptr) {
            // Abnormal case: free message to prevent leak
            MQTTClient_freeMessage(&message);
            MQTTClient_free(topicName);
            return 1;
        }

        // 1. Acquire the lock
        std::lock_guard<std::mutex> lock(worker->client_mutex);

        // 2. Check if the client is being destroyed *after* getting the lock.
        // If client is NULL, destroy() has been called.
        // That function will free all resources, including this message.
        // Freeing it here would be a double-free CRASH.
        if (worker->client == nullptr || worker->should_stop) {
            // Client is gone or stopping. DO NOT free the message.
            // Just return to acknowledge.
            return 1;
        }

        // Use topicLen! topicName is not guaranteed to be null-terminated.
        std::string topic(topicName, topicLen);
        // --- END BUG FIX ---

        std::string payload((char*)message->payload, message->payloadlen);
        worker->status = "Msg received on " + topic;

        nlohmann::json j;
        j["deviceId"] = worker->name; // DeviceId is static
        j["adapterName"] = worker->adapter->GetName();
        j["protocol"] = worker->adapter->GetProtocol();
        j["timestamp"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

        nlohmann::json parsed_payload;
        try { parsed_payload = nlohmann::json::parse(payload); }
        catch (...) { parsed_payload = payload; }

        j["values"][topic] = parsed_payload; // Topic is a key in values

        static_cast<MqttAdapter*>(worker->adapter)->PushData(j.dump());

        // We are still inside the lock and the client is valid,
        // so it is 100% safe to free the message here.
        MQTTClient_freeMessage(&message);
        MQTTClient_free(topicName);
        return 1;
    }

    void DevicePollLoop(MqttDeviceWorker* worker) {
        // --- NEW: Initialize all option structs ---
        MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
        MQTTClient_SSLOptions ssl_opts = MQTTClient_SSLOptions_initializer;
        MQTTClient_willOptions lwt_opts = MQTTClient_willOptions_initializer;

        // --- 1. Populate Connect Options ---
        conn_opts.keepAliveInterval = worker->keepAlive;
        conn_opts.cleansession = worker->cleanSession;
        conn_opts.connectTimeout = worker->connectTimeout;

        if (!worker->username.empty()) {
            conn_opts.username = worker->username.c_str();
            conn_opts.password = worker->password.c_str();
        }

        // --- 2. Populate LWT (Will) Options ---
        if (!worker->lwtTopic.empty()) {
            lwt_opts.topicName = worker->lwtTopic.c_str();
            lwt_opts.message = worker->lwtPayload.c_str();
            lwt_opts.qos = worker->lwtQos;
            lwt_opts.retained = worker->lwtRetain;
            conn_opts.will = &lwt_opts; // Link LWT to connect options
        }

        // --- 3. Populate SSL/TLS Options ---
        if (worker->useTls) {
            // Paho calls the client certificate 'keyStore'
            ssl_opts.keyStore = worker->certFile.empty() ? NULL : worker->certFile.c_str();
            ssl_opts.privateKey = worker->keyFile.empty() ? NULL : worker->keyFile.c_str();
            ssl_opts.trustStore = worker->caFile.empty() ? NULL : worker->caFile.c_str();

            // This enables server certificate verification
            ssl_opts.enableServerCertAuth = 1;

            conn_opts.ssl = &ssl_opts; // Link SSL to connect options
        }
        // --- End of NEW Option Setup ---

        // Ensure client is null before creation
        {
            std::lock_guard<std::mutex> lock(worker->client_mutex);
            worker->client = nullptr;
        }

        int rc = MQTTClient_create(&worker->client, worker->brokerUri.c_str(), worker->clientId.c_str(), MQTTCLIENT_PERSISTENCE_NONE, NULL);
        if (rc != MQTTCLIENT_SUCCESS) {
            worker->status = "Error: Failed to create client"; return;
        }

        MQTTClient_setCallbacks(worker->client, worker, NULL, on_message_arrived, NULL);

        if (MQTTClient_connect(worker->client, &conn_opts) != MQTTCLIENT_SUCCESS) {
            worker->status = "Error: Connection Failed";
            MQTTClient_destroy(&worker->client);
            return;
        }

        worker->status = "Running & Subscribed";
        for (const std::string& topic : worker->subscribeTopics) {
            // --- NEW: Subscribe with specified QoS ---
            MQTTClient_subscribe(worker->client, topic.c_str(), worker->subscribeQos);
        }

        while (!worker->should_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        {
            std::lock_guard<std::mutex> lock(worker->client_mutex);
            MQTTClient_disconnect(worker->client, 1000);
            MQTTClient_destroy(&worker->client);
            worker->client = nullptr;
        }
        worker->status = "Stopped";
    }
    std::thread m_reaper_thread;
    std::mutex m_reaper_mutex;
    std::vector<std::unique_ptr<MqttDeviceWorker>> m_reaper_queue;
    void ReaperLoop() {
        while (!m_should_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::lock_guard<std::mutex> lock(m_reaper_mutex);
            m_reaper_queue.erase(std::remove_if(m_reaper_queue.begin(), m_reaper_queue.end(),
                [](std::unique_ptr<MqttDeviceWorker>& worker) {
                    if (worker->thread.joinable()) {
                        worker->thread.join();
                    }
                    return true;
                }),
                m_reaper_queue.end());
        }
    }
    std::map<std::string, std::unique_ptr<MqttDeviceWorker>> m_devices;
    std::mutex m_device_mutex;
};


// --- ZmqAdapter ---

// A simple thread-safe queue for passing egress commands to the worker thread
template <typename T>
class ThreadSafeQueue {
public:
    void push(T value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(value));
        m_cond.notify_one();
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) {
            return std::nullopt;
        }
        T value = std::move(m_queue.front());
        m_queue.pop();
        return value;
    }

private:
    std::queue<T> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cond; // Not used for try_pop, but good to have
};
// --- ZmqAdapter ---

// (Requires the ThreadSafeQueue class from above)

struct ZmqDeviceWorker {
    std::string name;
    std::string endpoint;
    std::string topic; // Used for SUB filter & PUB topic
    std::string pattern; // "SUB", "PUB", "PULL", "PUSH", "REQ", "REP", "DEALER", "ROUTER"
    std::string bind_or_connect; // "bind" or "connect"
    std::string identity; // For DEALER/ROUTER

    std::thread thread;
    std::atomic<bool> should_stop;
    std::string status;
    IProtocolAdapter* adapter;
    zmq::context_t& zmq_context;

    // Egress queue for HandleCommand -> DevicePollLoop
    ThreadSafeQueue<std::string> egress_queue;

    ZmqDeviceWorker(std::string n, std::string ep, std::string t, std::string p, std::string boc, std::string id, IProtocolAdapter* parent, zmq::context_t& ctx)
        : name(n), endpoint(ep), topic(t), pattern(p), bind_or_connect(boc), identity(id),
        should_stop(false), status("Idle"), adapter(parent), zmq_context(ctx) {
    }
};

class ZmqAdapter : public IProtocolAdapter {
public:
    ZmqAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx)
        : IProtocolAdapter(name, ctx, io_ctx) {
        m_reaper_thread = std::thread(&ZmqAdapter::ReaperLoop, this);
    }

    ~ZmqAdapter() {
        {
            std::lock_guard<std::mutex> lock(m_device_mutex);
            for (auto& [name, worker] : m_devices) {
                worker->should_stop = true;
            }
        }
        if (m_reaper_thread.joinable()) {
            m_reaper_thread.join();
        }
        for (auto& [name, worker] : m_devices) {
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
        }
        m_devices.clear();
    }

    std::string GetProtocol() override { return "ZMQ"; }

    std::map<std::string, std::string> GetDeviceStatuses() override {
        std::map<std::string, std::string> statuses;
        std::lock_guard<std::mutex> lock(m_device_mutex);
        for (const auto& [name, worker] : m_devices) {
            statuses[name] = worker->status;
        }
        return statuses;
    }

    bool AddDevice(const std::string& device_name, const DeviceConfig& config) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (m_devices.count(device_name)) return false;

        try {
            std::string endpoint = config.at("endpoint");
            std::string pattern = config.at("pattern");
            std::string bind_or_connect = config.at("bind_or_connect");

            auto getConfigOr = [&](const std::string& key, const std::string& def) {
                auto it = config.find(key);
                return (it != config.end()) ? it->second : def;
                };

            std::string topic = getConfigOr("topic", "");
            std::string identity = getConfigOr("identity", "");

            auto worker = std::make_unique<ZmqDeviceWorker>(device_name, endpoint, topic, pattern, bind_or_connect, identity, this, m_zmq_context);
            worker->thread = std::thread(&ZmqAdapter::DevicePollLoop, this, worker.get());
            m_devices[device_name] = std::move(worker);

            AddLog("ZMQ: Added " + pattern + " device " + device_name);
            return true;
        }
        catch (const std::exception& e) {
            AddLog("ZMQ AddDevice error: " + std::string(e.what()));
            return false;
        }
    }

    bool RemoveDevice(const std::string& device_name) override {
        std::unique_ptr<ZmqDeviceWorker> worker_to_reap;
        {
            std::lock_guard<std::mutex> lock(m_device_mutex);
            if (!m_devices.count(device_name)) return false;
            auto& worker = m_devices[device_name];
            worker->should_stop = true;
            worker_to_reap = std::move(m_devices[device_name]);
            m_devices.erase(device_name);
        }
        {
            std::lock_guard<std::mutex> lock(m_reaper_mutex);
            m_reaper_queue.push_back(std::move(worker_to_reap));
        }
        AddLog("ZMQ: Queued device for removal: " + device_name);
        return true;
    }

    bool RestartDevice(const std::string& device_name) override {
        DeviceConfig config;
        {
            std::lock_guard<std::mutex> lock(m_device_mutex);
            auto it = m_devices.find(device_name);
            if (it == m_devices.end()) return false;

            config["endpoint"] = it->second->endpoint;
            config["topic"] = it->second->topic;
            config["pattern"] = it->second->pattern;
            config["bind_or_connect"] = it->second->bind_or_connect;
            config["identity"] = it->second->identity;
        }
        RemoveDevice(device_name);
        return AddDevice(device_name, config);
    }

    bool IsDeviceActive(const std::string& device_name) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        return m_devices.count(device_name);
    }

protected:
    // --- THIS IS THE NEWLY IMPLEMENTED EGRESS PATH ---
    void HandleCommand(const std::string& device_name, const nlohmann::json& cmd) override {
        if (g_log_show_egress) AddLog("ZMQ HandleCommand: " + cmd.dump(), LogType::EGRESS);

        std::lock_guard<std::mutex> lock(m_device_mutex);
        auto it = m_devices.find(device_name);
        if (it != m_devices.end()) {
            // Push the command payload (as a JSON string) to the worker's thread-safe queue
            it->second->egress_queue.push(cmd.dump());
        }
    }

    // --- HELPER TO FORMAT AND PUSH INGRESS DATA ---
    void PushIngressData(ZmqDeviceWorker* worker, const std::string& topic, const std::string& payload) {
        worker->status = "Msg received on " + topic;

        nlohmann::json j;
        j["deviceId"] = worker->name;
        j["adapterName"] = worker->adapter->GetName();
        j["protocol"] = worker->adapter->GetProtocol();
        j["timestamp"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

        nlohmann::json parsed_payload;
        try { parsed_payload = nlohmann::json::parse(payload); }
        catch (...) { parsed_payload = payload; }

        j["values"][topic] = parsed_payload;
        PushData(j.dump()); // Push to the central Hub
    }

    // --- HELPER TO SEND EGRESS DATA FROM THE WORKER THREAD ---
    void SendEgressData(zmq::socket_t& socket, ZmqDeviceWorker* worker, const std::string& cmd_string) {
        std::string pattern = worker->pattern;

        if (pattern == "PUB") {
            socket.send(zmq::const_buffer(worker->topic.c_str(), worker->topic.length()), zmq::send_flags::sndmore);
            socket.send(zmq::const_buffer(cmd_string.c_str(), cmd_string.length()), zmq::send_flags::none);
        }
        else if (pattern == "PUSH" || pattern == "REQ" || pattern == "DEALER" || pattern == "REP") {
            socket.send(zmq::const_buffer(cmd_string.c_str(), cmd_string.length()), zmq::send_flags::none);
        }
        else if (pattern == "ROUTER") {
            try {
                // ROUTER commands MUST be a JSON object with "targetIdentity" and "payload"
                nlohmann::json j_cmd = nlohmann::json::parse(cmd_string);
                std::string target_id = j_cmd.at("targetIdentity");
                std::string message = j_cmd.at("payload").dump(); // Re-serialize payload

                socket.send(zmq::const_buffer(target_id.c_str(), target_id.length()), zmq::send_flags::sndmore);
                socket.send(zmq::message_t(0), zmq::send_flags::sndmore); // Empty delimiter
                socket.send(zmq::const_buffer(message.c_str(), message.length()), zmq::send_flags::none);
            }
            catch (std::exception& e) {
                worker->status = std::string("ROUTER send error: ") + e.what();
            }
        }
    }


    // --- COMPLETELY REWRITTEN DEVICEPOLLLOOP ---
    void DevicePollLoop(ZmqDeviceWorker* worker) {
        try {
            // --- 1. Create Socket ---
            zmq::socket_t socket(worker->zmq_context, zmq::socket_type::sub); // default
            if (worker->pattern == "PUB") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::pub);
            else if (worker->pattern == "SUB") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::sub);
            else if (worker->pattern == "PUSH") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::push);
            else if (worker->pattern == "PULL") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::pull);
            else if (worker->pattern == "REQ") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::req);
            else if (worker->pattern == "REP") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::rep);
            else if (worker->pattern == "DEALER") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::dealer);
            else if (worker->pattern == "ROUTER") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::router);

            // --- 2. Set Options ---
            if (worker->pattern == "SUB") {
                socket.set(zmq::sockopt::subscribe, worker->topic);
            }
            if (worker->pattern == "DEALER" || worker->pattern == "ROUTER") {
                if (!worker->identity.empty()) {
                    socket.set(zmq::sockopt::routing_id, worker->identity);
                }
            }

            // Set a small receive timeout so the egress queue check is responsive
            socket.set(zmq::sockopt::rcvtimeo, 10);

            // --- 3. Bind or Connect ---
            if (worker->bind_or_connect == "bind") {
                socket.bind(worker->endpoint);
                worker->status = "Bound to " + worker->endpoint;
            }
            else {
                socket.connect(worker->endpoint);
                worker->status = "Connected to " + worker->endpoint;
            }

            // --- 4. Main Poll Loop ---
            while (!worker->should_stop) {

                // --- A: Check for Ingress Data (Device -> Hub) ---
                // These patterns can receive data
                if (worker->pattern == "SUB" || worker->pattern == "PULL" || worker->pattern == "REP" ||
                    worker->pattern == "DEALER" || worker->pattern == "ROUTER" || worker->pattern == "REQ")
                {
                    // REQ sockets block after send, so skip non-blocking recv
                    if (worker->pattern != "REQ") {
                        zmq::message_t topic_msg, data_msg, id_msg, empty_msg;

                        // Poll for non-blocking receive
                        std::optional<size_t> rcv_size;
                        if (worker->pattern == "ROUTER") rcv_size = socket.recv(id_msg, zmq::recv_flags::dontwait);
                        else rcv_size = socket.recv(topic_msg, zmq::recv_flags::dontwait);

                        if (rcv_size.has_value()) {
                            std::string topic, payload, identity;

                            if (worker->pattern == "SUB") {
                                socket.recv(data_msg, zmq::recv_flags::none); // Block for 2nd part
                                topic = topic_msg.to_string();
                                payload = data_msg.to_string();
                            }
                            else if (worker->pattern == "PULL" || worker->pattern == "REP" || worker->pattern == "DEALER") {
                                topic = worker->name; // No topic, use device name
                                payload = topic_msg.to_string();
                            }
                            else if (worker->pattern == "ROUTER") {
                                socket.recv(empty_msg, zmq::recv_flags::none); // Block for empty
                                socket.recv(data_msg, zmq::recv_flags::none);  // Block for payload
                                identity = id_msg.to_string();
                                topic = identity; // Use identity as topic
                                payload = data_msg.to_string();
                            }

                            PushIngressData(worker, topic, payload);

                            // REP socket *must* send a reply
                            if (worker->pattern == "REP") {
                                // Check for a queued command to send as reply
                                auto cmd = worker->egress_queue.try_pop();
                                if (cmd.has_value()) {
                                    SendEgressData(socket, worker, *cmd);
                                }
                                else {
                                    socket.send(zmq::str_buffer("ACK"), zmq::send_flags::none); // Send default ACK
                                }
                            }
                        }
                    }
                }

                // --- B: Check for Egress Data (Hub -> Device) ---
                // These patterns can send data
                if (worker->pattern == "PUB" || worker->pattern == "PUSH" || worker->pattern == "REQ" ||
                    worker->pattern == "DEALER" || worker->pattern == "ROUTER" || worker->pattern == "REP")
                {
                    // Don't pop queue for REP, it's handled above on receive
                    if (worker->pattern != "REP") {
                        auto cmd_str = worker->egress_queue.try_pop();
                        if (cmd_str.has_value()) {
                            SendEgressData(socket, worker, *cmd_str);

                            // REQ socket *must* wait for a reply
                            if (worker->pattern == "REQ") {
                                zmq::message_t reply_msg;
                                if (socket.recv(reply_msg, zmq::recv_flags::none)) {
                                    PushIngressData(worker, worker->name + "_reply", reply_msg.to_string());
                                }
                                else {
                                    worker->status = "Error: REQ no reply";
                                }
                            }
                        }
                    }
                }

                // If a pattern does nothing in the loop (e.g. pure PUB/PUSH)
                // this sleep_for will prevent 100% CPU use.
                // For SUB/PULL etc. this is just a small delay.
                if (worker->pattern == "PUB" || worker->pattern == "PUSH") {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        }
        catch (const zmq::error_t& e) {
            if (e.num() != ETERM) worker->status = std::string("Error: ") + e.what();
        }
        worker->status = "Stopped";
    }

    // --- REAPER (Unchanged) ---
    std::thread m_reaper_thread;
    std::mutex m_reaper_mutex;
    std::vector<std::unique_ptr<ZmqDeviceWorker>> m_reaper_queue;
    void ReaperLoop() {
        while (!m_should_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::lock_guard<std::mutex> lock(m_reaper_mutex);
            m_reaper_queue.erase(std::remove_if(m_reaper_queue.begin(), m_reaper_queue.end(),
                [](std::unique_ptr<ZmqDeviceWorker>& worker) {
                    if (worker->thread.joinable()) {
                        worker->thread.join();
                    }
                    return true;
                }),
                m_reaper_queue.end());
        }
    }

    std::map<std::string, std::unique_ptr<ZmqDeviceWorker>> m_devices;
    std::mutex m_device_mutex;
};

// --- GatewayHub (V3 REFECTOR) ---
class GatewayHub {
private:
    std::map<std::string, DeviceData> m_devices;
    std::mutex m_devices_mutex;
    std::map<std::string, std::unique_ptr<IProtocolAdapter>> m_adapters;
    mutable std::mutex m_adapters_mutex;

    // ZMQ
    zmq::context_t m_zmq_context;
    std::thread m_command_bridge_thread;
    std::atomic<bool> m_command_bridge_running;
    zmq::socket_t m_cmd_router_socket;
    std::queue<nlohmann::json> m_command_queue;
    std::mutex m_command_queue_mutex;

    // V3 Data Proxy Thread
    std::thread m_data_proxy_thread;
    std::atomic<bool> m_proxy_running;

    // V3 Aggregator Thread
    std::thread m_aggregator_thread;
    std::atomic<bool> m_aggregator_running;

    // V3 uWS Subscriber Thread
    std::thread m_uws_subscriber_thread;
    std::atomic<bool> m_uws_subscriber_running;

    // uWebSockets
    std::thread m_uws_thread;
    std::queue<std::string> m_data_to_publish;
    std::mutex m_publish_queue_mutex;
    uWS::App* m_uws_app_ptr = nullptr;
    std::atomic<bool> m_uws_running;
    uWS::Loop* m_uws_loop = nullptr;
    std::mutex m_uws_mutex;
    std::condition_variable m_uws_cv;

    // Asio Thread Pool
    boost::asio::io_context m_io_context;
    std::vector<std::thread> m_thread_pool;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> m_work_guard;

    // This assumes m_adapters_mutex is already locked by the caller!
    bool _IsDeviceNameInUse_NoLock(const std::string& device_name) {
        for (auto& [adapter_name, adapter] : m_adapters) {
            // GetDeviceStatuses() has its own internal lock
            if (adapter->GetDeviceStatuses().count(device_name)) {
                return true;
            }
        }
        return false;
    }

public:
    GatewayHub()
        : m_zmq_context(1),
        m_command_bridge_running(false),
        m_cmd_router_socket(m_zmq_context, zmq::socket_type::router),
        m_proxy_running(false),
        m_aggregator_running(false),
        m_uws_subscriber_running(false),
        m_uws_running(false),
        m_work_guard(boost::asio::make_work_guard(m_io_context))
    {
        AddLog("GatewayHub constructed.");
    }

    ~GatewayHub() {
        Stop();
    }

    void Start() {
        m_command_bridge_running = true;
        m_proxy_running = true;
        m_aggregator_running = true;
        m_uws_subscriber_running = true;

        int pool_size = (s_worker_pool_size > 0) ? s_worker_pool_size : 4;
        for (int i = 0; i < pool_size; ++i) {
            m_thread_pool.emplace_back([this] {
                m_io_context.run();
                });
        }
        AddLog("Asio worker pool started with " + std::to_string(pool_size) + " threads.");
        PushNotification("Asio worker pool started", true);

        m_cmd_router_socket.bind("inproc://command_stream");

        m_data_proxy_thread = std::thread(&GatewayHub::RunDataProxy, this);
        m_aggregator_thread = std::thread(&GatewayHub::RunAggregator, this);
        m_uws_subscriber_thread = std::thread(&GatewayHub::RunUwsSubscriber, this);
        m_command_bridge_thread = std::thread(&GatewayHub::RunCommandBridge, this);

        AddLog("Gateway Hub V3 core threads started.");
        PushNotification("Gateway Hub started.", true);
    }

    void Stop() {
        m_command_bridge_running = false;
        m_proxy_running = false;
        m_aggregator_running = false;
        m_uws_subscriber_running = false;

        if (m_command_bridge_thread.joinable()) m_command_bridge_thread.join();
        if (m_data_proxy_thread.joinable()) m_data_proxy_thread.join();
        if (m_aggregator_thread.joinable()) m_aggregator_thread.join();
        if (m_uws_subscriber_thread.joinable()) m_uws_subscriber_thread.join();

        m_cmd_router_socket.close();

        if (m_uws_thread.joinable()) {
            m_uws_thread.detach();
        }

        {
            std::lock_guard<std::mutex> lock(m_adapters_mutex);
            for (auto& [name, adapter] : m_adapters) {
                adapter->Stop();
                adapter->Join();
            }
            m_adapters.clear();
        }
        m_work_guard.reset();
        m_io_context.stop();
        for (auto& t : m_thread_pool) {
            if (t.joinable()) t.join();
        }
        AddLog("Gateway Hub stopped.");
    }

    void StartUwsServer() {
        try {
            if (m_uws_running) {
                PushNotification("uWS Server is already running.", false);
                return;
            }
            if (m_uws_thread.joinable()) {
                m_uws_thread.join();
            }

            m_uws_running = true;
            m_uws_thread = std::thread(&GatewayHub::RunUwsServer, this);

            std::unique_lock<std::mutex> lock(m_uws_mutex);
            if (m_uws_cv.wait_for(lock, std::chrono::seconds(5),
                [this] { return m_uws_loop != nullptr && m_uws_app_ptr != nullptr; })) {

                AddLog("uWS Server initialized by user.");
            }
            else {
                AddLog("uWS Server failed to initialize (timeout).", LogType::SYSTEM);
                PushNotification("uWS Server failed to start (timeout).", false);
                m_uws_running = false;

                if (m_uws_thread.joinable()) {
                    m_uws_thread.join();
                }
            }
        }
        catch (...) {
            AddLog("Unexpected exception starting uWS server.", LogType::SYSTEM);
            m_uws_running = false;

            if (m_uws_thread.joinable()) {
                m_uws_thread.join();
            }
        }
    }

    bool GetUwsStatus() const {
        return m_uws_running;
    }

    bool HasAdapters() const {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        return !m_adapters.empty();
    }

    void RunCommandBridge() {
        AddLog("Command bridge thread started.");
        try {
            zmq::pollitem_t items[] = {
                { m_cmd_router_socket, 0, ZMQ_POLLIN, 0 }
            };

            while (m_command_bridge_running) {
                int rc = zmq::poll(items, 1, std::chrono::milliseconds(100));
                if (rc == -1) {
                    if (m_command_bridge_running) AddLog("Command bridge poll error");
                    break;
                }
                if (items[0].revents & ZMQ_POLLIN) {
                    zmq::message_t id_msg, empty_msg, payload_msg;
                    m_cmd_router_socket.recv(id_msg, zmq::recv_flags::none);
                    m_cmd_router_socket.recv(empty_msg, zmq::recv_flags::none);
                    m_cmd_router_socket.recv(payload_msg, zmq::recv_flags::none);
                    AddLog("Bridge: Received msg from adapter " + id_msg.to_string() + ": " + payload_msg.to_string());
                }
                nlohmann::json cmd_json;
                bool cmd_found = false;
                {
                    std::lock_guard<std::mutex> lock(m_command_queue_mutex);
                    if (!m_command_queue.empty()) {
                        cmd_json = m_command_queue.front();
                        m_command_queue.pop();
                        cmd_found = true;
                    }
                }
                if (cmd_found) {
                    std::string adapter_name = cmd_json.value("targetAdapter", "");
                    if (adapter_name.empty()) continue;
                    std::string payload = cmd_json.dump();
                    bool adapter_found = false;
                    {
                        std::lock_guard<std::mutex> lock(m_adapters_mutex);
                        adapter_found = m_adapters.count(adapter_name);
                    }
                    if (adapter_found) {
                        if (g_log_show_egress) AddLog("Bridge: Routing command to adapter " + adapter_name, LogType::EGRESS);
                        m_cmd_router_socket.send(zmq::buffer(adapter_name), zmq::send_flags::sndmore);
                        m_cmd_router_socket.send(zmq::buffer(""), zmq::send_flags::sndmore);
                        m_cmd_router_socket.send(zmq::buffer(payload), zmq::send_flags::none);
                    }
                    else {
                        AddLog("Bridge: Command for unknown adapter " + adapter_name, LogType::EGRESS);
                        PushNotification("Error: Command for unknown adapter '" + adapter_name + "'", false);
                    }
                }
            }
        }
        catch (const zmq::error_t& e) {
            if (m_command_bridge_running) AddLog("Command bridge thread exception: " + std::string(e.what()));
        }
        AddLog("Command bridge thread stopped.");
    }

    void RunDataProxy() {
        AddLog("Data proxy thread started.");
        try {
            zmq::socket_t pull(m_zmq_context, zmq::socket_type::pull);
            pull.bind("inproc://data_ingress");
            zmq::socket_t pub(m_zmq_context, zmq::socket_type::pub);
            pub.bind("inproc://data_pubsub");
            pull.set(zmq::sockopt::rcvtimeo, 100);
            pull.set(zmq::sockopt::rcvhwm, g_zmq_rcvhwm);   // Limit queue to 10000 messages
            while (m_proxy_running) {
                zmq::message_t msg;
                std::optional<size_t> res = pull.recv(msg, zmq::recv_flags::none);
                if (res.has_value()) {
                    pub.send(msg, zmq::send_flags::none);
                }
            }
        }
        catch (const zmq::error_t& e) {
            if (m_proxy_running) AddLog("Data proxy thread exception: " + std::string(e.what()));
        }
        AddLog("Data proxy thread stopped.");
    }

    void RunAggregator() {
        AddLog("Aggregator thread started.");
        try {
            zmq::socket_t sub(m_zmq_context, zmq::socket_type::sub);
            sub.connect("inproc://data_pubsub");
            sub.set(zmq::sockopt::subscribe, "");
            sub.set(zmq::sockopt::conflate, 1);
            sub.set(zmq::sockopt::rcvtimeo, 100);

            while (m_aggregator_running) {
                zmq::message_t msg;
                std::optional<size_t> res = sub.recv(msg, zmq::recv_flags::none);

                if (res.has_value()) { // We got a message
                    std::string s = msg.to_string();
                    std::string adapterName, deviceId;

                    // --- Step 1: Parse both keys ---
                    try {
                        nlohmann::json data = nlohmann::json::parse(s);
                        adapterName = data.value("adapterName", "");
                        // NOTE: Your MqttAdapter JSON uses worker->name, which is the deviceId
                        deviceId = data.value("deviceId", "");
                    }
                    catch (const std::exception& e) {
                        AddLog("Aggregator: Bad JSON: " + std::string(e.what()));
                        continue; // Ignore bad JSON
                    }

                    if (adapterName.empty() || deviceId.empty()) continue;

                    // --- Step 2: Perform the NEW TWO-LEVEL "Source of Truth" check ---
                    bool deviceIsStillActive = false;
                    {
                        std::lock_guard<std::mutex> adapter_lock(m_adapters_mutex);
                        auto it = m_adapters.find(adapterName);

                        if (it != m_adapters.end()) {
                            // Adapter is alive. Now ask the adapter: is the DEVICE alive?
                            deviceIsStillActive = it->second->IsDeviceActive(deviceId);
                        }
                        // else: adapter is gone, so deviceIsStillActive remains false
                    }

                    // --- Step 3: Act on verified data ---
                    if (deviceIsStillActive) {
                        // BOTH adapter and device are confirmed alive.
                        // It's safe to create or update.
                        std::lock_guard<std::mutex> lock(m_devices_mutex);
                        _UpdateDeviceMap(s);
                    }
                    else {
                        // This is a "zombie" message. The adapter is gone OR
                        // the adapter has already deleted this device.
                        // We must drop the message.

                        // As a final cleanup, we can also force-remove
                        // the device from the hub's map, just in case.
                        std::lock_guard<std::mutex> lock(m_devices_mutex);
                        if (m_devices.count(deviceId)) {
                            m_devices.erase(deviceId);
                        }
                    }
                }
            }
        }
        catch (const zmq::error_t& e) {
            if (m_aggregator_running) AddLog("Aggregator thread exception: " + std::string(e.what()));
        }
        AddLog("Aggregator thread stopped.");
    }

    void RunUwsSubscriber() {
        AddLog("uWS Subscriber thread started.");
        try {
            zmq::socket_t sub(m_zmq_context, zmq::socket_type::sub);
            sub.connect("inproc://data_pubsub");
            sub.set(zmq::sockopt::subscribe, "");
            sub.set(zmq::sockopt::rcvtimeo, 100);
            sub.set(zmq::sockopt::conflate, 1);
            while (m_uws_subscriber_running) {
                zmq::message_t msg;
                std::optional<size_t> res = sub.recv(msg, zmq::recv_flags::none);
                if (res.has_value()) {
                    std::string data_str = msg.to_string();

                    // --- MODIFIED: Check filter *before* logging ---
                    if (g_log_show_ingress) {
                        AddLog("Ingress: " + data_str, LogType::INGRESS);
                    }

                    if (m_uws_running && m_uws_loop) {
                        {
                            std::lock_guard<std::mutex> lock(m_publish_queue_mutex);
                            m_data_to_publish.push(data_str);
                        }
                        m_uws_loop->defer([this] { DeferPublishData(); });
                    }
                }
            }
        }
        catch (const zmq::error_t& e) {
            if (m_uws_subscriber_running) AddLog("uWS Subscriber thread exception: " + std::string(e.what()));
        }
        AddLog("uWS Subscriber thread stopped.");
    }

    void RunUwsServer() {
        AddLog("uWS thread started.");
        try {
            m_uws_loop = uWS::Loop::get();
            uWS::App local_app;
            m_uws_app_ptr = &local_app;
            uWS::App::WebSocketBehavior<std::string> behavior;
            behavior.open = [this](auto* ws) {
                AddLog("Web UI Client connected.");
                ws->subscribe("data/all");
                };
            behavior.message = [this](auto* ws, std::string_view message, uWS::OpCode opCode) {
                if (g_log_show_egress) AddLog("Web UI command received: " + std::string(message), LogType::EGRESS);
                try {
                    nlohmann::json cmd = nlohmann::json::parse(message);
                    std::lock_guard<std::mutex> lock(m_command_queue_mutex);
                    m_command_queue.push(cmd);
                }
                catch (const std::exception& e) {
                    AddLog("Web UI command error: " + std::string(e.what()), LogType::EGRESS);
                }
                };
            behavior.close = [this](auto* ws, int code, std::string_view message) {
                AddLog("Web UI Client disconnected.");
                };
            local_app.ws<std::string>("/*", std::move(behavior))
                .listen(s_ws_host, s_ws_port, [this](auto* token) {
                if (token) {
                    AddLog("uWS Server listening on port " + std::to_string(s_ws_port));
                    PushNotification("uWS Server started on port " + std::to_string(s_ws_port), true);
                    m_uws_running = true;
                }
                else {
                    AddLog("uWS Server FAILED to listen on port " + std::to_string(s_ws_port));
                    PushNotification(
                        "Error: Could not start server at " + s_ws_host + ":" + std::to_string(s_ws_port) +
                        ". The host or port may be invalid or already in use.",
                        false
                    );
                    m_uws_running = false;
                }
                    });
            {
                std::lock_guard<std::mutex> lock(m_uws_mutex);
                m_uws_cv.notify_one();
            }
            if (m_uws_running) {
                local_app.run(); // Blocking call
            }
        }
        catch (const std::exception& e) {
            AddLog("uWS Thread exception: " + std::string(e.what()));
        }
        m_uws_loop = nullptr;
        m_uws_app_ptr = nullptr;
        m_uws_running = false;
        AddLog("uWS Server thread stopped.");
    }

    void DeferPublishData() {
        if (!m_uws_app_ptr) return;
        std::queue<std::string> local_queue;
        {
            std::lock_guard<std::mutex> lock(m_publish_queue_mutex);
            std::swap(local_queue, m_data_to_publish);
        }
        while (!local_queue.empty()) {
            m_uws_app_ptr->publish("data/all", local_queue.front(), uWS::OpCode::TEXT);
            local_queue.pop();
        }
    }

    void _UpdateDeviceMap(const std::string& json_data) {
        try {
            nlohmann::json j = nlohmann::json::parse(json_data);
            std::string deviceId = j.value("deviceId", "");
            if (deviceId.empty()) return;
            auto& device = m_devices[deviceId];
            device.id = deviceId;
            device.adapter_name = j.value("adapterName", "N/A");
            device.protocol = j.value("protocol", "N/A");
            device.last_seen = j.value("timestamp", (time_t)0);
            if (j.contains("values")) {
                device.last_value_json = j["values"].dump();
            }
            device.message_count++;
        }
        catch (const nlohmann::json::parse_error& e) {
        }
    }

    void GetDeviceData(std::map<std::string, DeviceData>& devices) {
        std::lock_guard<std::mutex> lock(m_devices_mutex);
        devices = m_devices;
    }

    void GetLogs(std::vector<std::string>& logs) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        logs = g_logs;
    }

    void ClearLogs() {
        {
            std::lock_guard<std::mutex> lock(g_log_mutex);
            g_logs.clear();
        }
        AddLog("Log cleared.");
    }

    bool AddAdapter(const std::string& name, const std::string& protocol_type) {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        if (m_adapters.count(name)) {
            PushNotification("Error: Adapter '" + name + "' already exists.", false);
            return false;
        }
        if (protocol_type == "ModbusTCP") m_adapters[name] = std::make_unique<ModbusTCPAdapter>(name, m_zmq_context, m_io_context);
        else if (protocol_type == "OPC-UA") m_adapters[name] = std::make_unique<OpcuaAdapter>(name, m_zmq_context, m_io_context);
        else if (protocol_type == "MQTT") m_adapters[name] = std::make_unique<MqttAdapter>(name, m_zmq_context, m_io_context);
        else if (protocol_type == "ZMQ") m_adapters[name] = std::make_unique<ZmqAdapter>(name, m_zmq_context, m_io_context);
        else {
            PushNotification("Error: Unknown protocol type.", false);
            return false;
        }
        m_adapters[name]->Start();
        PushNotification("Adapter '" + name + "' added.", true);
        AddLog("Adapter '" + name + "' added.");
        return true;
    }

    bool RemoveAdapter(const std::string& name) {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        if (!m_adapters.count(name)) return false;
        m_adapters[name]->Stop();
        m_adapters[name]->Join();
        m_adapters.erase(name);
        PushNotification("Adapter '" + name + "' removed.", true);
        AddLog("Adapter '" + name + "' removed.");
        return true;
    }

    std::map<std::string, std::string> GetAdapterStatuses() {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        std::map<std::string, std::string> statuses;
        for (const auto& [name, adapter] : m_adapters) {
            statuses[name] = adapter->GetStatus();
        }
        return statuses;
    }

    std::string GetAdapterProtocol(const std::string& adapter_name) {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        if (m_adapters.count(adapter_name)) {
            return m_adapters[adapter_name]->GetProtocol();
        }
        return "Unknown";
    }

    bool AddDeviceToAdapter(const std::string& adapter_name, const std::string& device_name, const DeviceConfig& config) {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);

        // --- Global uniqueness check ---
        if (_IsDeviceNameInUse_NoLock(device_name)) {
            PushNotification("Error: Device name '" + device_name + "' is already in use!", false);
            AddLog("Attempted to add device with duplicate name: " + device_name);
            return false;
        }

        if (m_adapters.count(adapter_name)) {
            if (m_adapters[adapter_name]->AddDevice(device_name, config)) {
                PushNotification("Device '" + device_name + "' added to " + adapter_name, true);
                return true;
            }
        }
        PushNotification("Error adding device '" + device_name + "'", false);
        return false;
    }

    bool RemoveDeviceFromAdapter(const std::string& adapter_name, const std::string& device_name) {
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(m_adapters_mutex);
            if (m_adapters.count(adapter_name)) {
                if (m_adapters[adapter_name]->RemoveDevice(device_name)) {
                    PushNotification("Device '" + device_name + "' removed.", true);
                    removed = true;
                }
            }
        }
        if (removed) {
            std::lock_guard<std::mutex> lock(m_devices_mutex);
            for (auto it = m_devices.begin(); it != m_devices.end();) {
                bool match = false;
                if (it->second.adapter_name == adapter_name) {
                    if (it->first == device_name) {
                        match = true;
                    }
                    else if (it->first.rfind(device_name + ":", 0) == 0) {
                        match = true;
                    }
                }
                if (match) {
                    it = m_devices.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
        return removed;
    }

    bool RestartDevice(const std::string& adapter_name, const std::string& device_name) {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        if (m_adapters.count(adapter_name)) {
            if (m_adapters[adapter_name]->RestartDevice(device_name)) {
                PushNotification("Device '" + device_name + "' restarted.", true);
                return true;
            }
            PushNotification("Error restarting device '" + device_name + "'", false);
            return false;
        }
        return false;
    }

    std::map<std::string, std::string> GetDeviceStatusesForAdapter(const std::string& adapter_name) {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        if (m_adapters.count(adapter_name)) {
            return m_adapters[adapter_name]->GetDeviceStatuses();
        }
        return {};
    }

    void SendTestCommand(const std::string& adapter_name, const std::string& payload_json) {
        try {
            nlohmann::json cmd = nlohmann::json::parse(payload_json);
            cmd["targetAdapter"] = adapter_name;
            std::lock_guard<std::mutex> lock(m_command_queue_mutex);
            m_command_queue.push(cmd);
            PushNotification("Test command sent to " + adapter_name, true);
        }
        catch (const std::exception& e) {
            PushNotification(std::string("Command JSON error: ") + e.what(), false);
        }
    }
};


// =================================================================================
//
// Dear ImGui UI Rendering
//
// =================================================================================

void DrawGatewayUI(GatewayHub& hub) {
    ImGui::SetNextWindowSize(ImVec2(1000, 700), ImGuiCond_FirstUseEver);
    ImGui::Begin("Multiprotocol Gateway Hub");

    ImGui::Text("uWS Server:");
    ImGui::SameLine();

    bool adapters_exist = hub.HasAdapters();
    bool uws_running = hub.GetUwsStatus();

    // --- Local buffers for host and port ---
    static int port_buf = s_ws_port;
    static char s_ws_host_buf[256];
    static bool s_host_buf_initialized = false;
    if (!s_host_buf_initialized) {
        // One-time copy from the global std::string to the local char buffer
        strncpy(s_ws_host_buf, s_ws_host.c_str(), 256);
        s_ws_host_buf[255] = '\0'; // Ensure null-termination
        s_host_buf_initialized = true;
    }

    // --- Disable inputs if server is running or adapters exist ---
    if (uws_running) ImGui::BeginDisabled(true);

    // --- Port Input ---
    ImGui::PushItemWidth(100);
    if (ImGui::InputInt("Port", &port_buf)) {
        if (port_buf < 1024) port_buf = 1024;
        if (port_buf > 65535) port_buf = 65535;
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();

    // --- Host Input ---
    ImGui::PushItemWidth(150); // A bit wider for IP/domain
    ImGui::InputText("Host", s_ws_host_buf, 256);
    ImGui::PopItemWidth();

    // --- End Disable ---
    if (uws_running) ImGui::EndDisabled();

    ImGui::SameLine();

    if (uws_running) {
        // --- Running State ---
        // Display "localhost" if listening on "0.0.0.0" for better clarity
        const char* display_host = (s_ws_host == "0.0.0.0") ? "localhost" : s_ws_host.c_str();
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Running at ws://%s:%d", display_host, s_ws_port);
        ImGui::SameLine();
        float button_width = ImGui::CalcTextSize("Shut Down Server").x + ImGui::GetStyle().FramePadding.x * 2;
        float right_edge = ImGui::GetWindowContentRegionMax().x;
        ImGui::SetCursorPosX(right_edge - button_width);
        static bool show_exit_popup = false;
        if (ImGui::Button("Shut Down Server")) {
            show_exit_popup = true;
        }
        if (show_exit_popup) {
            ImGui::OpenPopup("Exit Confirmation");
        }
        if (ImGui::BeginPopupModal("Exit Confirmation", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Are you sure you want to exit?");
            ImGui::Separator();

            if (ImGui::Button("Yes", ImVec2(120, 0))) {
                std::exit(0);
            }
            ImGui::SameLine();
            if (ImGui::Button("No", ImVec2(120, 0))) {
                show_exit_popup = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    else {
        static bool server_running = false;

        ImGui::BeginDisabled(server_running);
        if (ImGui::Button("Start Server")) {
            s_ws_port = port_buf;
            s_ws_host = s_ws_host_buf;

            server_running = true;

            // Capture hub by reference is okay if it's global or static
            std::thread([&hub]() {
                hub.StartUwsServer();
                server_running = false;
                }).detach();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Stopped.");
    }
    ImGui::Separator();

    if (ImGui::BeginTabBar("MainTabs")) {
        // --- Tab 1: Live Log ---
        if (ImGui::BeginTabItem("Live Log")) {
            if (ImGui::Button("Clear Log")) {
                hub.ClearLogs();
            }
            ImGui::SameLine();
            static bool show_ingress = true;
            static bool show_egress = true;
            ImGui::Checkbox("Show Device -> Hub (Ingress)", &show_ingress);
            ImGui::SameLine();
            ImGui::Checkbox("Show Hub -> Device (Egress)", &show_egress);
            {
                std::lock_guard<std::mutex> lock(g_log_mutex);
                g_log_show_ingress = show_ingress;
                g_log_show_egress = show_egress;
            }
            static std::vector<std::string> logs;
            static int last_log_count = 0;
            hub.GetLogs(logs);
            bool scroll_to_bottom = (logs.size() != last_log_count);
            last_log_count = (int)logs.size();
            ImGui::BeginChild("LogScroll", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), false, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
            for (const auto& log : logs) {
                ImGui::TextUnformatted(log.c_str());
            }
            if (scroll_to_bottom) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::PopStyleVar();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // --- Tab 2: Device Status ---
        if (ImGui::BeginTabItem("Device Status")) {
            static std::map<std::string, DeviceData> devices;
            hub.GetDeviceData(devices);
            ImGui::Text("Discovered Devices: %zu", devices.size());
            if (ImGui::BeginTable("DeviceTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Device ID");
                ImGui::TableSetupColumn("Adapter");
                ImGui::TableSetupColumn("Protocol");
                ImGui::TableSetupColumn("Msg Count");
                ImGui::TableSetupColumn("Last Seen");
                ImGui::TableSetupColumn("Last Value (JSON)");
                ImGui::TableHeadersRow();
                for (auto const& [id, data] : devices) {
                    ImGui::PushID(id.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%s", id.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%s", data.adapter_name.c_str());
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%s", data.protocol.c_str());
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%d", data.message_count);
                    ImGui::TableSetColumnIndex(4);
                    char time_buf[100];
                    strftime(time_buf, 100, "%Y-%m-%d %H:%M:%S", std::localtime(&data.last_seen));
                    ImGui::Text("%s", time_buf);
                    ImGui::TableSetColumnIndex(5); ImGui::Text("%s", data.last_value_json.c_str());
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // --- Tab 3: Adapter Management ---
        if (ImGui::BeginTabItem("Adapter Management")) {
            static const char* protocol_types[] = { "ModbusTCP", "OPC-UA", "MQTT", "ZMQ" };
            static int current_protocol_idx = 0;
            static char adapter_name_buf[128] = "ModbusService1";
            ImGui::Text("Add New Adapter Service");
            ImGui::Separator();
            ImGui::PushItemWidth(100);
            int* p_pool_size = &s_worker_pool_size;
            bool adapters_exist = hub.HasAdapters();
            if (adapters_exist) ImGui::BeginDisabled(true);
            if (ImGui::InputInt("Asio Pool Size", p_pool_size, 1, 2)) {
                if (*p_pool_size < 1) *p_pool_size = 1;
                if (*p_pool_size > 64) *p_pool_size = 64;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(Default: %d)", s_hardware_cores);
            if (adapters_exist) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("(Remove all services to change)");
            }
            ImGui::PopItemWidth();
            ImGui::InputText("Service Name##AdapterName", adapter_name_buf, IM_ARRAYSIZE(adapter_name_buf));
            ImGui::Combo("Protocol Type##AdapterProto", &current_protocol_idx, protocol_types, IM_ARRAYSIZE(protocol_types));
            if (ImGui::Button("Add Service")) {
                hub.AddAdapter(adapter_name_buf, protocol_types[current_protocol_idx]);
            }
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::Text("Adapter Services & Devices");
            ImGui::Separator();
            static std::string selected_adapter_name = "";
            static std::string adapter_to_delete = "";
            static bool show_delete_popup = false;
            if (ImGui::BeginTable("AdapterLayout", 2, ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Adapter Services", ImGuiTableColumnFlags_WidthFixed, 300.0f);
                ImGui::TableSetupColumn("Device Configuration");
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::BeginChild("AdapterListChild");
                std::map<std::string, std::string> adapter_statuses = hub.GetAdapterStatuses();
                for (auto const& pair : adapter_statuses) {
                    const std::string& name = pair.first; const std::string& status = pair.second;
                    bool is_selected = (name == selected_adapter_name);
                    ImGui::PushID(name.c_str());
                    if (ImGui::Selectable(name.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        selected_adapter_name = name;
                    }
                    ImGui::PopID();
                    ImGui::TextDisabled("  %s", status.c_str());
                    ImGui::PushID(name.c_str());
                    if (ImGui::Button("Delete Service")) {
                        adapter_to_delete = name; show_delete_popup = true;
                    }
                    ImGui::PopID(); ImGui::Separator();
                }
                ImGui::EndChild();
                if (show_delete_popup) {
                    ImGui::OpenPopup("Delete Service?");
                    show_delete_popup = false;
                }
                if (ImGui::BeginPopupModal("Delete Service?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::Text("Warning: You are about to delete service '%s'.", adapter_to_delete.c_str());
                    ImGui::Text("This will also remove all devices running under it."); ImGui::Separator();
                    ImGui::Text("Are you sure you want to proceed?");
                    if (ImGui::Button("Yes, Safe Delete", ImVec2(120, 0))) {
                        auto devices = hub.GetDeviceStatusesForAdapter(adapter_to_delete);
                        for (const auto& [dev_name, dev_status] : devices) {
                            hub.RemoveDeviceFromAdapter(adapter_to_delete, dev_name);
                        }
                        hub.RemoveAdapter(adapter_to_delete);
                        if (selected_adapter_name == adapter_to_delete) selected_adapter_name = "";
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("No, Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::BeginChild("DeviceConfigChild");
                if (selected_adapter_name.empty()) {
                    ImGui::Text("Select an adapter service from the left to configure devices.");
                }
                else {
                    std::string protocol = hub.GetAdapterProtocol(selected_adapter_name);
                    ImGui::Text("Configuring Devices for: %s (%s)", selected_adapter_name.c_str(), protocol.c_str());
                    ImGui::Separator();
                    ImGui::Text("Add New Device");
                    static char dev_name_buf[128] = "plc_line_1";
                    static char dev_ip_buf[128] = "127.0.0.1";
                    static int dev_port_int = 502;
                    static char dev_opcua_ep_buf[256] = "opc.tcp://127.0.0.1:4840";
                    static char dev_opcua_nodes_buf[256] = "ns=1;s=MyVariable";
                    static char dev_mqtt_broker_buf[256] = "tcp://broker.hivemq.com:1883";
                    static char dev_mqtt_sub_topics_buf[256] = "gateway/data/#";
                    static char dev_mqtt_cmd_topic_buf[256] = "gateway/commands";
                    static char dev_mqtt_client_id_buf[128] = "";
                    static char dev_mqtt_user_buf[128] = "";
                    static char dev_mqtt_pass_buf[128] = "";
                    static bool dev_mqtt_clean_sess_bool = true;
                    static int dev_mqtt_keep_alive_int = 60;
                    static int dev_mqtt_sub_qos_int = 1;
                    static char dev_mqtt_lwt_topic_buf[256] = "";
                    static char dev_mqtt_lwt_payload_buf[256] = "";
                    static int dev_mqtt_lwt_qos_int = 1;
                    static bool dev_mqtt_lwt_retain_bool = false;
                    static bool dev_mqtt_tls_bool = false;
                    static char dev_mqtt_ca_buf[256] = "";
                    static char dev_mqtt_cert_buf[256] = "";
                    static char dev_mqtt_key_buf[256] = "";
                    static char dev_zmq_ep_buf[256] = "tcp://127.0.0.1:5555";
                    static char dev_zmq_topic_buf[256] = "gateway_data";
                    static const char* zmq_patterns[] = { "SUB", "PUB", "PULL", "PUSH", "REQ", "REP", "DEALER", "ROUTER" };
                    static int zmq_pattern_idx = 0;
                    static const char* zmq_boc[] = { "connect", "bind" };
                    static int zmq_boc_idx = 0;
                    static char dev_zmq_id_buf[256] = "";
                    ImGui::InputText("Device Name##DevName", dev_name_buf, IM_ARRAYSIZE(dev_name_buf));
                    if (protocol == "ModbusTCP") {
                        ImGui::InputText("IP Address##DevIP", dev_ip_buf, IM_ARRAYSIZE(dev_ip_buf));
                        ImGui::InputInt("Port##DevPort", &dev_port_int);
                    }
                    else if (protocol == "OPC-UA") {
                        ImGui::InputText("Endpoint URL##DevOpcuaEP", dev_opcua_ep_buf, IM_ARRAYSIZE(dev_opcua_ep_buf));
                        ImGui::InputText("NodeIDs (comma-sep)##DevOpcuaNodes", dev_opcua_nodes_buf, IM_ARRAYSIZE(dev_opcua_nodes_buf));
                    }
                    else if (protocol == "MQTT") {
                        ImGui::SeparatorText("Connection (Required)");
                        ImGui::InputText("Broker URI##DevMqttBroker", dev_mqtt_broker_buf, IM_ARRAYSIZE(dev_mqtt_broker_buf));
                        ImGui::InputText("Subscribe Topics##DevMqttSub", dev_mqtt_sub_topics_buf, IM_ARRAYSIZE(dev_mqtt_sub_topics_buf));

                        ImGui::SeparatorText("Identity & Authentication");
                        ImGui::InputText("Client ID##DevMqttClient", dev_mqtt_client_id_buf, IM_ARRAYSIZE(dev_mqtt_client_id_buf));
                        ImGui::SameLine(); ImGui::TextDisabled("(Optional)");
                        ImGui::InputText("Username##DevMqttUser", dev_mqtt_user_buf, IM_ARRAYSIZE(dev_mqtt_user_buf));
                        ImGui::InputText("Password##DevMqttPass", dev_mqtt_pass_buf, IM_ARRAYSIZE(dev_mqtt_pass_buf), ImGuiInputTextFlags_Password);
                        ImGui::InputText("Command Topic##DevMqttCmd", dev_mqtt_cmd_topic_buf, IM_ARRAYSIZE(dev_mqtt_cmd_topic_buf));

                        ImGui::SeparatorText("Session & Keep-Alive");
                        ImGui::Checkbox("Clean Session##DevMqttClean", &dev_mqtt_clean_sess_bool);

                        ImGui::PushItemWidth(100);
                        ImGui::InputInt("Keep Alive (s)##DevMqttKeepAlive", &dev_mqtt_keep_alive_int);
                        if (dev_mqtt_keep_alive_int < 0) dev_mqtt_keep_alive_int = 0;
                        ImGui::SameLine();
                        ImGui::InputInt("Subscribe QoS##DevMqttSubQoS", &dev_mqtt_sub_qos_int);
                        if (dev_mqtt_sub_qos_int < 0) dev_mqtt_sub_qos_int = 0;
                        if (dev_mqtt_sub_qos_int > 2) dev_mqtt_sub_qos_int = 2;
                        ImGui::PopItemWidth();

                        ImGui::SeparatorText("Last Will & Testament (LWT)");
                        ImGui::InputText("LWT Topic##DevMqttLWTTopic", dev_mqtt_lwt_topic_buf, IM_ARRAYSIZE(dev_mqtt_lwt_topic_buf));
                        ImGui::InputText("LWT Payload##DevMqttLWTPayload", dev_mqtt_lwt_payload_buf, IM_ARRAYSIZE(dev_mqtt_lwt_payload_buf));
                        ImGui::PushItemWidth(100);
                        ImGui::InputInt("LWT QoS##DevMqttLWTQoS", &dev_mqtt_lwt_qos_int);
                        if (dev_mqtt_lwt_qos_int < 0) dev_mqtt_lwt_qos_int = 0;
                        if (dev_mqtt_lwt_qos_int > 2) dev_mqtt_lwt_qos_int = 2;
                        ImGui::PopItemWidth();
                        ImGui::SameLine();
                        ImGui::Checkbox("LWT Retain##DevMqttLWTRetain", &dev_mqtt_lwt_retain_bool);

                        ImGui::SeparatorText("Security (TLS)");
                        ImGui::Checkbox("Use TLS##DevMqttTLS", &dev_mqtt_tls_bool);
                        if (dev_mqtt_tls_bool) {
                            ImGui::InputText("CA File Path##DevMqttCA", dev_mqtt_ca_buf, IM_ARRAYSIZE(dev_mqtt_ca_buf));
                            ImGui::InputText("Cert File Path##DevMqttCert", dev_mqtt_cert_buf, IM_ARRAYSIZE(dev_mqtt_cert_buf));
                            ImGui::InputText("Key File Path##DevMqttKey", dev_mqtt_key_buf, IM_ARRAYSIZE(dev_mqtt_key_buf));
                        }
                    }
                    else if (protocol == "ZMQ") {
                        ImGui::SeparatorText("ZMQ Configuration");
                        ImGui::Combo("Pattern", &zmq_pattern_idx, zmq_patterns, IM_ARRAYSIZE(zmq_patterns));
                        ImGui::Combo("Topology", &zmq_boc_idx, zmq_boc, IM_ARRAYSIZE(zmq_boc));
                        ImGui::InputText("Endpoint##DevEP", dev_zmq_ep_buf, IM_ARRAYSIZE(dev_zmq_ep_buf));

                        const char* pattern = zmq_patterns[zmq_pattern_idx];

                        // Only show Topic for SUB (filter) and PUB (topic)
                        if (strcmp(pattern, "SUB") == 0 || strcmp(pattern, "PUB") == 0) {
                            ImGui::InputText("Topic/Filter##DevTopic", dev_zmq_topic_buf, IM_ARRAYSIZE(dev_zmq_topic_buf));
                        }

                        // Only show Identity for DEALER and ROUTER
                        if (strcmp(pattern, "DEALER") == 0 || strcmp(pattern, "ROUTER") == 0) {
                            ImGui::InputText("Identity##DevIdentity", dev_zmq_id_buf, IM_ARRAYSIZE(dev_zmq_id_buf));
                            ImGui::SameLine(); ImGui::TextDisabled("(Optional for DEALER)");
                        }
                    }
                    if (ImGui::Button("Add Device")) {
                        DeviceConfig config;
                        if (protocol == "ModbusTCP") { config["ip"] = dev_ip_buf; config["port"] = std::to_string(dev_port_int); }
                        else if (protocol == "OPC-UA") { config["endpoint"] = dev_opcua_ep_buf; config["nodeIds"] = dev_opcua_nodes_buf; }
                        else if (protocol == "MQTT") {
                            // --- Required ---
                            config["brokerUri"] = dev_mqtt_broker_buf;
                            config["subscribeTopics"] = dev_mqtt_sub_topics_buf;

                            // --- Add all new fields to the config map ---
                            config["commandTopic"] = dev_mqtt_cmd_topic_buf;
                            config["clientId"] = dev_mqtt_client_id_buf;
                            config["username"] = dev_mqtt_user_buf;
                            config["password"] = dev_mqtt_pass_buf;

                            config["cleanSession"] = dev_mqtt_clean_sess_bool ? "true" : "false";
                            config["keepAlive"] = std::to_string(dev_mqtt_keep_alive_int);
                            config["subscribeQos"] = std::to_string(dev_mqtt_sub_qos_int);

                            config["lwtTopic"] = dev_mqtt_lwt_topic_buf;
                            config["lwtPayload"] = dev_mqtt_lwt_payload_buf;
                            config["lwtQos"] = std::to_string(dev_mqtt_lwt_qos_int);
                            config["lwtRetain"] = dev_mqtt_lwt_retain_bool ? "true" : "false";

                            config["useTls"] = dev_mqtt_tls_bool ? "true" : "false";
                            config["caFile"] = dev_mqtt_ca_buf;
                            config["certFile"] = dev_mqtt_cert_buf;
                            config["keyFile"] = dev_mqtt_key_buf;
                        }
                        else if (protocol == "ZMQ") {
                            config["endpoint"] = dev_zmq_ep_buf;
                            config["pattern"] = zmq_patterns[zmq_pattern_idx];
                            config["bind_or_connect"] = zmq_boc[zmq_boc_idx];
                            config["topic"] = dev_zmq_topic_buf;
                            config["identity"] = dev_zmq_id_buf;
                        }
                        hub.AddDeviceToAdapter(selected_adapter_name, dev_name_buf, config);
                    }
                    ImGui::Separator();
                    ImGui::Text("Managed Devices");
                    if (ImGui::BeginTable("DeviceListTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                        ImGui::TableSetupColumn("Device Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                        ImGui::TableHeadersRow();
                        std::map<std::string, std::string> device_statuses = hub.GetDeviceStatusesForAdapter(selected_adapter_name);
                        for (auto const& pair : device_statuses) {
                            const std::string& name = pair.first; const std::string& status = pair.second;
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", name.c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImVec4 status_color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                            if (status.find("Running") != std::string::npos) status_color = ImVec4(0, 1, 0, 1);
                            if (status.find("Error") != std::string::npos || status.find("Failed") != std::string::npos) status_color = ImVec4(1, 0, 0, 1);
                            if (status == "Stopped") status_color = ImVec4(1, 1, 0, 1);
                            if (status.find("Subscribed") != std::string::npos) status_color = ImVec4(0.0f, 0.7f, 1.0f, 1.0f);
                            ImGui::TextColored(status_color, "%s", status.c_str());
                            ImGui::TableSetColumnIndex(2);
                            ImGui::PushID(name.c_str());
                            bool is_failed = (status.find("Error") != std::string::npos || status.find("Failed") != std::string::npos);
                            if (is_failed) {
                                if (ImGui::Button("Reconnect")) hub.RestartDevice(selected_adapter_name, name);
                                ImGui::SameLine();
                            }
                            if (ImGui::Button("Remove")) hub.RemoveDeviceFromAdapter(selected_adapter_name, name);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndChild();
                ImGui::EndTable();
            }

            ImGui::EndTabItem();
        }

        // --- Tab 4: Send Test Command ---
        if (ImGui::BeginTabItem("Send Test Command")) {
            static char adapter_name_buf[128] = "MqttService1";
            static char command_json_buf[512] = "{\"targetDevice\":\"MyBroker\", \"command\":\"publish\", \"payload\": \"Hello from Hub\"}";
            ImGui::Text("Note: Commands are sent to the Adapter Service.");
            ImGui::Text("The service is responsible for routing to the correct device.");
            ImGui::InputText("Adapter Service Name##CmdTarget", adapter_name_buf, IM_ARRAYSIZE(adapter_name_buf));
            ImGui::TextUnformatted("Command JSON");
            ImGui::InputTextMultiline(
                "##CmdPayload",
                command_json_buf,
                IM_ARRAYSIZE(command_json_buf),
                ImVec2(ImGui::GetContentRegionAvail().x, 150)
            );
            if (ImGui::Button("Send Command")) {
                hub.SendTestCommand(adapter_name_buf, command_json_buf);
            }
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Note: The adapter's command handler (if implemented)\nmust parse the JSON to find the 'targetDevice'.");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // --- Notification Overlay ---
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 10, viewport->WorkPos.y + viewport->WorkSize.y - 10), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    if (ImGui::Begin("Notifications", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        std::lock_guard<std::mutex> lock(g_notification_mutex);
        double current_time = ImGui::GetTime();
        g_notifications.erase(std::remove_if(g_notifications.begin(), g_notifications.end(),
            [current_time](const Notification& n) { return n.expiry_time < current_time; }), g_notifications.end());
        for (const auto& n : g_notifications) {
            ImVec4 color = n.is_success ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1);
            ImGui::TextColored(color, "%s", n.message.c_str());
        }
    }
    ImGui::End();
    ImGui::End();
}


// =================================================================================
//
// Main Function
//
// =================================================================================

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "Glfw Error %d: %s\n", error, description);
    AddLog(std::string("Glfw Error: ") + description);
}

int main(int, char**) {
    // --- 1. Setup GLFW ---
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    // --- 2. Setup Window + OpenGL + GLEW ---
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Multiprotocol Gateway Hub", NULL, NULL);
    if (window == NULL)
        return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    if (glewInit() != GLEW_OK) {
        fprintf(stderr, "Failed to initialize GLEW\n");
        return 1;
    }

    // --- 3. Setup Dear ImGui ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // --- 4. Start the GatewayHub ---
    static GatewayHub hub;
    hub.Start();

    // --- 5. Main Render Loop ---
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        DrawGatewayUI(hub); // Render our UI

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // --- 6. Cleanup ---
    hub.Stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
