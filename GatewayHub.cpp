#pragma once

#include "GatewayHub.h"

// --- global variables ---
unsigned int s_hardware_cores = (std::thread::hardware_concurrency() > 0) ? std::thread::hardware_concurrency() : 4;
int s_worker_pool_size = (int)s_hardware_cores; // Default to the number of logical cores

// --- FORWARD DECLARATIONS (for circular dependency fix) ---
class ModbusTCPAdapter;
class OpcuaAdapter;
class GatewayHub; // Forward declare GatewayHub for global logger

// --- Global Logging & Notification System ---
std::mutex g_log_mutex;
std::vector<std::string> g_logs;
bool g_log_show_ingress = true; // Show (Device -> WebUI)
bool g_log_show_egress = true;  // Show (WebUI -> Device)
const size_t g_log_max_lines = 10000; // Max log lines
const int g_zmq_rcvhwm = 10000;        // Max queued ZMQ messages

std::vector<Notification> g_notifications;
std::mutex g_notification_mutex;

void AddLog(const std::string& msg, LogType type) {
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

    // Pass the arguments in the correct struct order
    g_notifications.emplace_back(message, ImGui::GetTime() + 5.0, is_success);
}

// --- IProtocolAdapter Interface ---
// --- IProtocolAdapter Implementation ---

IProtocolAdapter::IProtocolAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx)
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

IProtocolAdapter::~IProtocolAdapter() {
    Stop();
    Join();
    m_data_push_socket.close();
    m_cmd_socket.close();
}

void IProtocolAdapter::Start() {
    m_thread = std::thread(&IProtocolAdapter::Run, this);
}

void IProtocolAdapter::Stop() {
    m_should_stop = true;
}

