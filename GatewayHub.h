// GatewayHub.h
#pragma once

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

// --- uWebSockets ---
#include <uwebsockets/App.h>
#include <uwebsockets/WebSocket.h>

// --- Boost.Asio ---
// (We only need the main header for post, strands, and timers)
#include <boost/asio.hpp>

// --- Dear ImGui Includes ---
#include <GL/glew.h>
#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

enum class LogType {
    SYSTEM,  // Default, always show
    INGRESS, // Device -> WebUI
    EGRESS   // WebUI -> Device
};

void AddLog(const std::string& msg, LogType type = LogType::SYSTEM); 
extern unsigned int s_hardware_cores;
extern int s_worker_pool_size;
extern int s_ws_port;
extern std::string s_ws_host;

// --- Global Logging & Notification System ---
extern std::mutex g_log_mutex;
extern std::vector<std::string> g_logs;
extern bool g_log_show_ingress; // Show (Device -> WebUI)
extern bool g_log_show_egress;  // Show (WebUI -> Device)
extern const size_t g_log_max_lines; // Max log lines
extern const int g_zmq_rcvhwm;        // Max queued ZMQ messages

struct Notification {
    std::string message;
    double expiry_time;
    bool is_success;
};

extern std::vector<Notification> g_notifications;
extern std::mutex g_notification_mutex;
// --- Helper Types ---
using DeviceConfig = std::map<std::string, std::string>;

// --- IProtocolAdapter Interface ---
class IProtocolAdapter {
public:
    // --- Constructor (Declaration) ---
    IProtocolAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx);

    // --- Destructor (Declaration) ---
    virtual ~IProtocolAdapter();

    // --- Public Methods (Declarations) ---
    void Start();
    void Stop();
    void Join();

    // --- Virtual Methods ---
    virtual void Run(); // Default implementation is in the .cpp

    // --- Pure Virtual Methods (Interface) ---
    virtual bool AddDevice(const std::string& device_name, const DeviceConfig& config) = 0;
    virtual bool RemoveDevice(const std::string& device_name) = 0;
    virtual bool RestartDevice(const std::string& device_name) = 0;
    virtual bool IsDeviceActive(const std::string& device_name) = 0;
    virtual std::map<std::string, std::string> GetDeviceStatuses() = 0;
    virtual std::string GetProtocol() = 0;

    // --- Inline Virtual Getters (Implementation in header) ---
    virtual std::string GetName() const { return m_name; }
    virtual std::string GetStatus() { return "Running"; }

protected:
    // --- Pure Virtual Handler ---
    virtual void HandleCommand(const std::string& device_name, const nlohmann::json& cmd) = 0;

    // --- Protected Methods (Declaration) ---
    void PushData(const std::string& json_payload);

    // --- Member Variables ---
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
class ModbusTCPAdapter; // --- Forward declaration is needed ---

// --- ModbusDeviceWorker Class Definition ---
class ModbusDeviceWorker : public std::enable_shared_from_this<ModbusDeviceWorker> {
public:
    // --- Member Variables ---
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

    // --- Constructor & Destructor (Declarations) ---
    ModbusDeviceWorker(std::string n, std::string i, int p, boost::asio::io_context& io_ctx, IProtocolAdapter* adapter);
    ~ModbusDeviceWorker();

    // --- Public Methods (Declarations) ---
    void StartPoll();
    void StopPoll();

private:
    // --- Private Methods (Declarations) ---
    void OnTimer(const boost::system::error_code& ec);
    void DoBlockingPoll();

    // This function's implementation is provided later, after ModbusTCPAdapter is defined
    void OnPollComplete(const std::string& poll_status, const nlohmann::json& j_values);
};

// --- ModbusTCPAdapter Class Definition ---
class ModbusTCPAdapter : public IProtocolAdapter {
public:
    // --- Constructor & Destructor (Declarations) ---
    ModbusTCPAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx);
    ~ModbusTCPAdapter();

    // --- Overridden Methods (Declarations & Inlines) ---
    std::string GetProtocol() override { return "ModbusTCP"; }
    std::map<std::string, std::string> GetDeviceStatuses() override;
    bool AddDevice(const std::string& device_name, const DeviceConfig& config) override;
    bool RemoveDevice(const std::string& device_name) override;
    bool RestartDevice(const std::string& device_name) override;
    bool IsDeviceActive(const std::string& device_name) override;

    // --- Friend Declaration ---
    friend class ModbusDeviceWorker;