void IProtocolAdapter::Join() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void IProtocolAdapter::Run() {
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

void IProtocolAdapter::PushData(const std::string& json_payload) {
    std::lock_guard<std::mutex> lock(m_zmq_push_mutex);
    m_data_push_socket.send(zmq::buffer(json_payload), zmq::send_flags::none);
}

// --- ModbusDeviceWorker Implementations ---

ModbusDeviceWorker::ModbusDeviceWorker(std::string n, std::string i, int p, boost::asio::io_context& io_ctx, IProtocolAdapter* adapter)
    : name(n), ip(i), port(p), is_connected(false), ctx(nullptr), status("Idle"), m_stopped(false),
    m_io_context(io_ctx),
    m_strand(boost::asio::make_strand(io_ctx)),
    m_poll_timer(io_ctx),
    m_adapter(adapter)
{
}

ModbusDeviceWorker::~ModbusDeviceWorker() {
    if (ctx) {
        modbus_close(ctx);
        modbus_free(ctx);
    }
}

void ModbusDeviceWorker::StartPoll() {
    boost::asio::post(m_strand,
        std::bind(&ModbusDeviceWorker::OnTimer, shared_from_this(), boost::system::error_code()));
}

void ModbusDeviceWorker::StopPoll() {
    boost::asio::post(m_strand, [this, self = shared_from_this()]() {
        m_stopped = true;
        m_poll_timer.cancel();
        });
}

void ModbusDeviceWorker::OnTimer(const boost::system::error_code& ec) {
    if (ec == boost::asio::error::operation_aborted || m_stopped) {
        status = "Stopped";
        return;
    }
    boost::asio::post(m_io_context,
        std::bind(&ModbusDeviceWorker::DoBlockingPoll, shared_from_this()));
}

void ModbusDeviceWorker::DoBlockingPoll() {
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

// --- ModbusTCPAdapter Implementations ---

ModbusTCPAdapter::ModbusTCPAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx)
    : IProtocolAdapter(name, ctx, io_ctx) {
}

ModbusTCPAdapter::~ModbusTCPAdapter() {
    std::lock_guard<std::mutex> lock(m_device_mutex);
    for (auto& [name, worker] : m_devices) {
        worker->StopPoll();
    }
    m_devices.clear();
}

std::map<std::string, std::string> ModbusTCPAdapter::GetDeviceStatuses() {
    std::map<std::string, std::string> statuses;
    std::lock_guard<std::mutex> lock(m_device_mutex);
    for (const auto& [name, worker] : m_devices) {
        statuses[name] = worker->status;
    }
    return statuses;
}

bool ModbusTCPAdapter::AddDevice(const std::string& device_name, const DeviceConfig& config) {
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

bool ModbusTCPAdapter::RemoveDevice(const std::string& device_name) {
    std::lock_guard<std::mutex> lock(m_device_mutex);
    if (!m_devices.count(device_name)) return false;
    m_devices[device_name]->StopPoll();
    m_devices.erase(device_name);
    AddLog("ModbusTCP: Removed device " + device_name);
    return true;
}

bool ModbusTCPAdapter::RestartDevice(const std::string& device_name) {
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

bool ModbusTCPAdapter::IsDeviceActive(const std::string& device_name) {
    std::lock_guard<std::mutex> lock(m_device_mutex);
    return m_devices.count(device_name);
}

void ModbusTCPAdapter::HandleCommand(const std::string& device_name, const nlohmann::json& cmd) {
    if (g_log_show_egress) AddLog("Modbus HandleCommand: " + cmd.dump(), LogType::EGRESS);
}

// --- Late Definition of ModbusDeviceWorker::OnPollComplete ---
// This function is defined last, as it needs to know about ModbusTCPAdapter methods.
void ModbusDeviceWorker::OnPollComplete(const std::string& poll_status, const nlohmann::json& j_values) {
    if (m_stopped) return;
    status = poll_status;
    if (status == "Running") {
        nlohmann::json j;
        j["deviceId"] = name;
        j["adapterName"] = m_adapter->GetName();
        j["protocol"] = m_adapter->GetProtocol();
        j["timestamp"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        j["payload"] = j_values;

        static_cast<ModbusTCPAdapter*>(m_adapter)->PushData(j.dump());
    }
    m_poll_timer.expires_after(std::chrono::seconds(1));
    m_poll_timer.async_wait(boost::asio::bind_executor(m_strand,
        std::bind(&ModbusDeviceWorker::OnTimer, shared_from_this(), std::placeholders::_1)));
}

// --- OpcuaDeviceWorker Implementations ---

OpcuaDeviceWorker::OpcuaDeviceWorker(std::string n, std::string ep, std::vector<std::string> nodes, boost::asio::io_context& io_ctx, IProtocolAdapter* adapter)
    : name(n), endpoint(ep), nodeIds(nodes), client(nullptr), is_connected(false), status("Idle"), m_stopped(false),
    m_io_context(io_ctx),
    m_strand(boost::asio::make_strand(io_ctx)),
    m_poll_timer(io_ctx),
    m_adapter(adapter)
{
}

OpcuaDeviceWorker::~OpcuaDeviceWorker() {
    if (client) {
        UA_Client_disconnect(client);
        UA_Client_delete(client);
    }
}

void OpcuaDeviceWorker::StartPoll() {
    boost::asio::post(m_strand,
        std::bind(&OpcuaDeviceWorker::OnTimer, shared_from_this(), boost::system::error_code()));
}

void OpcuaDeviceWorker::StopPoll() {
    boost::asio::post(m_strand, [this, self = shared_from_this()]() {
        m_stopped = true;
        m_poll_timer.cancel();
        });
}

void OpcuaDeviceWorker::OnTimer(const boost::system::error_code& ec) {
    if (ec || m_stopped) {
        status = "Stopped";
        return;
    }
    boost::asio::post(m_io_context,
        std::bind(&OpcuaDeviceWorker::DoBlockingPoll, shared_from_this()));
}

void OpcuaDeviceWorker::DoBlockingPoll() {
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

// --- OpcuaAdapter Implementations ---

OpcuaAdapter::OpcuaAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx)
    : IProtocolAdapter(name, ctx, io_ctx) {
}

OpcuaAdapter::~OpcuaAdapter() {
    std::lock_guard<std::mutex> lock(m_device_mutex);
    for (auto& [name, worker] : m_devices) {
        worker->StopPoll();
    }
    m_devices.clear();
}

std::map<std::string, std::string> OpcuaAdapter::GetDeviceStatuses() {
    std::map<std::string, std::string> statuses;
    std::lock_guard<std::mutex> lock(m_device_mutex);
    for (const auto& [name, worker] : m_devices) {
        statuses[name] = worker->status;
    }
    return statuses;
}

bool OpcuaAdapter::AddDevice(const std::string& device_name, const DeviceConfig& config) {
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

bool OpcuaAdapter::RemoveDevice(const std::string& device_name) {
    std::lock_guard<std::mutex> lock(m_device_mutex);
    if (!m_devices.count(device_name)) return false;
    m_devices[device_name]->StopPoll();
    m_devices.erase(device_name);
    AddLog("OPC-UA: Removed device " + device_name);
    return true;
}

bool OpcuaAdapter::RestartDevice(const std::string& device_name) {
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

bool OpcuaAdapter::IsDeviceActive(const std::string& device_name) {
    std::lock_guard<std::mutex> lock(m_device_mutex);
    return m_devices.count(device_name);
}

void OpcuaAdapter::HandleCommand(const std::string& device_name, const nlohmann::json& cmd) {
    if (g_log_show_egress) AddLog("OPC-UA HandleCommand: " + cmd.dump(), LogType::EGRESS);
}

// --- Late Definition of OpcuaDeviceWorker::OnPollComplete ---
void OpcuaDeviceWorker::OnPollComplete(const std::string& poll_status, const nlohmann::json& j_values) {
    if (m_stopped) return;
    status = poll_status;
    if (status == "Running") {
        nlohmann::json j;
        j["deviceId"] = name;
        j["adapterName"] = m_adapter->GetName();
        j["protocol"] = m_adapter->GetProtocol();
        j["timestamp"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        j["payload"] = j_values;

        static_cast<OpcuaAdapter*>(m_adapter)->PushData(j.dump());
    }
    m_poll_timer.expires_after(std::chrono::seconds(1));
    m_poll_timer.async_wait(boost::asio::bind_executor(m_strand,
        std::bind(&OpcuaDeviceWorker::OnTimer, shared_from_this(), std::placeholders::_1)));
}

// --- MqttAdapter Implementations ---

MqttAdapter::MqttAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx)
    : IProtocolAdapter(name, ctx, io_ctx) {
    m_reaper_thread = std::thread(&MqttAdapter::ReaperLoop, this);
}

MqttAdapter::~MqttAdapter() {
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

std::map<std::string, std::string> MqttAdapter::GetDeviceStatuses() {
    std::map<std::string, std::string> statuses;
    std::lock_guard<std::mutex> lock(m_device_mutex);
    for (const auto& [name, worker] : m_devices) {
        statuses[name] = worker->status;
    }
    return statuses;
}

bool MqttAdapter::AddDevice(const std::string& device_name, const DeviceConfig& config) {
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

bool MqttAdapter::RemoveDevice(const std::string& device_name) {
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
        if (worker_to_destroy->thread.joinable()) {
            worker_to_destroy->thread.join();
        }
    }
    return true;
}

bool MqttAdapter::RestartDevice(const std::string& device_name) {
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
        config["password"] = worker->password;
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

bool MqttAdapter::IsDeviceActive(const std::string& device_name) {
    std::lock_guard<std::mutex> lock(m_device_mutex);
    return m_devices.count(device_name);
}

void MqttAdapter::HandleCommand(const std::string& device_name, const nlohmann::json& cmd) {
    std::lock_guard<std::mutex> lock(m_device_mutex);
    if (m_devices.count(device_name)) {
        auto& worker = m_devices[device_name];
        std::string topic = worker->commandTopic;

        // [FIX 1] Check for empty topic and LOG ERROR (fixes "not shown on live log")
        if (topic.empty()) {
            if (g_log_show_egress) AddLog("MQTT Error: No 'commandTopic' configured for " + device_name, LogType::EGRESS);
            return;
        }

        // [FIX 2] Extract ONLY the payload content (removes "deviceID" wrapper)
        std::string payload_str;
        if (cmd.contains("payload")) {
            const auto& p = cmd["payload"];
            if (p.is_string()) payload_str = p.get<std::string>(); // Handle raw string payloads
            else payload_str = p.dump();                           // Handle JSON object payloads
        }
        else {
            payload_str = cmd.dump(); // Fallback
        }

        if (worker->client) {
            if (g_log_show_egress) AddLog("MQTT: Publishing to " + topic + ": " + payload_str, LogType::EGRESS);

            MQTTClient_message pubmsg = MQTTClient_message_initializer;
            pubmsg.payload = (void*)payload_str.c_str();
            pubmsg.payloadlen = (int)payload_str.length();
            pubmsg.qos = 1;
            pubmsg.retained = 0;
            MQTTClient_publishMessage(worker->client, topic.c_str(), &pubmsg, NULL);
        }
        else {
            if (g_log_show_egress) AddLog("MQTT Error: Client not connected for " + device_name, LogType::EGRESS);
        }
    }
}

int MqttAdapter::on_message_arrived(void* context, char* topicName, int topicLen, MQTTClient_message* message) {
    MqttDeviceWorker* worker = static_cast<MqttDeviceWorker*>(context);

    if (worker == nullptr) {
        MQTTClient_freeMessage(&message);
        MQTTClient_free(topicName);
        return 1;
    }

    std::lock_guard<std::mutex> lock(worker->client_mutex);

    if (worker->client == nullptr || worker->should_stop) {
        return 1;
    }

    std::string topic(topicName, topicLen);
    std::string payload((char*)message->payload, message->payloadlen);
    worker->status = "Msg received on " + topic;

    nlohmann::json j;
    j["deviceId"] = worker->name;
    j["adapterName"] = worker->adapter->GetName();
    j["protocol"] = worker->adapter->GetProtocol();
    j["timestamp"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    nlohmann::json parsed_payload;
    try { parsed_payload = nlohmann::json::parse(payload); }
    catch (...) { parsed_payload = payload; }

    j["payload"] = parsed_payload;

    static_cast<MqttAdapter*>(worker->adapter)->PushData(j.dump());

    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

void MqttAdapter::DevicePollLoop(MqttDeviceWorker* worker) {
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    MQTTClient_SSLOptions ssl_opts = MQTTClient_SSLOptions_initializer;
    MQTTClient_willOptions lwt_opts = MQTTClient_willOptions_initializer;

    conn_opts.keepAliveInterval = worker->keepAlive;
    conn_opts.cleansession = worker->cleanSession;
    conn_opts.connectTimeout = worker->connectTimeout;

    if (!worker->username.empty()) {
        conn_opts.username = worker->username.c_str();
        conn_opts.password = worker->password.c_str();
    }

    if (!worker->lwtTopic.empty()) {
        lwt_opts.topicName = worker->lwtTopic.c_str();
        lwt_opts.message = worker->lwtPayload.c_str();
        lwt_opts.qos = worker->lwtQos;
        lwt_opts.retained = worker->lwtRetain;
        conn_opts.will = &lwt_opts;
    }

    if (worker->useTls) {
        ssl_opts.keyStore = worker->certFile.empty() ? NULL : worker->certFile.c_str();
        ssl_opts.privateKey = worker->keyFile.empty() ? NULL : worker->keyFile.c_str();
        ssl_opts.trustStore = worker->caFile.empty() ? NULL : worker->caFile.c_str();
        ssl_opts.enableServerCertAuth = 1;
        conn_opts.ssl = &ssl_opts;
    }

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

void MqttAdapter::ReaperLoop() {
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

// --- ZmqAdapter Implementations ---

ZmqAdapter::ZmqAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx)
    : IProtocolAdapter(name, ctx, io_ctx) {
    m_reaper_thread = std::thread(&ZmqAdapter::ReaperLoop, this);
}

ZmqAdapter::~ZmqAdapter() {
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

std::map<std::string, std::string> ZmqAdapter::GetDeviceStatuses() {
    std::map<std::string, std::string> statuses;
    std::lock_guard<std::mutex> lock(m_device_mutex);
    for (const auto& [name, worker] : m_devices) {
        statuses[name] = worker->status;
    }
    return statuses;
}

bool ZmqAdapter::AddDevice(const std::string& device_name, const DeviceConfig& config) {
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

bool ZmqAdapter::RemoveDevice(const std::string& device_name) {
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

bool ZmqAdapter::RestartDevice(const std::string& device_name) {
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

bool ZmqAdapter::IsDeviceActive(const std::string& device_name) {
    std::lock_guard<std::mutex> lock(m_device_mutex);
    return m_devices.count(device_name);
}

void ZmqAdapter::HandleCommand(const std::string& device_name, const nlohmann::json& cmd) {
    if (g_log_show_egress) AddLog("ZMQ HandleCommand: " + cmd.dump(), LogType::EGRESS);

    std::lock_guard<std::mutex> lock(m_device_mutex);
    auto it = m_devices.find(device_name);
    if (it != m_devices.end()) {
        it->second->egress_queue.push(cmd.dump());
    }
}

void ZmqAdapter::PushIngressData(ZmqDeviceWorker* worker, const std::string& topic, const std::string& payload) {
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

void ZmqAdapter::SendEgressData(zmq::socket_t& socket, ZmqDeviceWorker* worker, const std::string& cmd_string) {
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
            nlohmann::json j_cmd = nlohmann::json::parse(cmd_string);
            std::string target_id = j_cmd.at("targetIdentity");
            std::string message = j_cmd.at("payload").dump();

            socket.send(zmq::const_buffer(target_id.c_str(), target_id.length()), zmq::send_flags::sndmore);
            socket.send(zmq::message_t(0), zmq::send_flags::sndmore); // Empty delimiter
            socket.send(zmq::const_buffer(message.c_str(), message.length()), zmq::send_flags::none);
        }
        catch (std::exception& e) {
            worker->status = std::string("ROUTER send error: ") + e.what();
        }
    }
}

void ZmqAdapter::DevicePollLoop(ZmqDeviceWorker* worker) {
    try {
        zmq::socket_t socket(worker->zmq_context, zmq::socket_type::sub); // default
        if (worker->pattern == "PUB") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::pub);
        else if (worker->pattern == "SUB") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::sub);
        else if (worker->pattern == "PUSH") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::push);
        else if (worker->pattern == "PULL") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::pull);
        else if (worker->pattern == "REQ") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::req);
        else if (worker->pattern == "REP") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::rep);
        else if (worker->pattern == "DEALER") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::dealer);
        else if (worker->pattern == "ROUTER") socket = zmq::socket_t(worker->zmq_context, zmq::socket_type::router);

        if (worker->pattern == "SUB") {
            socket.set(zmq::sockopt::subscribe, worker->topic);
        }
        if (worker->pattern == "DEALER" || worker->pattern == "ROUTER") {
            if (!worker->identity.empty()) {
                socket.set(zmq::sockopt::routing_id, worker->identity);
            }
        }

        socket.set(zmq::sockopt::rcvtimeo, 10);

        if (worker->bind_or_connect == "bind") {
            socket.bind(worker->endpoint);
            worker->status = "Bound to " + worker->endpoint;
        }
        else {
            socket.connect(worker->endpoint);
            worker->status = "Connected to " + worker->endpoint;
        }

        while (!worker->should_stop) {
            if (worker->pattern == "SUB" || worker->pattern == "PULL" || worker->pattern == "REP" ||
                worker->pattern == "DEALER" || worker->pattern == "ROUTER" || worker->pattern == "REQ")
            {
                if (worker->pattern != "REQ") {
                    zmq::message_t topic_msg, data_msg, id_msg, empty_msg;
                    std::optional<size_t> rcv_size;
                    if (worker->pattern == "ROUTER") rcv_size = socket.recv(id_msg, zmq::recv_flags::dontwait);
                    else rcv_size = socket.recv(topic_msg, zmq::recv_flags::dontwait);

                    if (rcv_size.has_value()) {
                        std::string topic, payload, identity;

                        if (worker->pattern == "SUB") {
                            socket.recv(data_msg, zmq::recv_flags::none);
                            topic = topic_msg.to_string();
                            payload = data_msg.to_string();
                        }
                        else if (worker->pattern == "PULL" || worker->pattern == "REP" || worker->pattern == "DEALER") {
                            topic = worker->name;
                            payload = topic_msg.to_string();
                        }
                        else if (worker->pattern == "ROUTER") {
                            socket.recv(empty_msg, zmq::recv_flags::none);
                            socket.recv(data_msg, zmq::recv_flags::none);
                            identity = id_msg.to_string();
                            topic = identity;
                            payload = data_msg.to_string();
                        }

                        PushIngressData(worker, topic, payload);

                        if (worker->pattern == "REP") {
                            auto cmd = worker->egress_queue.try_pop();
                            if (cmd.has_value()) {
                                SendEgressData(socket, worker, *cmd);
                            }
                            else {
                                socket.send(zmq::str_buffer("ACK"), zmq::send_flags::none);
                            }
                        }
                    }
                }
            }

            if (worker->pattern == "PUB" || worker->pattern == "PUSH" || worker->pattern == "REQ" ||
                worker->pattern == "DEALER" || worker->pattern == "ROUTER" || worker->pattern == "REP")
            {
                if (worker->pattern != "REP") {
                    auto cmd_str = worker->egress_queue.try_pop();
                    if (cmd_str.has_value()) {
                        SendEgressData(socket, worker, *cmd_str);

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

void ZmqAdapter::ReaperLoop() {
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

// =================================================================================
// Static Callback for Cloud Messages (WebUI -> Hub)
// =================================================================================
int CloudMsgArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message) {
    GatewayHub* hub = (GatewayHub*)context;

    if (message) {
        std::string payload((char*)message->payload, message->payloadlen);
        // Now valid because we made RouteCloudCommand public
        hub->RouteCloudCommand(payload);

        MQTTClient_freeMessage(&message);
    }
    MQTTClient_free(topicName);
    return 1;
}

// --- GatewayHub (V3 REFECTOR) ---
bool GatewayHub::_IsDeviceNameInUse_NoLock(const std::string& device_name) {
    for (auto& [adapter_name, adapter] : m_adapters) {
        // GetDeviceStatuses() has its own internal lock
        if (adapter->GetDeviceStatuses().count(device_name)) {
            return true;
        }
    }
    return false;
}

GatewayHub::~GatewayHub() {
    Stop();
}

void GatewayHub::Start() {
    m_command_bridge_running = true;
    m_proxy_running = true;
    m_aggregator_running = true;

    // [TEST] Verify Protobuf is linked correctly
    SparkplugB::Payload test_payload;
    test_payload.set_timestamp(std::time(nullptr));
    std::cout << "[SYSTEM] Protobuf Integration Check: SUCCESS. Payload Object Created." << std::endl;

    // [FIX 1] Bind the Router Socket ONCE here
    try {
        m_cmd_router_socket.bind("inproc://command_stream");
        AddLog("System: Command Router bound to inproc://command_stream");
    }
    catch (const zmq::error_t& e) {
        AddLog("System Error: Failed to bind Command Router: " + std::string(e.what()));
    }

    int pool_size = (s_worker_pool_size > 0) ? s_worker_pool_size : 4;
    for (int i = 0; i < pool_size; ++i) {
        m_thread_pool.emplace_back([this] {
            m_io_context.run();
            });
    }
    AddLog("Asio worker pool started with " + std::to_string(pool_size) + " threads.");
    PushNotification("Asio worker pool started", true);

    // [FIX 2] Actually START the Command Bridge Thread
    m_command_bridge_thread = std::thread(&GatewayHub::RunCommandBridge, this);

    m_data_proxy_thread = std::thread(&GatewayHub::RunDataProxy, this);
    m_aggregator_thread = std::thread(&GatewayHub::RunAggregator, this);

    if (m_cloud_auto_connect) {
        AddLog("System: Auto-connecting Cloud Link...", LogType::SYSTEM);
        RestartCloudLink();
    }
    else {
        AddLog("System: Cloud Link standby (Auto-Connect disabled).", LogType::SYSTEM);
    }
    AddLog("Gateway Hub V3 core threads started.");
    PushNotification("Gateway Hub started.", true);
}

void GatewayHub::Stop() {
    m_command_bridge_running = false;
    m_proxy_running = false;
    m_aggregator_running = false;
    m_cloud_stop_signal = true; // Signal cloud thread to stop

    if (m_command_bridge_thread.joinable()) m_command_bridge_thread.join();
    if (m_data_proxy_thread.joinable()) m_data_proxy_thread.join();
    if (m_aggregator_thread.joinable()) m_aggregator_thread.join();
    if (m_cloud_thread.joinable()) m_cloud_thread.join();

    m_cmd_router_socket.close();


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

void GatewayHub::RunCommandBridge() {
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

void GatewayHub::RunDataProxy() {
    AddLog("Data proxy thread started.");
    try {
        zmq::socket_t pull(m_zmq_context, zmq::socket_type::pull);
        pull.bind("inproc://data_ingress");
        zmq::socket_t pub(m_zmq_context, zmq::socket_type::pub);
        pub.bind("inproc://data_pubsub");
        pull.set(zmq::sockopt::rcvtimeo, 100);
        pull.set(zmq::sockopt::rcvhwm, g_zmq_rcvhwm);    // Limit queue to 10000 messages
        while (m_proxy_running) {
            zmq::message_t msg;
            std::optional<size_t> res = pull.recv(msg, zmq::recv_flags::none);
            // -------------------------
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

void GatewayHub::RunAggregator() {
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

            if (res.has_value()) {
                std::string s = msg.to_string();
                try {
                    nlohmann::json j = nlohmann::json::parse(s);
                    std::string deviceId = j.value("deviceId", "");
                    std::string adapterName = j.value("adapterName", "");

                    if (deviceId.empty() || adapterName.empty()) continue;

                    // Check if device is active (Simplified check for speed)
                    bool isActive = false;
                    {
                        std::lock_guard<std::mutex> lock(m_adapters_mutex);
                        if (m_adapters.count(adapterName)) {
                            isActive = m_adapters[adapterName]->IsDeviceActive(deviceId);
                        }
                    }

                    if (isActive) {
                        std::lock_guard<std::mutex> lock(m_devices_mutex);
                        DeviceData& dd = m_devices[deviceId];
                        dd.id = deviceId;
                        dd.adapter_name = adapterName;
                        dd.protocol = j.value("protocol", "Unknown");
                        dd.last_seen = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                        dd.message_count++;

                        // [CHANGED] Extract only the payload for the "Last Value" column
                        if (j.contains("payload")) {
                            dd.last_value_json = j["payload"].dump();
                        }
                        else {
                            dd.last_value_json = s; // Fallback to full message
                        }
                    }
                }
                catch (...) { /* Ignore parse errors */ }
            }
        }
    }
    catch (const zmq::error_t& e) {
        if (m_aggregator_running) AddLog("Aggregator thread exception: " + std::string(e.what()));
    }
    AddLog("Aggregator thread stopped.");
}

void GatewayHub::RestartCloudLink() {
    // 1. Stop existing thread
    m_cloud_stop_signal = true;
    if (m_cloud_thread.joinable()) {
        AddLog("System: Stopping Cloud Service...", LogType::SYSTEM);
        m_cloud_thread.join(); // This blocks until the old connection attempt times out
    }

    // 2. Log the URL being used (Verify your UI input reached here)
    AddLog("System: Starting Cloud Service -> " + m_mqtt_broker_url, LogType::SYSTEM);

    // 3. Start new thread
    m_cloud_stop_signal = false;
    m_cloud_thread = std::thread(&GatewayHub::RunCloudLink, this);
}

// =================================================================================
// RunCloudLink - The Bidirectional Bridge
// =================================================================================
void GatewayHub::RunCloudLink() {
    // Setup ZMQ Subscriber (Ingress: Device -> Hub)
    zmq::socket_t data_sub(m_zmq_context, zmq::socket_type::sub);
    data_sub.connect("inproc://data_pubsub");
    data_sub.set(zmq::sockopt::subscribe, "");

    // Setup Paho MQTT Client
    MQTTClient_create(&m_mqtt_client, "tcp://broker.emqx.io:1883", m_node_id.c_str(), MQTTCLIENT_PERSISTENCE_NONE, NULL);

    // [ADDED] Register the Callback Function
    // This tells Paho to call 'MessageArrived' when a message comes in.
    // We pass 'this' so the static function can access GatewayHub methods.
    MQTTClient_setCallbacks(m_mqtt_client, (void*)this, NULL, GatewayHub::MessageArrived, NULL);

    AddLog("Cloud: Thread Started. Waiting for connection...", LogType::SYSTEM);

    while (m_command_bridge_running && !m_cloud_stop_signal) {

        // =================================================================
        // PART A: CONNECTION MANAGEMENT
        // =================================================================
        if (!MQTTClient_isConnected(m_mqtt_client)) {
            SetCloudStatus(false);

            m_bdSeq++;
            if (m_bdSeq > 255) m_bdSeq = 0;

            MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
            conn_opts.keepAliveInterval = 20;
            conn_opts.cleansession = 1;

            // Last Will (NDEATH)
            std::string topic_ndeath = GetSparkplugTopic(SparkplugTopicType::NDEATH);
            std::string payload_ndeath = BuildDeathPayload();
            MQTTClient_willOptions will_opts = MQTTClient_willOptions_initializer;
            will_opts.topicName = topic_ndeath.c_str();
            will_opts.message = nullptr;
            will_opts.payload.len = (int)payload_ndeath.size();
            will_opts.payload.data = (void*)payload_ndeath.data();
            will_opts.qos = 0;
            will_opts.retained = 0;
            conn_opts.will = &will_opts;

            // Attempt Connection
            int rc = MQTTClient_connect(m_mqtt_client, &conn_opts);

            if (rc == MQTTCLIENT_SUCCESS) {
                SetCloudStatus(true);
                AddLog("Cloud: Connected! Sending NBIRTH...", LogType::SYSTEM);

                // [ADDED] Subscribe to Device Commands (DCMD)
                // spBv1.0/Factory_A/DCMD/HubDevice1/+
                std::string dcmd_sub = "spBv1.0/Factory_A/DCMD/" + m_node_id + "/+";
                MQTTClient_subscribe(m_mqtt_client, dcmd_sub.c_str(), 0);
                AddLog("Cloud: Subscribed to " + dcmd_sub, LogType::SYSTEM);

                // Send NBIRTH
                std::string topic_nbirth = GetSparkplugTopic(SparkplugTopicType::NBIRTH);
                std::string payload_nbirth = BuildBirthPayload();
                MQTTClient_message pubmsg = MQTTClient_message_initializer;
                pubmsg.payloadlen = (int)payload_nbirth.size();
                pubmsg.payload = (void*)payload_nbirth.data();
                pubmsg.qos = 0;
                pubmsg.retained = 0;
                MQTTClient_publishMessage(m_mqtt_client, topic_nbirth.c_str(), &pubmsg, NULL);
            }
            else {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }
        }
        // =================================================================
        // PART B: DATA LOOP
        // =================================================================

        zmq::message_t z_msg;
        // Non-blocking check so we can loop back to check connection status often
        if (data_sub.recv(z_msg, zmq::recv_flags::dontwait)) {
            std::string json_str(static_cast<char*>(z_msg.data()), z_msg.size());

            // [MODIFIED] New Payload Logic: HubID + DeviceID + RawPayload
            try {
                auto json_data = nlohmann::json::parse(json_str);
                std::string device_id = json_data.value("deviceId", "Unknown_Device");

                // 1. Create Protobuf Payload
                SparkplugB::Payload sp_payload;
                sp_payload.set_timestamp(std::time(nullptr));
                sp_payload.set_seq(m_seq++);
                if (m_seq > 255) m_seq = 0;

                // 2. ENFORCE USER REQUIREMENT: (Hub ID, Device ID, Payload)

                // Metric A: Hub ID
                auto* m_hub = sp_payload.add_metrics();
                m_hub->set_name("Meta/HubID");
                m_hub->set_datatype(SparkplugB::DataType::String);
                m_hub->set_string_value(m_node_id);

                // Metric B: Device ID
                auto* m_dev = sp_payload.add_metrics();
                m_dev->set_name("Meta/DeviceID");
                m_dev->set_datatype(SparkplugB::DataType::String);
                m_dev->set_string_value(device_id);

                // Metric C: The Actual Data Payload
                auto* m_data = sp_payload.add_metrics();
                m_data->set_name("Data/Payload");
                m_data->set_datatype(SparkplugB::DataType::String);

                // [FIX] Extract ONLY the "payload" sub-object and dump it to string
                if (json_data.contains("payload")) {
                    // .dump() converts the JSON object back to a string
                    m_data->set_string_value(json_data["payload"].dump());
                }
                else {
                    // Fallback: if the structure is weird, send the whole thing or an error
                    m_data->set_string_value("{}");
                }

                // 3. Publish to NDATA (Unified Stream)
                // Note: We Serialize to string then publish
                std::string binary_payload;
                if (sp_payload.SerializeToString(&binary_payload)) {
                    std::string topic_ndata = GetSparkplugTopic(SparkplugTopicType::NDATA);

                    MQTTClient_message pubmsg = MQTTClient_message_initializer;
                    pubmsg.payload = (void*)binary_payload.data();
                    pubmsg.payloadlen = (int)binary_payload.size();
                    pubmsg.qos = 0;
                    pubmsg.retained = 0;

                    MQTTClient_publishMessage(m_mqtt_client, topic_ndata.c_str(), &pubmsg, NULL);

                    if (g_log_show_ingress) {
                        AddLog("Cloud TX: " + std::to_string(binary_payload.size()) + " bytes (Hub:" + m_node_id + ", Dev:" + device_id + ")", LogType::INGRESS);
                    }
                }
            }
            catch (const std::exception& e) {
                AddLog("Cloud Error: Invalid JSON ingress: " + std::string(e.what()), LogType::SYSTEM);
            }
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Cleanup
    MQTTClient_disconnect(m_mqtt_client, 10000);
    MQTTClient_destroy(&m_mqtt_client);
}

// =================================================================================
// RouteCloudCommand: ZMQ Router Logic
// =================================================================================
void GatewayHub::RouteCloudCommand(const std::string& payload) {
    // 1. Parse
    nlohmann::json json_cmd;
    try {
        json_cmd = nlohmann::json::parse(payload, nullptr, false);
    }
    catch (...) { return; }

    if (json_cmd.is_discarded()) {
        AddLog("Cmd Error: Invalid JSON", LogType::EGRESS);
        return;
    }

    // 2. Extract Device ID
    std::string device_id = json_cmd.value("deviceID", "");
    if (device_id.empty()) {
        AddLog("Cmd Error: Missing deviceID", LogType::EGRESS);
        return;
    }

    // 3. Find Adapter (Thread-safe map access)
    std::string adapter_name;
    bool adapter_found = false;
    {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        for (auto const& [name, adapter] : m_adapters) {
            if (adapter->IsDeviceActive(device_id)) {
                adapter_name = name;
                adapter_found = true;
                break;
            }
        }
    }

    if (adapter_found) {
        // 4. Construct Internal Command (same format as SendTestCommand)
        nlohmann::json cmd;
        cmd["targetAdapter"] = adapter_name;
        cmd["targetDevice"] = device_id;

        // Handle payload wrapping
        if (json_cmd.contains("payload")) {
            cmd["payload"] = json_cmd["payload"];
        }
        else {
            cmd["payload"] = json_cmd;
        }

        // [FIX 5] Push to Queue
        {
            std::lock_guard<std::mutex> lock(m_command_queue_mutex);
            m_command_queue.push(cmd);
        }

        if (g_log_show_egress) AddLog("Cloud: Queued cmd for " + device_id, LogType::EGRESS);
    }
    else {
        AddLog("Cmd Error: Device '" + device_id + "' not found", LogType::EGRESS);
    }
}

void GatewayHub::_UpdateDeviceMap(const std::string& json_data) {
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

void GatewayHub::ClearLogs() {
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        g_logs.clear();
    }
    AddLog("Log cleared.");
}

bool GatewayHub::AddAdapter(const std::string& name, const std::string& protocol_type) {
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

bool GatewayHub::RemoveAdapter(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_adapters_mutex);
    if (!m_adapters.count(name)) return false;
    m_adapters[name]->Stop();
    m_adapters[name]->Join();
    m_adapters.erase(name);
    PushNotification("Adapter '" + name + "' removed.", true);
    AddLog("Adapter '" + name + "' removed.");
    return true;
}

std::map<std::string, std::string> GatewayHub::GetAdapterStatuses() {
    std::lock_guard<std::mutex> lock(m_adapters_mutex);
    std::map<std::string, std::string> statuses;
    for (const auto& [name, adapter] : m_adapters) {
        statuses[name] = adapter->GetStatus();
    }
    return statuses;
}

std::string GatewayHub::GetAdapterProtocol(const std::string& adapter_name) {
    std::lock_guard<std::mutex> lock(m_adapters_mutex);
    if (m_adapters.count(adapter_name)) {
        return m_adapters[adapter_name]->GetProtocol();
    }
    return "Unknown";
}

bool GatewayHub::AddDeviceToAdapter(const std::string& adapter_name, const std::string& device_name, const DeviceConfig& config) {
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

bool GatewayHub::RemoveDeviceFromAdapter(const std::string& adapter_name, const std::string& device_name) {
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

bool GatewayHub::RestartDevice(const std::string& adapter_name, const std::string& device_name) {
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

std::map<std::string, std::string> GatewayHub::GetDeviceStatusesForAdapter(const std::string& adapter_name) {
    std::lock_guard<std::mutex> lock(m_adapters_mutex);
    if (m_adapters.count(adapter_name)) {
        return m_adapters[adapter_name]->GetDeviceStatuses();
    }
    return {};
}

// =================================================================================
// SendTestCommand: Bypasses ZMQ, Uses Direct Object Access
// =================================================================================
void GatewayHub::SendTestCommand(const std::string& device_id, const std::string& payload_json) {
    // 1. Find Adapter for this Device
    std::string adapter_name;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        for (auto const& [name, adapter] : m_adapters) {
            if (adapter->IsDeviceActive(device_id)) {
                adapter_name = name;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        PushNotification("Error: Device '" + device_id + "' not found", false);
        return;
    }

    // 2. Construct Command
    try {
        nlohmann::json cmd;
        cmd["targetAdapter"] = adapter_name;
        cmd["targetDevice"] = device_id;

        // Parse user payload
        nlohmann::json user_payload = nlohmann::json::parse(payload_json);
        cmd["payload"] = user_payload;

        std::lock_guard<std::mutex> lock(m_command_queue_mutex);
        m_command_queue.push(cmd);
        PushNotification("Test command queued for " + device_id, true);
    }
    catch (const std::exception& e) {
        PushNotification("Invalid JSON: " + std::string(e.what()), false);
    }
}

// =================================================================================
// Sparkplug B Topic Generator
// =================================================================================
std::string GatewayHub::GetSparkplugTopic(SparkplugTopicType type, const std::string& device_id) {
    std::string msg_type;

    switch (type) {
    case SparkplugTopicType::NBIRTH: msg_type = "NBIRTH"; break;
    case SparkplugTopicType::NDEATH: msg_type = "NDEATH"; break;
    case SparkplugTopicType::NDATA:  msg_type = "NDATA";  break;
    case SparkplugTopicType::NCMD:   msg_type = "NCMD";   break;
    case SparkplugTopicType::DBIRTH: msg_type = "DBIRTH"; break;
    case SparkplugTopicType::DDEATH: msg_type = "DDEATH"; break;
    case SparkplugTopicType::DDATA:  msg_type = "DDATA";  break;
    case SparkplugTopicType::DCMD:   msg_type = "DCMD";   break;
    default: msg_type = "NDATA"; break;
    }

    // Structure: spBv1.0 / Group_ID / Message_Type / Edge_Node_ID / [Device_ID]
    std::stringstream ss;
    ss << SP_VERSION << "/" << SP_GROUP_ID << "/" << msg_type << "/" << m_node_id;

    // If it is a Device message (starts with 'D'), append the Device ID
    if (!device_id.empty() && (msg_type[0] == 'D')) {
        ss << "/" << device_id;
    }

    return ss.str();
}

// =================================================================================
// Sparkplug B Payload Builders
// =================================================================================
std::string GatewayHub::BuildDeathPayload() {
    SparkplugB::Payload payload;
    payload.set_timestamp(std::time(nullptr));

    // Metric: bdSeq (Mandatory for NDEATH)
    auto* metric = payload.add_metrics();
    metric->set_name("bdSeq");
    metric->set_datatype(SparkplugB::DataType::UInt64);
    metric->set_long_value(m_bdSeq);

    // Serialize to binary string
    std::string output;
    payload.SerializeToString(&output);
    return output;
}

std::string GatewayHub::BuildBirthPayload() {
    SparkplugB::Payload payload;
    payload.set_timestamp(std::time(nullptr));
    payload.set_uuid(m_node_id);

    // 1. Mandatory Metric: bdSeq (Session Tracker)
    auto* m_seq = payload.add_metrics();
    m_seq->set_name("bdSeq");
    m_seq->set_datatype(SparkplugB::DataType::UInt64);
    m_seq->set_long_value(m_bdSeq);

    // 2. Mandatory Metric: Node Control/Rebirth
    auto* m_rebirth = payload.add_metrics();
    m_rebirth->set_name("Node Control/Rebirth");
    m_rebirth->set_datatype(SparkplugB::DataType::Boolean);
    m_rebirth->set_boolean_value(false);

    // 3. System Metrics
    auto* m_status = payload.add_metrics();
    m_status->set_name("System/Status");
    m_status->set_datatype(SparkplugB::DataType::String);
    m_status->set_string_value("ONLINE");

    // 4. REAL SCENARIO: Iterate through known devices to register them
    // We lock the mutex to safely read the device map
    {
        std::lock_guard<std::mutex> lock(m_devices_mutex);
        for (const auto& [device_id, device_data] : m_devices) {
            // Register a metric for this device's connection status
            auto* m_dev = payload.add_metrics();
            m_dev->set_name("Devices/" + device_id + "/Status");
            m_dev->set_datatype(SparkplugB::DataType::String);
            m_dev->set_string_value("CONNECTED");

            // Note: In Phase 4, you can also loop through specific tags here
            // e.g., "Devices/" + device_id + "/Temperature"
        }
    }

    std::string output;
    payload.SerializeToString(&output);
    return output;
}

// =================================================================================
// Static MQTT Callback (Must match Paho MQTT signature)
// =================================================================================
int GatewayHub::MessageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message) {
    GatewayHub* hub = (GatewayHub*)context;
    std::string topic(topicName);

    // 1. Parse Topic to find Target Device ID
    // Format: spBv1.0 / Group / DCMD / EdgeNodeID / DeviceID
    std::vector<std::string> tokens;
    std::stringstream ss(topic);
    std::string segment;
    while (std::getline(ss, segment, '/')) {
        tokens.push_back(segment);
    }

    // Check if this is a Device Command (DCMD) targeting a specific device
    // tokens[0]=ver, [1]=Group, [2]=MsgType, [3]=Node, [4]=Device
    if (tokens.size() >= 5 && tokens[2] == "DCMD") {
        std::string targetDeviceID = tokens[4];

        // 2. Parse Sparkplug B Protobuf Payload to get the JSON command string
        // Note: Ensure you have the correct namespace for your generated proto files.
        // It is usually org::eclipse::tahu::protobuf::Payload or SparkplugB::Payload
        SparkplugB::Payload sp_payload;
        bool parsed = sp_payload.ParseFromArray(message->payload, message->payloadlen);

        if (parsed) {
            std::string commandJson = "";

            // Iterate metrics to find the command string (usually in "Data/Payload" or similar)
            for (int i = 0; i < sp_payload.metrics_size(); i++) {
                const auto& metric = sp_payload.metrics(i);
                if (metric.datatype() == SparkplugB::DataType::String) {
                    commandJson = metric.string_value();
                    break; // Use the first string metric as the command
                }
            }

            if (!commandJson.empty()) {
                AddLog("Cloud RX: Command for " + targetDeviceID + " -> " + commandJson, LogType::EGRESS);

                // 3. Route to internal Adapter (Reusing your existing logic)
                hub->SendTestCommand(targetDeviceID, commandJson);
            }
        }
        else {
            AddLog("Cloud RX: Failed to parse DCMD Protobuf.", LogType::SYSTEM);
        }
    }

    // Cleanup Paho message resources
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1; // Return 1 to indicate success
}