protected:
    // --- Protected Methods (Declarations) ---
    void HandleCommand(const std::string& device_name, const nlohmann::json& cmd) override;

    // --- Member Variables ---
    std::map<std::string, std::shared_ptr<ModbusDeviceWorker>> m_devices;
    std::mutex m_device_mutex;
};

// --- OpcuaAdapter ---
class OpcuaAdapter; // --- Forward declaration is needed ---

// --- OpcuaDeviceWorker Class Definition ---
class OpcuaDeviceWorker : public std::enable_shared_from_this<OpcuaDeviceWorker> {
public:
    // --- Member Variables ---
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

    // --- Constructor & Destructor (Declarations) ---
    OpcuaDeviceWorker(std::string n, std::string ep, std::vector<std::string> nodes, boost::asio::io_context& io_ctx, IProtocolAdapter* adapter);
    ~OpcuaDeviceWorker();

    // --- Public Methods (Declarations) ---
    void StartPoll();
    void StopPoll();

private:
    // --- Private Methods (Declarations) ---
    void OnTimer(const boost::system::error_code& ec);
    void DoBlockingPoll();

    // This function's implementation is provided later
    void OnPollComplete(const std::string& poll_status, const nlohmann::json& j_values);
};

// --- OpcuaAdapter Class Definition ---
class OpcuaAdapter : public IProtocolAdapter {
public:
    // --- Constructor & Destructor (Declarations) ---
    OpcuaAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx);
    ~OpcuaAdapter();

    // --- Overridden Methods (Declarations & Inlines) ---
    std::string GetProtocol() override { return "OPC-UA"; }
    std::map<std::string, std::string> GetDeviceStatuses() override;
    bool AddDevice(const std::string& device_name, const DeviceConfig& config) override;
    bool RemoveDevice(const std::string& device_name) override;
    bool RestartDevice(const std::string& device_name) override;
    bool IsDeviceActive(const std::string& device_name) override;

    // --- Friend Declaration ---
    friend class OpcuaDeviceWorker;

protected:
    // --- Protected Methods (Declarations) ---
    void HandleCommand(const std::string& device_name, const nlohmann::json& cmd) override;

    // --- Member Variables ---
    std::map<std::string, std::shared_ptr<OpcuaDeviceWorker>> m_devices;
    std::mutex m_device_mutex;
};

// --- MqttDeviceWorker Struct Definition ---
// This full struct definition is needed by the MqttAdapter class
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

    // --- Constructor (Inline) ---
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

// --- MqttAdapter Class Definition ---
class MqttAdapter : public IProtocolAdapter {
public:
    // --- Constructor & Destructor (Declarations) ---
    MqttAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx);
    ~MqttAdapter();

    // --- Overridden Methods (Declarations & Inlines) ---
    std::string GetProtocol() override { return "MQTT"; }
    std::map<std::string, std::string> GetDeviceStatuses() override;
    bool AddDevice(const std::string& device_name, const DeviceConfig& config) override;
    bool RemoveDevice(const std::string& device_name) override;
    bool RestartDevice(const std::string& device_name) override;
    bool IsDeviceActive(const std::string& device_name) override;

protected:
    // --- Protected Methods (Declarations) ---
    void HandleCommand(const std::string& device_name, const nlohmann::json& cmd) override;

    // --- Static Callback ---
    static int on_message_arrived(void* context, char* topicName, int topicLen, MQTTClient_message* message);

    // --- Private Thread Loops (Declarations) ---
    void DevicePollLoop(MqttDeviceWorker* worker);
    void ReaperLoop();

    // --- Member Variables ---
    std::thread m_reaper_thread;
    std::mutex m_reaper_mutex;
    std::vector<std::unique_ptr<MqttDeviceWorker>> m_reaper_queue;
    std::map<std::string, std::unique_ptr<MqttDeviceWorker>> m_devices;
    std::mutex m_device_mutex;
};

// --- ZmqAdapter ---
// --- ThreadSafeQueue Class Definition ---
// This is a template, so its full definition must remain in the header.
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
    std::condition_variable m_cond;
};


// --- ZmqDeviceWorker Struct Definition ---
// This full struct definition is needed by the ZmqAdapter class
struct ZmqDeviceWorker {
    std::string name;
    std::string endpoint;
    std::string topic;
    std::string pattern;
    std::string bind_or_connect;
    std::string identity;

    std::thread thread;
    std::atomic<bool> should_stop;
    std::string status;
    IProtocolAdapter* adapter;
    zmq::context_t& zmq_context;

    ThreadSafeQueue<std::string> egress_queue;

    // --- Constructor (Inline) ---
    ZmqDeviceWorker(std::string n, std::string ep, std::string t, std::string p, std::string boc, std::string id, IProtocolAdapter* parent, zmq::context_t& ctx)
        : name(n), endpoint(ep), topic(t), pattern(p), bind_or_connect(boc), identity(id),
        should_stop(false), status("Idle"), adapter(parent), zmq_context(ctx) {
    }
};

// --- ZmqAdapter Class Definition ---
class ZmqAdapter : public IProtocolAdapter {
public:
    // --- Constructor & Destructor (Declarations) ---
    ZmqAdapter(const std::string& name, zmq::context_t& ctx, boost::asio::io_context& io_ctx);
    ~ZmqAdapter();

    // --- Overridden Methods (Declarations & Inlines) ---
    std::string GetProtocol() override { return "ZMQ"; }
    std::map<std::string, std::string> GetDeviceStatuses() override;
    bool AddDevice(const std::string& device_name, const DeviceConfig& config) override;
    bool RemoveDevice(const std::string& device_name) override;
    bool RestartDevice(const std::string& device_name) override;
    bool IsDeviceActive(const std::string& device_name) override;

protected:
    // --- Protected Methods (Declarations) ---
    void HandleCommand(const std::string& device_name, const nlohmann::json& cmd) override;

    // --- Private Helper/Thread Methods (Declarations) ---
    void PushIngressData(ZmqDeviceWorker* worker, const std::string& topic, const std::string& payload);
    void SendEgressData(zmq::socket_t& socket, ZmqDeviceWorker* worker, const std::string& cmd_string);
    void DevicePollLoop(ZmqDeviceWorker* worker);
    void ReaperLoop();

    // --- Member Variables ---
    std::thread m_reaper_thread;
    std::mutex m_reaper_mutex;
    std::vector<std::unique_ptr<ZmqDeviceWorker>> m_reaper_queue;
    std::map<std::string, std::unique_ptr<ZmqDeviceWorker>> m_devices;
    std::mutex m_device_mutex;
};

// --- Struct Definitions ---
// This struct is required by the GetDeviceData and _UpdateDeviceMap methods
struct DeviceData {
    std::string id;
    std::string adapter_name;
    std::string protocol;
    time_t last_seen = 0;
    std::string last_value_json;
    int message_count = 0;
};

// --- Class Definition ---
class GatewayHub {
private:
    // --- Member Variables ---
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

    // --- Private Method Declarations ---
    bool _IsDeviceNameInUse_NoLock(const std::string& device_name);
    void RunCommandBridge();
    void RunDataProxy();
    void RunAggregator();
    void RunUwsSubscriber();
    void RunUwsServer();
    void DeferPublishData();
    void _UpdateDeviceMap(const std::string& json_data);

public:
    // --- Constructor & Destructor ---
    // Constructor is left inline
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

    ~GatewayHub(); // Implementation is in the .cpp file

    // --- Public Method Declarations ---
    void Start();
    void Stop();

    void StartUwsServer();

    // Simple getters are left inline
    bool GetUwsStatus() const {
        return m_uws_running;
    }

    bool HasAdapters() const {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        return !m_adapters.empty();
    }

    void GetDeviceData(std::map<std::string, DeviceData>& devices) {
        std::lock_guard<std::mutex> lock(m_devices_mutex);
        devices = m_devices;
    }

    void GetLogs(std::vector<std::string>& logs) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        logs = g_logs;
    }

    void ClearLogs();

    bool AddAdapter(const std::string& name, const std::string& protocol_type);
    bool RemoveAdapter(const std::string& name);
    std::map<std::string, std::string> GetAdapterStatuses();
    std::string GetAdapterProtocol(const std::string& adapter_name);

    bool AddDeviceToAdapter(const std::string& adapter_name, const std::string& device_name, const DeviceConfig& config);
    bool RemoveDeviceFromAdapter(const std::string& adapter_name, const std::string& device_name);
    bool RestartDevice(const std::string& adapter_name, const std::string& device_name);
    std::map<std::string, std::string> GetDeviceStatusesForAdapter(const std::string& adapter_name);

    void SendTestCommand(const std::string& adapter_name, const std::string& payload_json);
};

