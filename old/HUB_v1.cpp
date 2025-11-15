/*
 * Multiprotocol Gateway Hub (Dear ImGui Application) - REDESIGNED
 *
 * Architecture (One-to-Many):
 * 1. Main Thread: Runs Dear ImGui loop.
 * 2. GatewayHub Class: Central manager for adapter services.
 * 3. ZMQ Internal Bus (Thread-Safe):
 * - Data: PUSH/PULL ("inproc://data_stream") for adapter -> bridge.
 * 4. Core Background Threads:
 * - uWS Thread: Runs the uS::Loop for the WebSocket server.
 * - ZMQ Bridge Thread: The core. PULLs data and updates state.
 * 5. Dynamic Adapters (IProtocolAdapter):
 * - An abstract interface `IProtocolAdapter` defines a *service manager*.
 * - Concrete classes (e.g., `ModbusTCPAdapter`) manage their own list of devices.
 * - Each adapter manages its own worker threads (one per device).
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
#include <iomanip> // For std::put_time
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

// --- Port Configuration ---
const int WS_PORT = 9002;

// --- Per-Socket Data for uWS ---
struct PerSocketData {};
using WebSocket = uWS::WebSocket<false, true, PerSocketData>;

// --- ZMQ Bus Addresses ---
const std::string DATA_STREAM_BUS = "inproc://data_stream";
const std::string COMMAND_STREAM_BUS = "inproc://command_stream";

// =================================================================================
//
// Global Data Structures
//
// =================================================================================

/**
 * @brief Represents the latest state of a discovered device/tag.
 */
struct DeviceData {
    std::string id;
    std::string protocol;
    std::string adapter_name;
    std::string last_value_json;
    time_t last_seen = 0;
    int message_count = 0;
};

/**
 * @brief Structure for commands routed from the UI/WebSocket to a specific adapter.
 */
struct CommandToRoute {
    std::string adapter_name;
    std::string payload_json;
};

/**
 * @brief Flexible configuration map for adding new devices to an adapter.
 */
using DeviceConfig = std::map<std::string, std::string>;


// --- Forward Declaration ---
class GatewayHub;

// =================================================================================
//
// IProtocolAdapter INTERFACE (REDESIGNED)
//
// =================================================================================

class IProtocolAdapter {
public:
    virtual ~IProtocolAdapter() {
        Stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    // Start/Stop the adapter's *manager* thread
    void Start() {
        if (m_running.load()) return;
        m_running = true;
        m_thread = std::thread(&IProtocolAdapter::Run, this);
    }

    void Stop() {
        m_running = false;
    }

    bool Join() {
        if (m_thread.joinable()) {
            m_thread.join();
            return true;
        }
        return false;
    }

    /**
     * @brief The main loop for the *adapter manager*.
     * Its job is to manage its own list of device workers/threads.
     */
    virtual void Run() = 0;

    // --- New Device Management Methods ---

    /**
     * @brief Adds a new device to this adapter's management list.
     * @param device_name A unique name for this device (e.g., "plc_line_1").
     * @param config A map of settings (e.g., {"ip": "1.2.3.4", "port": "502"}).
     * @return True on success, false on failure.
     */
    virtual bool AddDevice(const std::string& device_name, const DeviceConfig& config) = 0;

    /**
     * @brief Removes a device from this adapter's management list.
     * @param device_name The unique name of the device to remove.
     * @return True on success, false on failure.
     */
    virtual bool RemoveDevice(const std::string& device_name) = 0;

    /**
     * @brief Gets the status of all devices managed by this adapter.
     * @return A map of {device_name, status_string}.
     */
    virtual std::map<std::string, std::string> GetDeviceStatuses() const = 0;


    // --- Getters ---
    virtual std::string GetName() const = 0;     // Name of the adapter (e.g., "ModbusService1")
    virtual std::string GetProtocol() const = 0; // Protocol (e.g., "ModbusTCP")

    /**
     * @brief Gets the overall status of the adapter service itself.
     * (e.g., "Running, 5 devices")
     */
    virtual std::string GetStatus() const = 0;

    bool IsRunning() const { return m_running.load(); }

protected:
    IProtocolAdapter(std::string name, GatewayHub* hub, zmq::context_t& zmq_ctx)
        : m_name(name), m_hub(hub), m_zmq_context(zmq_ctx), m_running(false) {
    }

    void Log(const std::string& msg); // Implemented after GatewayHub

    std::string m_name;
    GatewayHub* m_hub;
    zmq::context_t& m_zmq_context;
    std::thread m_thread;
    std::atomic<bool> m_running;
};


// =================================================================================
//
// GatewayHub CLASS (REDESIGNED)
//
// =================================================================================

// Forward-declare concrete adapters so GatewayHub can be defined first
class ModbusTCPAdapter;
class OpcuaAdapter;
class MqttAdapter;
class ZmqAdapter;


class GatewayHub {
public:
    GatewayHub() : m_running(true), m_zmq_context(2) {}

    ~GatewayHub() {
        Stop();
    }

    void Start() {
        m_running = true;
        AddLog("GatewayHub starting...");

        // Start ZMQ Bridge Thread
        m_zmq_bridge_thread = std::thread(&GatewayHub::RunZmqBridge, this);

        // Start uWS Server Thread
        m_uws_thread = std::thread(&GatewayHub::RunUwsServer, this);

        // Wait for uWS to be ready
        {
            std::unique_lock<std::mutex> lock(m_uws_mutex);
            m_uws_cv.wait(lock, [this] { return m_uws_loop != nullptr; });
        }
        AddLog("uWebSockets server loop initialized.");
        AddLog("GatewayHub running. Add adapters via the UI.");
    }

    void Stop() {
        if (!m_running.exchange(false)) {
            return; // Already stopped
        }

        AddLog("GatewayHub stopping...");

        // Stop all adapters
        {
            std::lock_guard<std::mutex> lock(m_adapters_mutex);
            for (auto& [name, adapter] : m_adapters) {
                adapter->Stop();
            }
            m_adapters.clear(); // Destructors will join threads
        }
        AddLog("All adapters stopped.");

        // Shut down ZMQ context, which unblocks all ZMQ socket polls
        m_zmq_context.shutdown();

        // Wake up ZMQ bridge thread
        {
            std::lock_guard<std::mutex> lock(m_command_queue_mutex);
            m_command_queue.push({}); // Push empty command to wake the waiting CV
        }
        m_command_queue_cv.notify_one();

        // Wake up uWS thread and tell it to stop using defer
        DeferStopLoop();

        // Join core threads
        if (m_zmq_bridge_thread.joinable()) m_zmq_bridge_thread.join();
        if (m_uws_thread.joinable()) m_uws_thread.join();

        AddLog("All threads joined. GatewayHub stopped.");
    }

    // --- Notification System ---
    void PushNotification(const std::string& message, bool is_success) {
        std::lock_guard<std::mutex> lock(m_notifications_mutex);
        m_notifications.push({ message, is_success });
        if (m_notifications.size() > 5) { // Limit queue size
            m_notifications.pop();
        }
    }

    std::queue<std::pair<std::string, bool>> PopAllNotifications() {
        std::lock_guard<std::mutex> lock(m_notifications_mutex);
        std::queue<std::pair<std::string, bool>> temp_queue;
        std::swap(temp_queue, m_notifications);
        return temp_queue;
    }

    // --- Thread-Safe Log Functions (for ImGui & Adapters) ---
    void AddLog(const std::string& msg) {
        std::lock_guard<std::mutex> lock(m_log_mutex);
        std::string log_entry = "[" + GetTimestamp() + "] " + msg;
        std::cout << log_entry << std::endl; // Also log to console
        m_logs.push_back(log_entry);
        if (m_logs.size() > 200) {
            m_logs.pop_front();
        }
    }

    void GetLogs(std::vector<std::string>& out_logs) {
        std::lock_guard<std::mutex> lock(m_log_mutex);
        out_logs.clear();
        for (const auto& log : m_logs) {
            out_logs.push_back(log);
        }
    }

    // --- Thread-Safe Device Status (for ImGui) ---
    void GetDeviceData(std::map<std::string, DeviceData>& out_devices) {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        out_devices = m_devices; // Return a copy
    }

    // --- Thread-Safe Command Injection (for ImGui/uWS) ---
    void SendTestCommand(const std::string& adapter_name, const std::string& payload_json) {
        AddLog("Queueing command from ImGui for adapter: " + adapter_name);
        {
            std::lock_guard<std::mutex> lock(m_command_queue_mutex);
            m_command_queue.push({ adapter_name, payload_json });
        }
        m_command_queue_cv.notify_one(); // Notify ZMQ bridge thread
    }

    // --- Adapter Management (REDESIGNED) ---

    /**
     * @brief Adds a new adapter service (e.g., "ModbusService1")
     * IMPLEMENTATION MUST BE *AFTER* CONCRETE ADAPTERS ARE DEFINED
     */
    bool AddAdapter(const std::string& name, const std::string& protocol_type);

    /**
     * @brief Removes an adapter service. The adapter's destructor will stop all its devices.
     */
    bool RemoveAdapter(const std::string& name) {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        if (m_adapters.count(name)) {
            // 1. Erasing the shared_ptr triggers its destructor.
            // 2. The ~IProtocolAdapter() destructor calls Stop().
            // 3. The ModbusTCPAdapter::Run() loop will exit, stopping/joining all worker threads.
            m_adapters.erase(name);

            // 4. Clean up devices from the *hub's* device map
            std::lock_guard<std::mutex> device_lock(m_device_mutex);
            for (auto it = m_devices.begin(); it != m_devices.end(); ) {
                if (it->second.adapter_name == name) {
                    it = m_devices.erase(it);
                }
                else {
                    ++it;
                }
            }

            PushNotification("Adapter service '" + name + "' removed.", true);
            return true;
        }
        PushNotification("Error: Adapter '" + name + "' not found.", false);
        return false;
    }

    // --- NEW Device-to-Adapter Management ---

    bool AddDeviceToAdapter(const std::string& adapter_name, const std::string& device_name, const DeviceConfig& config) {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        if (m_adapters.count(adapter_name)) {
            if (m_adapters[adapter_name]->AddDevice(device_name, config)) {
                PushNotification("Device '" + device_name + "' added to " + adapter_name, true);
                return true;
            }
            PushNotification("Error adding device '" + device_name + "'", false);
            return false;
        }
        PushNotification("Error: Adapter '" + adapter_name + "' not found.", false);
        return false;
    }

    bool RemoveDeviceFromAdapter(const std::string& adapter_name, const std::string& device_name) {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        if (m_adapters.count(adapter_name)) {
            if (m_adapters[adapter_name]->RemoveDevice(device_name)) {

                // Clean up this specific device from the hub's device map
                // Note: This logic assumes deviceId starts with device_name
                std::lock_guard<std::mutex> device_lock(m_device_mutex);
                for (auto it = m_devices.begin(); it != m_devices.end(); ) {
                    if (it->second.adapter_name == adapter_name && it->first.rfind(device_name, 0) == 0) {
                        it = m_devices.erase(it);
                    }
                    else {
                        ++it;
                    }
                }

                PushNotification("Device '" + device_name + "' removed from " + adapter_name, true);
                return true;
            }
            PushNotification("Error removing device '" + device_name + "'", false);
            return false;
        }
        PushNotification("Error: Adapter '" + adapter_name + "' not found.", false);
        return false;
    }

    // --- NEW Status Getters ---

    std::map<std::string, std::string> GetAdapterStatuses() {
        std::map<std::string, std::string> statuses;
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        for (auto& [name, adapter] : m_adapters) {
            statuses[name] = adapter->GetStatus();
        }
        return statuses;
    }

    // Gets protocol type for the UI
    std::string GetAdapterProtocol(const std::string& adapter_name) {
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        if (m_adapters.count(adapter_name)) {
            return m_adapters[adapter_name]->GetProtocol();
        }
        return "Unknown";
    }

    std::map<std::string, std::string> GetDeviceStatusesForAdapter(const std::string& adapter_name) {
        std::map<std::string, std::string> statuses;
        std::lock_guard<std::mutex> lock(m_adapters_mutex);
        if (m_adapters.count(adapter_name)) {
            statuses = m_adapters[adapter_name]->GetDeviceStatuses();
        }
        return statuses;
    }

    zmq::context_t& GetZmqContext() { return m_zmq_context; }

    // --- Thread-safe mechanism to wake up the uWS thread for publishing data ---
// --- Thread-safe mechanism to wake up the uWS thread for publishing data ---
    void DeferPublishData() {
        // Check m_uws_loop *before* deferring
        if (m_uws_loop) {
            m_uws_loop->defer([this]() {
                // This code runs on the uWS thread

                // Check m_uws_app_ptr *inside* the deferred lambda
                if (!m_running || !m_uws_app_ptr) return;

                std::queue<std::string> local_queue;
                {
                    std::lock_guard<std::mutex> lock(m_publish_queue_mutex);
                    std::swap(local_queue, m_data_to_publish);
                }

                // FIXED: Publish to the topic via the App pointer
                while (!local_queue.empty()) {
                    std::string msg = local_queue.front();
                    local_queue.pop();

                    // This is the correct, thread-safe, non-blocking way
                    m_uws_app_ptr->publish("data/all", msg, uWS::OpCode::TEXT, false);
                }
                });
        }
    }

private:
    // --- Core State ---
    std::atomic<bool> m_running;
    zmq::context_t m_zmq_context;

    // --- Core Threading ---
    std::thread m_uws_thread;
    std::thread m_zmq_bridge_thread;

    // --- Adapter Management ---
    std::mutex m_adapters_mutex;
    std::map<std::string, std::shared_ptr<IProtocolAdapter>> m_adapters;

    // --- ImGui State ---
    std::mutex m_log_mutex;
    std::deque<std::string> m_logs;
    std::mutex m_device_mutex;
    std::map<std::string, DeviceData> m_devices;
    std::mutex m_notifications_mutex;
    std::queue<std::pair<std::string, bool>> m_notifications; // {message, is_success}

    // --- uWS State ---
    uWS::Loop* m_uws_loop = nullptr;
    uWS::App* m_uws_app_ptr = nullptr;
    std::mutex m_uws_mutex;
    std::condition_variable m_uws_cv;
    std::mutex m_publish_queue_mutex;
    std::queue<std::string> m_data_to_publish; // Data to be sent by uWS thread
    //std::mutex m_ws_clients_mutex;
    //std::set<WebSocket*> m_ws_clients;

    // --- ZMQ Bridge State ---
    std::mutex m_command_queue_mutex;
    std::condition_variable m_command_queue_cv;
    std::queue<CommandToRoute> m_command_queue; // Commands from uWS/ImGui

    // --- Thread-safe mechanism to stop the uWS thread ---
    void DeferStopLoop() {
        if (m_uws_loop) {
            m_uws_loop->defer([this]() {
                // This code runs on the uWS thread
                AddLog("uWS loop received deferred stop signal.");
                // Freeing the loop pointer breaks the App::run() blocking call
                us_loop_free((us_loop_t*)m_uws_loop);
                });
        }
    }


    // ===================================================================
    // THREAD: uWebSockets Server
    // ===================================================================
    void RunUwsServer() {
        AddLog("uWS thread started.");

        try {
            // Get the loop instance
            m_uws_loop = uWS::Loop::get();

            // --- THIS IS THE KEY ---
            // Create the App object *on this thread*
            uWS::App local_app;
            // Save a pointer to it for DeferPublishData to use
            m_uws_app_ptr = &local_app;
            // --- END KEY ---

            // Notify the main thread that the loop is ready
            {
                std::lock_guard<std::mutex> lock(m_uws_mutex);
                m_uws_cv.notify_one();
            }

            // 1. WebSocket Behavior
            uWS::App::WebSocketBehavior<PerSocketData> behavior;
            behavior.idleTimeout = 900;
            behavior.maxPayloadLength = 16 * 1024;

            behavior.open = [this](WebSocket* ws) {
                AddLog("Web client connected.");
                ws->subscribe("data/all");
                // --- NO m_ws_clients logic needed ---
                };

            behavior.message = [this](WebSocket* ws, std::string_view message, uWS::OpCode opCode) {
                AddLog("Received command from WebUI: " + std::string(message));
                try {
                    // Assume message is JSON: {"targetAdapter": "id", "command": "..."}
                    // Quick and dirty parse to find targetAdapter
                    std::string msg_str(message);
                    size_t target_pos = msg_str.find("\"targetAdapter\"");
                    if (target_pos == std::string::npos) return;

                    size_t start = msg_str.find(":", target_pos);
                    start = msg_str.find("\"", start);
                    if (start == std::string::npos) return;

                    size_t end = msg_str.find("\"", start + 1);
                    if (end == std::string::npos) return;

                    std::string adapter_name = msg_str.substr(start + 1, end - start - 1);

                    // Queue this command for the ZMQ bridge thread
                    {
                        std::lock_guard<std::mutex> lock(m_command_queue_mutex);
                        m_command_queue.push({ adapter_name, msg_str });
                    }
                    m_command_queue_cv.notify_one();

                }
                catch (const std::exception& e) {
                    AddLog("Error parsing uWS command: " + std::string(e.what()));
                }
                };

            behavior.close = [this](WebSocket* ws, int code, std::string_view message) {
                AddLog("Web client disconnected.");
                // --- NO m_ws_clients logic needed ---
                };

            // 2. Start listening
            // FIXED: Use the local_app object
            local_app
                .ws<PerSocketData>("/gateway", std::move(behavior))
                .listen(WS_PORT, [this](auto* listen_socket) {
                if (listen_socket) {
                    AddLog("uWS listening on port " + std::to_string(WS_PORT));
                }
                else {
                    AddLog("uWS FAILED to listen on port " + std::to_string(WS_PORT));
                }
                    })
                .run(); // This blocks until loop is freed

            // run() has returned, so we are stopping
            m_uws_loop = nullptr;
            m_uws_app_ptr = nullptr; // Clear the pointer
            AddLog("uWS thread stopped.");

        }
        catch (const std::exception& e) {
            AddLog("uWS thread exception: " + std::string(e.what()));
        }
    }

    // ===================================================================
    // THREAD: ZMQ Bridge (The Heart)
    // ===================================================================
    void RunZmqBridge() {
        AddLog("ZMQ bridge thread started.");
        try {
            // 1. PULL socket for incoming data from all adapters
            zmq::socket_t data_pull(m_zmq_context, zmq::socket_type::pull);
            data_pull.bind(DATA_STREAM_BUS);

            // 2. ROUTER socket for commands (to/from adapters)
            zmq::socket_t cmd_router(m_zmq_context, zmq::socket_type::router);
            cmd_router.bind(COMMAND_STREAM_BUS);

            // Poll items
            zmq::pollitem_t items[] = {
                { data_pull, 0, ZMQ_POLLIN, 0 }, // Poll for data
                { cmd_router, 0, ZMQ_POLLIN, 0 }  // Poll for commands from adapters
            };

            while (m_running) {
                // Wait for ZMQ activity
                // We use a timeout so we can check the command queue from ImGui/uWS
                // This call is already correct and uses std::chrono::milliseconds(100)
                int rc = zmq::poll(items, 2, std::chrono::milliseconds(100));

                if (rc == -1) {
                    if (m_running) AddLog("ZMQ bridge poll error");
                    break; // Exit on error (e.g., context shutdown)
                }

                // --- 1. Process incoming data from adapters ---
                if (items[0].revents & ZMQ_POLLIN) {
                    zmq::message_t msg;
                    auto res = data_pull.recv(msg, zmq::recv_flags::dontwait);
                    if (res.has_value()) {
                        std::string data_str = msg.to_string();

                        // A. Update internal device status (thread-safe)
                        UpdateDeviceStatus(data_str);

                        // B. Queue data for uWS server (thread-safe)
                        {
                            std::lock_guard<std::mutex> lock(m_publish_queue_mutex);
                            m_data_to_publish.push(data_str);
                        }
                        // Wake up uWS thread using defer
                        DeferPublishData();
                    }
                }

                // --- 2. Process commands/responses from adapters ---
                if (items[1].revents & ZMQ_POLLIN) {
                    // ROUTER socket receives [identity] [empty] [payload]
                    zmq::message_t id_msg, empty_msg, payload_msg;
                    cmd_router.recv(id_msg, zmq::recv_flags::none);
                    cmd_router.recv(empty_msg, zmq::recv_flags::none);
                    cmd_router.recv(payload_msg, zmq::recv_flags::none);

                    AddLog("Bridge: Received msg from adapter " + id_msg.to_string() + ": " + payload_msg.to_string());
                    // Full implementation would handle responses/heartbeats here
                }

                // --- 3. Process commands from ImGui/uWS ---
                CommandToRoute cmd;
                bool cmd_found = false;
                {
                    // Use condition variable to wait for commands or timeout
                    std::unique_lock<std::mutex> lock(m_command_queue_mutex);
                    m_command_queue_cv.wait_for(lock, std::chrono::milliseconds(0), [this] { return !m_command_queue.empty(); });

                    if (!m_command_queue.empty()) {
                        cmd = m_command_queue.front();
                        m_command_queue.pop();
                        cmd_found = true;
                    }
                }

                if (cmd_found) {
                    if (!m_running && cmd.adapter_name.empty()) break; // Stop signal (empty command)

                    // Check if adapter exists
                    bool adapter_found = false;
                    {
                        std::lock_guard<std::mutex> lock(m_adapters_mutex);
                        adapter_found = m_adapters.count(cmd.adapter_name);
                    }

                    if (adapter_found) {
                        AddLog("Bridge: Routing command to adapter " + cmd.adapter_name);
                        // Send to ROUTER socket: [identity] [empty] [payload]
                        cmd_router.send(zmq::buffer(cmd.adapter_name), zmq::send_flags::sndmore);
                        cmd_router.send(zmq::buffer(""), zmq::send_flags::sndmore);
                        cmd_router.send(zmq::buffer(cmd.payload_json), zmq::send_flags::none);
                    }
                    else {
                        AddLog("Bridge: Command for unknown adapter " + cmd.adapter_name);
                    }
                }

            } // end while(m_running)

        }
        catch (const zmq::error_t& e) {
            if (m_running) AddLog("ZMQ bridge thread exception: " + std::string(e.what()));
        }
        AddLog("ZMQ bridge thread stopped.");
    }

    // --- Helper to parse JSON and update device map ---
    void UpdateDeviceStatus(const std::string& json_data) {
        // Quick and dirty parse
        try {
            std::string device_id, protocol, adapter_name, values;

            auto find_value = [&](const std::string& key) -> std::string {
                size_t pos = json_data.find("\"" + key + "\"");
                if (pos == std::string::npos) return "";
                size_t start = json_data.find(":", pos);
                start = json_data.find("\"", start);
                size_t end = json_data.find("\"", start + 1);
                return json_data.substr(start + 1, end - start - 1);
                };

            device_id = find_value("deviceId");
            protocol = find_value("protocol");
            adapter_name = find_value("adapterName");

            size_t val_pos = json_data.find("\"values\"");
            if (val_pos == std::string::npos) return;
            size_t brace_start = json_data.find("{", val_pos);
            size_t brace_end = json_data.find("}", brace_start);
            values = json_data.substr(brace_start, brace_end - brace_start + 1);

            if (device_id.empty()) return;

            std::lock_guard<std::mutex> lock(m_device_mutex);
            auto& device = m_devices[device_id]; // Creates if not exist
            device.id = device_id;
            device.protocol = protocol;
            device.adapter_name = adapter_name;
            device.last_value_json = values;
            device.last_seen = time(nullptr);
            device.message_count++;
        }
        catch (...) {
            AddLog("Error parsing JSON in UpdateDeviceStatus: " + json_data);
        }
    }

    // --- Helper Functions ---
    std::string GetTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        // The use of std::localtime is accepted due to the _CRT_SECURE_NO_WARNINGS macro being defined
        ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %X");
        return ss.str();
    }
};

// --- IProtocolAdapter helper implementation ---
void IProtocolAdapter::Log(const std::string& msg) {
    if (m_hub) {
        m_hub->AddLog("[" + m_name + "] " + msg);
    }
}


// =================================================================================
//
// CONCRETE ADAPTERS (REDESIGNED)
//
// =================================================================================

/**
 * @brief Helper struct to manage a single device connection in its own thread.
 */
struct ModbusDeviceWorker {
    std::string name;
    std::string ip;
    int port;
    std::atomic<bool> running;
    std::atomic<bool> stop_requested;
    std::string status;
    std::thread thread;

    ModbusDeviceWorker() : running(false), stop_requested(false), status("Initialized") {}
};


/**
 * @brief ModbusTCPAdapter now manages a map of ModbusDeviceWorker threads.
 */
class ModbusTCPAdapter : public IProtocolAdapter {
public:
    ModbusTCPAdapter(std::string name, GatewayHub* hub, zmq::context_t& zmq_ctx)
        : IProtocolAdapter(name, hub, zmq_ctx) {
        std::string m_status = "Running (0 devices)";
    }

    ~ModbusTCPAdapter() {
        Stop(); // Stops the manager thread
        // Destructor of m_devices will handle stopping/joining device threads
    }

    std::string GetProtocol() const override { return "ModbusTCP"; }
    std::string GetName() const override { return m_name; }

    std::string GetStatus() const override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        return "Running (" + std::to_string(m_devices.size()) + " devices)";
    }

    std::map<std::string, std::string> GetDeviceStatuses() const override {
        std::map<std::string, std::string> statuses;
        std::lock_guard<std::mutex> lock(m_device_mutex);
        for (auto const& [name, worker] : m_devices) {
            statuses[name] = worker->status;
        }
        return statuses;
    }

    bool AddDevice(const std::string& device_name, const DeviceConfig& config) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (m_devices.count(device_name)) {
            Log("Error: Device " + device_name + " already exists.");
            return false;
        }

        try {
            auto worker = std::make_unique<ModbusDeviceWorker>();
            worker->name = device_name;
            worker->ip = config.at("ip");
            worker->port = std::stoi(config.at("port"));
            worker->running = true;
            worker->stop_requested = false;
            worker->status = "Starting...";

            // Start the actual polling thread for this device
            worker->thread = std::thread(&ModbusTCPAdapter::DevicePollLoop, this, worker.get());

            m_devices[device_name] = std::move(worker);
            Log("Started device " + device_name + " at " + config.at("ip"));
            return true;
        }
        catch (const std::exception& e) {
            Log("Error adding device " + device_name + ": " + e.what());
            return false;
        }
    }

    bool RemoveDevice(const std::string& device_name) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        auto it = m_devices.find(device_name);
        if (it != m_devices.end()) {
            Log("Stopping device " + device_name);
            it->second->stop_requested = true;
            if (it->second->thread.joinable()) {
                it->second->thread.join();
            }
            m_devices.erase(it);
            Log("Removed device " + device_name);
            return true;
        }
        return false;
    }

    /**
     * @brief Manager thread loop.
     */
    void Run() override {
        Log("Manager thread started.");
        while (m_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            // In a real app, this thread would check for dead worker threads
            // and restart them.
        }

        // Stop is requested, so tell all workers to stop
        std::lock_guard<std::mutex> lock(m_device_mutex);
        for (auto& [name, worker] : m_devices) {
            worker->stop_requested = true;
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
        }
        m_devices.clear();
        Log("Manager thread stopped.");
    }

    /**
     * @brief The *actual* device I/O loop.
     * MOVED TO PUBLIC so std::thread can access it.
     */
    void DevicePollLoop(ModbusDeviceWorker* worker) {
        Log(worker->name + ": Thread started.");
        modbus_t* ctx = nullptr;

        try {
            // PUSH socket for data (thread-safe, one per thread)
            zmq::socket_t data_push(m_zmq_context, zmq::socket_type::push);
            data_push.connect(DATA_STREAM_BUS);

            // --- Real Modbus Logic ---
            ctx = modbus_new_tcp(worker->ip.c_str(), worker->port);
            if (!ctx) {
                worker->status = "Error: Failed context";
                return;
            }
            if (modbus_connect(ctx) == -1) {
                worker->status = "Error: Connection Failed (" + std::string(modbus_strerror(errno)) + ")";
                modbus_free(ctx);
                return;
            }

            worker->status = "Running (Active)";
            uint16_t regs[10];

            while (!worker->stop_requested) {
                int num_regs = modbus_read_registers(ctx, 0, 5, regs);

                if (num_regs == -1) {
                    worker->status = "Error: Read failed";
                }
                else {
                    worker->status = "Running (Active)";

                    // The adapter *invents* the deviceId.
                    // This is the CRITICAL step.
                    std::string deviceId = worker->name + ":reg_0_to_4";

                    std::stringstream ss;
                    ss << "{\"deviceId\":\"" << deviceId << "\",\"adapterName\":\"" << m_name << "\",\"protocol\":\"" << GetProtocol() << "\",\"timestamp\":" << time(nullptr)
                        << ",\"values\":{\"reg0\":" << regs[0] << ",\"reg1\":" << regs[1] << ",\"reg2\":" << regs[2] << ",\"reg3\":" << regs[3] << ",\"reg4\":" << regs[4] << "}}";

                    data_push.send(zmq::buffer(ss.str()), zmq::send_flags::none);
                }

                // Use a timed sleep so the stop request is responsive
                for (int i = 0; i < 20 && !worker->stop_requested; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        catch (const std::exception& e) {
            Log(worker->name + " exception: " + e.what());
            worker->status = "Error: Exception";
        }

        if (ctx) {
            modbus_close(ctx);
            modbus_free(ctx);
        }
        worker->status = "Stopped";
        Log(worker->name + ": Thread stopped.");
    }

private:
    mutable std::mutex m_device_mutex;
    std::map<std::string, std::unique_ptr<ModbusDeviceWorker>> m_devices;
};

/**
 * @brief Context struct passed to open62541 callbacks
 * A unique instance of this struct is created for EACH monitored item.
 */
struct OpcuaCallbackContext {
    std::string device_name;
    std::string adapter_name;
    std::string protocol;
    std::string nodeIdStr;
    std::queue<std::string>* data_queue;
    std::mutex* queue_mutex;
    GatewayHub* hub;
};

/**
 * @brief Helper struct for OPC-UA device worker.
 */
struct OpcuaDeviceWorker {
    std::string name;
    std::string endpoint;
    std::vector<std::string> nodeIds;
    std::atomic<bool> stop_requested;
    std::string status;
    std::thread thread;
    UA_Client* client;
    std::queue<std::string> data_queue;
    std::mutex queue_mutex;
    std::vector<std::unique_ptr<OpcuaCallbackContext>> itemContexts;
    OpcuaDeviceWorker() : stop_requested(false), status("Initialized"), client(nullptr) {}
};


/**
 * @brief OpcuaAdapter implementation that manages subscriptions.
 */
class OpcuaAdapter : public IProtocolAdapter {
public:
    OpcuaAdapter(std::string name, GatewayHub* hub, zmq::context_t& zmq_ctx)
        : IProtocolAdapter(name, hub, zmq_ctx) {
        std::string m_status = "Running (0 devices)";
    }

    ~OpcuaAdapter() {
        Stop();
    }

    std::string GetName() const override { return m_name; }
    std::string GetProtocol() const override { return "OPC-UA"; }

    std::string GetStatus() const override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        return "Running (" + std::to_string(m_devices.size()) + " devices)";
    }

    std::map<std::string, std::string> GetDeviceStatuses() const override {
        std::map<std::string, std::string> statuses;
        std::lock_guard<std::mutex> lock(m_device_mutex);
        for (auto const& [name, worker] : m_devices) {
            statuses[name] = worker->status;
        }
        return statuses;
    }

    bool AddDevice(const std::string& device_name, const DeviceConfig& config) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (m_devices.count(device_name)) {
            Log("Error: Device " + device_name + " already exists.");
            return false;
        }

        try {
            auto worker = std::make_unique<OpcuaDeviceWorker>();
            worker->name = device_name;
            worker->endpoint = config.at("endpoint");

            // Parse comma-separated NodeIDs
            std::stringstream ss(config.at("nodeIds"));
            std::string nodeId;
            while (std::getline(ss, nodeId, ',')) {
                // Trim whitespace
                nodeId.erase(0, nodeId.find_first_not_of(" \n\r\t"));
                nodeId.erase(nodeId.find_last_not_of(" \n\r\t") + 1);
                if (!nodeId.empty()) {
                    worker->nodeIds.push_back(nodeId);
                }
            }
            if (worker->nodeIds.empty()) {
                Log("Error: No valid NodeIDs provided for " + device_name);
                return false;
            }

            worker->stop_requested = false;
            worker->status = "Starting...";

            worker->thread = std::thread(&OpcuaAdapter::DevicePollLoop, this, worker.get());
            m_devices[device_name] = std::move(worker);

            Log("Started OPC-UA device " + device_name + " for " + config.at("endpoint"));
            return true;
        }
        catch (const std::exception& e) {
            Log("Error adding OPC-UA device " + device_name + ": " + e.what());
            return false;
        }
    }

    bool RemoveDevice(const std::string& device_name) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        auto it = m_devices.find(device_name);
        if (it != m_devices.end()) {
            Log("Stopping OPC-UA device " + device_name);
            it->second->stop_requested = true;
            if (it->second->thread.joinable()) {
                it->second->thread.join();
            }
            // itemContexts will be cleared automatically by unique_ptr
            m_devices.erase(it);
            Log("Removed OPC-UA device " + device_name);
            return true;
        }
        return false;
    }

    // Manager thread
    void Run() override {
        Log("Manager thread started.");
        while (m_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }

        std::lock_guard<std::mutex> lock(m_device_mutex);
        for (auto& [name, worker] : m_devices) {
            worker->stop_requested = true;
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
        }
        m_devices.clear();
        Log("Manager thread stopped.");
    }

    /**
     * @brief open62541 data change callback (ROBUST VERSION)
     * The monContext is a pointer to a unique OpcuaCallbackContext
     */
    static void dataChangeCallback(UA_Client* client, UA_UInt32 subId, void* subContext,
        UA_UInt32 monId, void* monContext, UA_DataValue* value) {
        OpcuaCallbackContext* ctx = static_cast<OpcuaCallbackContext*>(monContext);
        if (!ctx) return;

        try {
            // We get the nodeIdStr directly from the context, no lookup needed!
            std::string nodeIdStr = ctx->nodeIdStr;

            // Format the value to JSON
            std::string valueStr = "\"unknown\"";
            if (value->value.type == NULL) {
                valueStr = "\"null\"";
            }
            else if (UA_Variant_isScalar(&value->value)) {
                if (value->value.type == &UA_TYPES[UA_TYPES_BOOLEAN]) {
                    valueStr = *static_cast<UA_Boolean*>(value->value.data) ? "true" : "false";
                }
                else if (value->value.type == &UA_TYPES[UA_TYPES_INT32]) {
                    valueStr = std::to_string(*static_cast<UA_Int32*>(value->value.data));
                }
                else if (value->value.type == &UA_TYPES[UA_TYPES_UINT32]) {
                    valueStr = std::to_string(*static_cast<UA_UInt32*>(value->value.data));
                }
                else if (value->value.type == &UA_TYPES[UA_TYPES_DOUBLE]) {
                    valueStr = std::to_string(*static_cast<UA_Double*>(value->value.data));
                }
                else if (value->value.type == &UA_TYPES[UA_TYPES_FLOAT]) {
                    valueStr = std::to_string(*static_cast<UA_Float*>(value->value.data));
                }
                else if (value->value.type == &UA_TYPES[UA_TYPES_STRING]) {
                    UA_String* ua_str = static_cast<UA_String*>(value->value.data);
                    valueStr = "\"" + std::string(reinterpret_cast<char*>(ua_str->data), ua_str->length) + "\"";
                }
            }

            // Create the JSON payload
            std::stringstream ss;
            ss << "{\"deviceId\":\"" << ctx->device_name << ":" << nodeIdStr << "\","
                << "\"adapterName\":\"" << ctx->adapter_name << "\","
                << "\"protocol\":\"" << ctx->protocol << "\","
                << "\"timestamp\":" << time(nullptr) << ","
                << "\"values\":{\"" << nodeIdStr << "\":" << valueStr << "}}";

            // Push to thread-safe queue
            std::lock_guard<std::mutex> lock(*ctx->queue_mutex);
            ctx->data_queue->push(ss.str());

        }
        catch (const std::exception& e) {
            if (ctx->hub) ctx->hub->AddLog("OPC-UA Callback Error: " + std::string(e.what()));
        }
    }


    /**
     * @brief The *actual* device I/O loop.
     */
    void DevicePollLoop(OpcuaDeviceWorker* worker) {
        Log(worker->name + ": Thread started.");

        worker->client = UA_Client_new();
        UA_ClientConfig_setDefault(UA_Client_getConfig(worker->client));

        try {
            zmq::socket_t data_push(m_zmq_context, zmq::socket_type::push);
            data_push.connect(DATA_STREAM_BUS);

            worker->status = "Connecting...";
            UA_StatusCode retval = UA_Client_connect(worker->client, worker->endpoint.c_str());
            if (retval != UA_STATUSCODE_GOOD) {
                worker->status = "Error: Connection Failed";
                UA_Client_delete(worker->client);
                worker->client = nullptr;
                return;
            }

            worker->status = "Creating Subscription...";
            UA_CreateSubscriptionRequest subReq = UA_CreateSubscriptionRequest_default();
            UA_CreateSubscriptionResponse subRes = UA_Client_Subscriptions_create(worker->client, subReq,
                NULL, NULL, NULL);
            if (subRes.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
                worker->status = "Error: Failed Subscription";
                UA_Client_disconnect(worker->client);
                UA_Client_delete(worker->client);
                worker->client = nullptr;
                return;
            }
            UA_UInt32 subId = subRes.subscriptionId;

            // Add Monitored Items
            for (const auto& nodeIdStr : worker->nodeIds) {
                UA_NodeId nodeId = UA_NODEID_NULL;

                // Convert std::string to UA_String
                UA_String uaStr = UA_STRING_ALLOC(nodeIdStr.c_str());

                // Parse into a NodeId
                UA_StatusCode retval = UA_NodeId_parse(&nodeId, uaStr);

                // Free the temporary UA_String
                UA_String_clear(&uaStr);

                if (retval != UA_STATUSCODE_GOOD) {
                    Log(worker->name + ": Invalid NodeID " + nodeIdStr);
                    continue;
                }

                auto item_ctx = std::make_unique<OpcuaCallbackContext>();
                item_ctx->device_name = worker->name;
                item_ctx->adapter_name = m_name;
                item_ctx->protocol = GetProtocol();
                item_ctx->nodeIdStr = nodeIdStr;
                item_ctx->data_queue = &worker->data_queue;
                item_ctx->queue_mutex = &worker->queue_mutex;
                item_ctx->hub = m_hub;

                UA_MonitoredItemCreateRequest monReq = UA_MonitoredItemCreateRequest_default(nodeId);

                UA_Client_MonitoredItems_createDataChange(
                    worker->client,
                    subId,
                    UA_TIMESTAMPSTORETURN_BOTH,
                    monReq,
                    item_ctx.get(),
                    dataChangeCallback,
                    nullptr
                );

                worker->itemContexts.push_back(std::move(item_ctx));
                UA_NodeId_clear(&nodeId);
            }

            worker->status = "Running (Active)";

            while (!worker->stop_requested) {
                // Run the client's async loop
                UA_Client_run_iterate(worker->client, 100);

                // Check for data from the callback
                std::lock_guard<std::mutex> lock(worker->queue_mutex);
                while (!worker->data_queue.empty()) {
                    data_push.send(zmq::buffer(worker->data_queue.front()), zmq::send_flags::none);
                    worker->data_queue.pop();
                }
            }

        }
        catch (const std::exception& e) {
            Log(worker->name + " exception: " + e.what());
            worker->status = "Error: Exception";
        }

        if (worker->client) {
            UA_Client_disconnect(worker->client);
            UA_Client_delete(worker->client);
            worker->client = nullptr;
        }
        worker->status = "Stopped";
        Log(worker->name + ": Thread stopped.");
    }

private:
    mutable std::mutex m_device_mutex;
    std::map<std::string, std::unique_ptr<OpcuaDeviceWorker>> m_devices;
};


/**
 * @brief Context for Paho MQTT C callbacks
 */
struct MqttCallbackContext {
    std::string device_name;
    std::string adapter_name;
    std::string protocol;
    // Thread-safe queue to pass data from callback thread to worker thread
    std::queue<std::string>* data_queue;
    std::mutex* queue_mutex;
    GatewayHub* hub;
};

/**
 * @brief Helper struct for MQTT broker connection worker.
 */
struct MqttDeviceWorker {
    std::string name; // Name of this broker connection, e.g., "MainBroker"
    std::string brokerUri;
    std::string clientId;
    std::string commandTopic; // Topic to PUBLISH commands TO
    std::vector<std::string> subscribeTopics;

    std::atomic<bool> stop_requested;
    std::string status;
    std::thread thread;
    MQTTClient client;

    // Thread-safe queue for incoming data (from onMessageArrived)
    std::queue<std::string> data_queue;
    std::mutex data_queue_mutex;

    // Thread-safe queue for outgoing data (from Manager::Run)
    std::queue<std::string> cmd_queue;
    std::mutex cmd_queue_mutex;

    MqttCallbackContext callback_context;

    MqttDeviceWorker() : stop_requested(false), status("Initialized"), client(nullptr) {}
};

/**
 * @brief MqttAdapter implementation that manages broker connections.
 */
class MqttAdapter : public IProtocolAdapter {
public:
    MqttAdapter(std::string name, GatewayHub* hub, zmq::context_t& zmq_ctx)
        : IProtocolAdapter(name, hub, zmq_ctx) {
        m_status = "Running (0 devices)";
    }

    ~MqttAdapter() {
        Stop();
    }

    std::string GetName() const override { return m_name; }
    std::string GetProtocol() const override { return "MQTT"; }

    std::string GetStatus() const override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        return "Running (" + std::to_string(m_devices.size()) + " devices)";
    }

    std::map<std::string, std::string> GetDeviceStatuses() const override {
        std::map<std::string, std::string> statuses;
        std::lock_guard<std::mutex> lock(m_device_mutex);
        // FIXED: Use iterator loop instead of C++17 structured binding
        for (auto const& pair : m_devices) {
            statuses[pair.first] = pair.second->status;
        }
        return statuses;
    }

    bool AddDevice(const std::string& device_name, const DeviceConfig& config) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (m_devices.count(device_name)) {
            Log("Error: Device " + device_name + " already exists.");
            return false;
        }

        try {
            auto worker = std::make_unique<MqttDeviceWorker>();
            worker->name = device_name;
            worker->brokerUri = config.at("brokerUri");
            worker->commandTopic = config.at("commandTopic");
            worker->clientId = config.count("clientId") ? config.at("clientId") : "GatewayHub_" + m_name + "_" + device_name;

            // Parse comma-separated topics
            std::stringstream ss(config.at("subscribeTopics"));
            std::string topic_str; // Use different name to avoid shadowing
            while (std::getline(ss, topic_str, ',')) {
                topic_str.erase(0, topic_str.find_first_not_of(" \n\r\t"));
                topic_str.erase(topic_str.find_last_not_of(" \n\r\t") + 1);
                if (!topic_str.empty()) {
                    worker->subscribeTopics.push_back(topic_str);
                }
            }
            if (worker->subscribeTopics.empty()) {
                Log("Error: No valid subscribeTopics provided for " + device_name);
                return false;
            }

            worker->stop_requested = false;
            worker->status = "Starting...";

            // Setup callback context
            worker->callback_context.device_name = device_name;
            worker->callback_context.adapter_name = m_name;
            worker->callback_context.protocol = GetProtocol();
            worker->callback_context.data_queue = &worker->data_queue;
            worker->callback_context.queue_mutex = &worker->data_queue_mutex;
            worker->callback_context.hub = m_hub;

            // Use ( ) not { } for std::thread constructor
            worker->thread = std::thread(&MqttAdapter::DevicePollLoop, this, worker.get());
            m_devices[device_name] = std::move(worker);

            Log("Started MQTT device " + device_name + " for " + config.at("brokerUri"));
            return true;
        }
        catch (const std::exception& e) {
            Log("Error adding MQTT device " + device_name + ": " + e.what());
            return false;
        }
    }

    bool RemoveDevice(const std::string& device_name) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        auto it = m_devices.find(device_name);
        if (it != m_devices.end()) {
            Log("Stopping MQTT device " + device_name);
            it->second->stop_requested = true;
            if (it->second->thread.joinable()) {
                it->second->thread.join();
            }
            m_devices.erase(it);
            Log("Removed MQTT device " + device_name);
            return true;
        }
        return false;
    }

    // Manager thread - Listens for ZMQ commands
    void Run() override {
        Log("Manager thread started.");
        zmq::socket_t cmd_dealer(m_zmq_context, zmq::socket_type::dealer);
        try {
            cmd_dealer.set(zmq::sockopt::routing_id, m_name);
            cmd_dealer.connect(COMMAND_STREAM_BUS);

            zmq::pollitem_t items[] = { { cmd_dealer, 0, ZMQ_POLLIN, 0 } };

            while (m_running) {
                if (zmq::poll(items, 1, std::chrono::milliseconds(100)) > 0) {
                    if (items[0].revents & ZMQ_POLLIN) {
                        zmq::message_t empty, payload;
                        cmd_dealer.recv(empty, zmq::recv_flags::none);
                        cmd_dealer.recv(payload, zmq::recv_flags::none);

                        std::string cmd_str = payload.to_string();
                        Log("Received command: " + cmd_str);

                        // Parse JSON to find targetDevice
                        std::string targetDevice = "";
                        try {
                            size_t pos = cmd_str.find("\"targetDevice\"");
                            if (pos != std::string::npos) {
                                size_t start = cmd_str.find("\"", pos + 15);
                                size_t end = cmd_str.find("\"", start + 1);
                                targetDevice = cmd_str.substr(start + 1, end - start - 1);
                            }
                        }
                        catch (...) { /* ignore parse error */ }

                        if (targetDevice.empty()) {
                            Log("Command JSON missing 'targetDevice' field.");
                            continue;
                        }

                        // Find worker and queue the command
                        std::lock_guard<std::mutex> lock(m_device_mutex);
                        if (m_devices.count(targetDevice)) {
                            auto& worker = m_devices.at(targetDevice);
                            // FIXED: Access member on unique_ptr
                            std::lock_guard<std::mutex> cmd_lock(worker->cmd_queue_mutex);
                            worker->cmd_queue.push(cmd_str);
                        }
                        else {
                            Log("Command for unknown device: " + targetDevice);
                        }
                    }
                }
            }
        }
        catch (const std::exception& e) {
            Log("Manager thread exception: " + std::string(e.what()));
        }

        // Stop all workers
        std::lock_guard<std::mutex> lock(m_device_mutex);
        // FIXED: Use iterator loop
        for (auto& pair : m_devices) {
            pair.second->stop_requested = true;
            if (pair.second->thread.joinable()) {
                pair.second->thread.join();
            }
        }
        m_devices.clear();
        Log("Manager thread stopped.");
    }

    /**
     * @brief Paho callback for connection lost
     */
    static void onConnectionLost(void* context, char* cause) {
        MqttCallbackContext* ctx = static_cast<MqttCallbackContext*>(context);
        if (ctx && ctx->hub) {
            ctx->hub->AddLog("MQTT (" + ctx->device_name + "): Connection lost: " + (cause ? cause : "unknown"));
        }
        // Worker thread will handle reconnect logic
    }

    /**
     * @brief Paho callback for message arrival
     */
    static int onMessageArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message) {
        MqttCallbackContext* ctx = static_cast<MqttCallbackContext*>(context);
        if (!ctx) return 1;

        try {
            std::string topic(topicName, topicLen);
            std::string payload(static_cast<char*>(message->payload), message->payloadlen);

            std::stringstream ss_simple;
            ss_simple << "{\"deviceId\":\"" << ctx->device_name << ":" << topic << "\","
                << "\"adapterName\":\"" << ctx->adapter_name << "\","
                << "\"protocol\":\"" << ctx->protocol << "\","
                << "\"timestamp\":" << time(nullptr) << ","
                << "\"values\":";

            // Try to see if payload is JSON, otherwise treat as string
            if (payload.starts_with("{") && payload.ends_with("}")) {
                ss_simple << payload;
            }
            else {
                // Escape string
                std::string escaped_payload;
                for (char c : payload) {
                    if (c == '"' || c == '\\') escaped_payload += '\\';
                    escaped_payload += c;
                }
                ss_simple << "{\"value\":\"" << escaped_payload << "\"}}";
            }

            std::lock_guard<std::mutex> lock(*ctx->queue_mutex);
            ctx->data_queue->push(ss_simple.str());

        }
        catch (const std::exception& e) {
            if (ctx->hub) ctx->hub->AddLog("MQTT Callback Error: " + std::string(e.what()));
        }

        MQTTClient_freeMessage(&message);
        MQTTClient_free(topicName);
        return 1;
    }

    /**
     * @brief The *actual* device I/O loop.
     */
    void DevicePollLoop(MqttDeviceWorker* worker) {
        Log(worker->name + ": Thread started.");

        try {
            zmq::socket_t data_push(m_zmq_context, zmq::socket_type::push);
            data_push.connect(DATA_STREAM_BUS);

            worker->status = "Connecting...";
            MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
            conn_opts.keepAliveInterval = 20;
            conn_opts.cleansession = 1;

            int rc = 0;
            // FIXED: MQTTClient_create takes 5 arguments
            rc = MQTTClient_create(&worker->client, worker->brokerUri.c_str(), worker->clientId.c_str(),
                MQTTCLIENT_PERSISTENCE_NONE, NULL);
            if (rc != MQTTCLIENT_SUCCESS) {
                worker->status = "Error: Failed to create";
                return;
            }

            // FIXED: MQTTClient_setCallbacks takes 5 arguments (NULL for deliveryComplete)
            MQTTClient_setCallbacks(worker->client, &worker->callback_context,
                onConnectionLost, onMessageArrived, NULL);

            rc = MQTTClient_connect(worker->client, &conn_opts);
            if (rc != MQTTCLIENT_SUCCESS) {
                worker->status = "Error: Connection failed";
                MQTTClient_destroy(&worker->client);
                return;
            }

            worker->status = "Subscribing...";
            for (const auto& topic : worker->subscribeTopics) {
                // FIXED: MQTTClient_subscribe takes 3 arguments (QoS)
                MQTTClient_subscribe(worker->client, topic.c_str(), 0);
            }
            worker->status = "Running (Active)";

            while (!worker->stop_requested) {
                // 1. Process incoming data from queue
                {
                    // FIXED: Access member on raw pointer
                    std::lock_guard<std::mutex> lock(worker->data_queue_mutex);
                    while (!worker->data_queue.empty()) {
                        data_push.send(zmq::buffer(worker->data_queue.front()), zmq::send_flags::none);
                        worker->data_queue.pop();
                    }
                }

                // 2. Process outgoing commands from queue
                {
                    // FIXED: Access member on raw pointer
                    std::lock_guard<std::mutex> lock(worker->cmd_queue_mutex);
                    while (!worker->cmd_queue.empty()) {
                        std::string cmd_payload = worker->cmd_queue.front();
                        worker->cmd_queue.pop();

                        Log("Publishing to " + worker->commandTopic + ": " + cmd_payload);

                        MQTTClient_message pubmsg = MQTTClient_message_initializer;
                        pubmsg.payload = (void*)cmd_payload.c_str();
                        pubmsg.payloadlen = (int)cmd_payload.length();
                        pubmsg.qos = 0;
                        pubmsg.retained = 0;

                        // FIXED: MQTTClient_publishMessage takes 4 arguments (NULL for token)
                        MQTTClient_publishMessage(worker->client, worker->commandTopic.c_str(), &pubmsg, NULL);
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

        }
        catch (const std::exception& e) {
            Log(worker->name + " exception: " + e.what());
            worker->status = "Error: Exception";
        }

        if (worker->client && MQTTClient_isConnected(worker->client)) {
            MQTTClient_disconnect(worker->client, 1000);
        }
        if (worker->client) {
            MQTTClient_destroy(&worker->client);
        }
        worker->status = "Stopped";
        Log(worker->name + ": Thread stopped.");
    }

private:
    mutable std::mutex m_device_mutex;
    std::map<std::string, std::unique_ptr<MqttDeviceWorker>> m_devices;

    // FIX: m_status was not declared, only initialized in constructor
    std::string m_status;
};


/**
 * @brief Helper struct for ZMQ device worker.
 */
struct ZmqDeviceWorker {
    std::string name;
    std::string endpoint;
    std::string topic;
    std::atomic<bool> stop_requested;
    std::string status;
    std::thread thread;

    ZmqDeviceWorker() : stop_requested(false), status("Initialized") {}
};

/**
 * @brief ZmqAdapter implementation that manages SUB sockets.
 */
class ZmqAdapter : public IProtocolAdapter {
public:
    ZmqAdapter(std::string name, GatewayHub* hub, zmq::context_t& zmq_ctx)
        : IProtocolAdapter(name, hub, zmq_ctx) {
        // FIXED: Initialize the *member* variable, not a local one
        m_status = "Running (0 devices)";
    }

    ~ZmqAdapter() {
        Stop();
    }

    std::string GetName() const override { return m_name; }
    std::string GetProtocol() const override { return "ZMQ"; }

    std::string GetStatus() const override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        return "Running (" + std::to_string(m_devices.size()) + " devices)";
    }

    std::map<std::string, std::string> GetDeviceStatuses() const override {
        std::map<std::string, std::string> statuses;
        std::lock_guard<std::mutex> lock(m_device_mutex);
        // FIXED: Use iterator loop
        for (auto const& pair : m_devices) {
            statuses[pair.first] = pair.second->status;
        }
        return statuses;
    }

    bool AddDevice(const std::string& device_name, const DeviceConfig& config) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (m_devices.count(device_name)) {
            Log("Error: Device " + device_name + " already exists.");
            return false;
        }

        try {
            auto worker = std::make_unique<ZmqDeviceWorker>();
            worker->name = device_name;
            worker->endpoint = config.at("endpoint");
            worker->topic = config.at("topic");
            worker->stop_requested = false;
            worker->status = "Starting...";

            // FIXED: Use ( ) for std::thread constructor
            worker->thread = std::thread(&ZmqAdapter::DevicePollLoop, this, worker.get());
            m_devices[device_name] = std::move(worker);

            // Use config.at() because the 'worker' unique_ptr is now null after the std::move.
            Log("Started ZMQ device " + device_name + " for " + config.at("endpoint"));
            return true;
        }
        catch (const std::exception& e) {
            Log("Error adding ZMQ device " + device_name + ": " + e.what());
            return false;
        }
    }

    bool RemoveDevice(const std::string& device_name) override {
        std::lock_guard<std::mutex> lock(m_device_mutex);
        auto it = m_devices.find(device_name);
        if (it != m_devices.end()) {
            Log("Stopping ZMQ device " + device_name);
            it->second->stop_requested = true;
            if (it->second->thread.joinable()) {
                it->second->thread.join();
            }
            m_devices.erase(it);
            Log("Removed ZMQ device " + device_name);
            return true;
        }
        return false;
    }

    void Run() override {
        Log("Manager thread started.");
        while (m_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }

        std::lock_guard<std::mutex> lock(m_device_mutex);
        // FIXED: Use iterator loop
        for (auto& pair : m_devices) {
            pair.second->stop_requested = true;
            if (pair.second->thread.joinable()) {
                pair.second->thread.join();
            }
        }
        m_devices.clear();
        Log("Manager thread stopped.");
    }

    /**
     * @brief The *actual* device I/O loop.
     * MOVED TO PUBLIC so std::thread can access it.
     */
    void DevicePollLoop(ZmqDeviceWorker* worker) {
        Log(worker->name + ": Thread started.");
        try {
            zmq::socket_t data_push(m_zmq_context, zmq::socket_type::push);
            data_push.connect(DATA_STREAM_BUS);

            zmq::socket_t sub_socket(m_zmq_context, zmq::socket_type::sub);
            sub_socket.connect(worker->endpoint);
            sub_socket.set(zmq::sockopt::subscribe, worker->topic);

            worker->status = "Running (Active)";

            zmq::pollitem_t items[] = { { sub_socket, 0, ZMQ_POLLIN, 0 } };

            while (!worker->stop_requested) {
                // Poll with a timeout to check the stop flag
                if (zmq::poll(items, 1, std::chrono::milliseconds(100)) > 0) {
                    if (items[0].revents & ZMQ_POLLIN) {
                        zmq::message_t msg;
                        sub_socket.recv(msg, zmq::recv_flags::none);

                        // Python script sends "topic json" as one string.
                        // We must split it and send only the json.
                        std::string s = msg.to_string();
                        size_t space_pos = s.find(' ');
                        if (space_pos != std::string::npos) {
                            std::string json_part = s.substr(space_pos + 1);
                            data_push.send(zmq::buffer(json_part), zmq::send_flags::none);
                        }
                    }
                }
            }
        }
        catch (const std::exception& e) {
            Log(worker->name + " exception: " + e.what());
            worker->status = "Error: Exception";
        }
        worker->status = "Stopped";
        Log(worker->name + ": Thread stopped.");
    }

private:
    mutable std::mutex m_device_mutex;
    std::map<std::string, std::unique_ptr<ZmqDeviceWorker>> m_devices;

    // FIXED: Declare the member variable
    std::string m_status;
};

// --- GatewayHub Adapter Management Implementations ---
// This *must* come after the concrete classes are defined!
bool GatewayHub::AddAdapter(const std::string& name, const std::string& protocol_type) {
    std::lock_guard<std::mutex> lock(m_adapters_mutex);
    if (m_adapters.count(name)) {
        PushNotification("Error: Adapter with name '" + name + "' already exists.", false);
        return false;
    }

    try {
        if (protocol_type == "ModbusTCP") {
            m_adapters[name] = std::make_shared<ModbusTCPAdapter>(name, this, m_zmq_context);
        }
        else if (protocol_type == "OPC-UA") {
            m_adapters[name] = std::make_shared<OpcuaAdapter>(name, this, m_zmq_context);
        }
        else if (protocol_type == "MQTT") {
            m_adapters[name] = std::make_shared<MqttAdapter>(name, this, m_zmq_context);
        }
        else if (protocol_type == "ZMQ") {
            m_adapters[name] = std::make_shared<ZmqAdapter>(name, this, m_zmq_context);
        }
        else {
            PushNotification("Error: Unknown protocol type '" + protocol_type + "'", false);
            return false;
        }

        m_adapters[name]->Start(); // Start the adapter's manager thread
        PushNotification(protocol_type + " service '" + name + "' started.", true);
        return true;
    }
    catch (const std::exception& e) {
        PushNotification("Error starting adapter '" + name + "': " + e.what(), false);
        return false;
    }
}


// =================================================================================
//
// Dear ImGui UI Rendering (REDESIGNED ADAPTER TAB)
//
// =================================================================================

void DrawGatewayUI(GatewayHub& hub) {
    ImGui::SetNextWindowSize(ImVec2(1000, 700), ImGuiCond_FirstUseEver);
    ImGui::Begin("Multiprotocol Gateway Hub");

    // --- Notification Area ---
    static std::queue<std::pair<std::string, bool>> display_notifications;
    static std::chrono::steady_clock::time_point notification_end_time;
    static bool notification_visible = false;
    const std::chrono::seconds display_duration(5);

    // 1. Process new notifications
    std::queue<std::pair<std::string, bool>> new_notifications = hub.PopAllNotifications();
    while (!new_notifications.empty()) {
        display_notifications.push(new_notifications.front());
        new_notifications.pop();
        notification_visible = true;
        notification_end_time = std::chrono::steady_clock::now() + display_duration;
    }

    // 2. Check visibility timeout
    if (notification_visible && std::chrono::steady_clock::now() > notification_end_time) {
        if (!display_notifications.empty()) {
            display_notifications.pop();
        }
        if (display_notifications.empty()) {
            notification_visible = false;
        }
        else {
            // Keep the next notification visible for another full duration
            notification_end_time = std::chrono::steady_clock::now() + display_duration;
        }
    }

    // 3. Render the current notification
    if (notification_visible && !display_notifications.empty()) {
        const auto& current_notif = display_notifications.front();
        ImVec4 color = current_notif.second ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
        ImGui::TextColored(color, "[STATUS] %s", current_notif.first.c_str());
        ImGui::Separator();
    }


    ImGui::Text("uWS Server Status: Running at ws://localhost:%d", WS_PORT);
    ImGui::Separator();

    if (ImGui::BeginTabBar("MainTabs")) {
        // --- Tab 1: Live Log ---
        if (ImGui::BeginTabItem("Live Log")) {
            static std::vector<std::string> logs;
            static int last_log_count = 0;

            hub.GetLogs(logs);
            bool scroll_to_bottom = (logs.size() != last_log_count);
            last_log_count = logs.size();

            ImGui::BeginChild("LogScroll", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), false, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1)); // Tight spacing
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
                    // The use of std::localtime is accepted due to the _CRT_SECURE_NO_WARNINGS macro being defined
                    strftime(time_buf, 100, "%Y-%m-%d %H:%M:%S", std::localtime(&data.last_seen));
                    ImGui::Text("%s", time_buf);
                    ImGui::TableSetColumnIndex(5); ImGui::Text("%s", data.last_value_json.c_str());

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // --- Tab 3: Adapter Management (REDESIGNED) ---
        if (ImGui::BeginTabItem("Adapter Management")) {

            static const char* protocol_types[] = { "ModbusTCP", "OPC-UA", "MQTT", "ZMQ" };
            static int current_protocol_idx = 0;
            static char adapter_name_buf[128] = "ModbusService1";

            // --- Section 1: Add New Adapter Service ---
            ImGui::Text("Add New Adapter Service");
            ImGui::Separator();
            ImGui::InputText("Service Name##AdapterName", adapter_name_buf, IM_ARRAYSIZE(adapter_name_buf));
            ImGui::Combo("Protocol Type##AdapterProto", &current_protocol_idx, protocol_types, IM_ARRAYSIZE(protocol_types));
            if (ImGui::Button("Add Service")) {
                hub.AddAdapter(adapter_name_buf, protocol_types[current_protocol_idx]);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // --- Section 2: Adapter & Device Configuration ---
            ImGui::Text("Adapter Services & Devices");
            ImGui::Separator();

            static std::string selected_adapter_name = "";

            // Use two columns for manager/device layout
            if (ImGui::BeginTable("AdapterLayout", 2, ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Adapter Services", ImGuiTableColumnFlags_WidthFixed, 300.0f);
                ImGui::TableSetupColumn("Device Configuration");
                ImGui::TableNextRow();

                // --- Column 1: List of Adapter Services ---
                ImGui::TableSetColumnIndex(0);
                ImGui::BeginChild("AdapterListChild");

                std::map<std::string, std::string> adapter_statuses = hub.GetAdapterStatuses();
                for (auto const& pair : adapter_statuses) { // Use pair to avoid C++17 binding issues
                    const std::string& name = pair.first;
                    const std::string& status = pair.second;

                    bool is_selected = (name == selected_adapter_name);
                    ImGui::PushID(name.c_str()); // Unique ID for this adapter
                    if (ImGui::Selectable(name.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        selected_adapter_name = name;
                    }
                    ImGui::PopID(); // Pop adapter ID

                    ImGui::TextDisabled("  %s", status.c_str());
                    ImGui::PushID(name.c_str()); // Unique ID for the button
                    if (ImGui::Button("Delete Service")) {
                        hub.RemoveAdapter(name);
                        if (selected_adapter_name == name) selected_adapter_name = "";
                    }
                    ImGui::PopID(); // Pop button ID
                    ImGui::Separator();
                }
                ImGui::EndChild();

                // --- Column 2: Device Config for Selected Adapter ---
                ImGui::TableSetColumnIndex(1);
                ImGui::BeginChild("DeviceConfigChild");

                if (selected_adapter_name.empty()) {
                    ImGui::Text("Select an adapter service from the left to configure devices.");
                }
                else {
                    std::string protocol = hub.GetAdapterProtocol(selected_adapter_name);
                    ImGui::Text("Configuring Devices for: %s (%s)", selected_adapter_name.c_str(), protocol.c_str());
                    ImGui::Separator();

                    // --- Dynamic "Add Device" Form ---
                    ImGui::Text("Add New Device");

                    // Static buffers for the device form
                    static char dev_name_buf[128] = "plc_line_1";
                    // Modbus
                    static char dev_ip_buf[128] = "127.0.0.1";
                    static int dev_port_int = 502;
                    // OPC-UA
                    static char dev_opcua_ep_buf[256] = "opc.tcp://127.0.0.1:4840";
                    static char dev_opcua_nodes_buf[256] = "ns=1;i=2"; // Example nodes
                    // MQTT
                    static char dev_mqtt_broker_buf[256] = "tcp://test.mosquitto.org:1883";
                    static char dev_mqtt_sub_topics_buf[256] = "gateway/data/#";
                    static char dev_mqtt_cmd_topic_buf[256] = "gateway/commands";
                    // ZMQ
                    static char dev_zmq_topic_buf[256] = "gateway_data";


                    ImGui::InputText("Device Name##DevName", dev_name_buf, IM_ARRAYSIZE(dev_name_buf));

                    // --- Form changes based on protocol ---
                    if (protocol == "ModbusTCP") {
                        ImGui::InputText("IP Address##DevIP", dev_ip_buf, IM_ARRAYSIZE(dev_ip_buf));
                        ImGui::InputInt("Port##DevPort", &dev_port_int);
                    }
                    else if (protocol == "OPC-UA") {
                        ImGui::InputText("Endpoint URL##DevOpcuaEP", dev_opcua_ep_buf, IM_ARRAYSIZE(dev_opcua_ep_buf));
                        ImGui::InputText("NodeIDs (comma-sep)##DevOpcuaNodes", dev_opcua_nodes_buf, IM_ARRAYSIZE(dev_opcua_nodes_buf));
                    }
                    else if (protocol == "MQTT") {
                        // THIS IS THE CORRECTED SECTION
                        ImGui::InputText("Broker URI##DevMqttBroker", dev_mqtt_broker_buf, IM_ARRAYSIZE(dev_mqtt_broker_buf));
                        ImGui::InputText("Subscribe Topics##DevMqttSub", dev_mqtt_sub_topics_buf, IM_ARRAYSIZE(dev_mqtt_sub_topics_buf));
                        ImGui::InputText("Command Topic##DevMqttCmd", dev_mqtt_cmd_topic_buf, IM_ARRAYSIZE(dev_mqtt_cmd_topic_buf));
                    }
                    else if (protocol == "ZMQ") {
                        ImGui::InputText("Endpoint##DevEP", dev_ip_buf, IM_ARRAYSIZE(dev_ip_buf)); // Re-using dev_ip_buf
                        ImGui::InputText("Topic##DevTopic", dev_zmq_topic_buf, IM_ARRAYSIZE(dev_zmq_topic_buf));
                    }
                    // ... etc.

                    if (ImGui::Button("Add Device")) {
                        DeviceConfig config;
                        if (protocol == "ModbusTCP") {
                            config["ip"] = dev_ip_buf;
                            config["port"] = std::to_string(dev_port_int);
                        }
                        else if (protocol == "OPC-UA") {
                            config["endpoint"] = dev_opcua_ep_buf;
                            config["nodeIds"] = dev_opcua_nodes_buf;
                        }
                        else if (protocol == "MQTT") {
                            // THIS IS THE CORRECTED SECTION
                            config["brokerUri"] = dev_mqtt_broker_buf;
                            config["subscribeTopics"] = dev_mqtt_sub_topics_buf;
                            config["commandTopic"] = dev_mqtt_cmd_topic_buf;
                        }
                        else if (protocol == "ZMQ") {
                            config["endpoint"] = dev_ip_buf;
                            config["topic"] = dev_zmq_topic_buf;
                        }
                        // ... else if ...
                        hub.AddDeviceToAdapter(selected_adapter_name, dev_name_buf, config);
                    }

                    ImGui::Separator();
                    ImGui::Text("Managed Devices");

                    // --- Table of existing devices for this adapter ---
                    if (ImGui::BeginTable("DeviceListTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                        ImGui::TableSetupColumn("Device Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        std::map<std::string, std::string> device_statuses = hub.GetDeviceStatusesForAdapter(selected_adapter_name);
                        for (auto const& pair : device_statuses) { // Use pair to avoid C++17 binding issues
                            const std::string& name = pair.first;
                            const std::string& status = pair.second;

                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("%s", name.c_str());
                            ImGui::TableSetColumnIndex(1);

                            ImGui::PushID(name.c_str()); // Unique ID for this row
                            ImGui::Text("%s", status.c_str());
                            ImGui::SameLine();
                            if (ImGui::Button("Remove")) {
                                hub.RemoveDeviceFromAdapter(selected_adapter_name, name);
                            }
                            ImGui::PopID(); // Pop row ID
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
            static char adapter_name_buf[128] = "ModbusService1";
            static char command_json_buf[512] = "{\"targetDevice\":\"plc_line_1\", \"command\":\"set_value\", \"payload\": 123}";

            ImGui::Text("Note: Commands are sent to the Adapter Service.");
            ImGui::Text("The service is responsible for routing to the correct device.");

            ImGui::InputText("Adapter Service Name##CmdTarget", adapter_name_buf, IM_ARRAYSIZE(adapter_name_buf));
            ImGui::InputTextMultiline("Command JSON##CmdPayload", command_json_buf, IM_ARRAYSIZE(command_json_buf), ImVec2(-1, 150));

            if (ImGui::Button("Send Command")) {
                hub.SendTestCommand(adapter_name_buf, command_json_buf);
            }
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Note: The adapter's command handler (if implemented)\nmust parse the JSON to find the 'targetDevice'.");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}


// =================================================================================
//
// Main Function (ImGui Setup & Loop)
//
// =================================================================================

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "Glfw Error %d: %s\n", error, description);
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
    // Has to be static or on heap so it lives for the app lifetime
    static GatewayHub hub;
    hub.Start();

    // --- 5. Main Render Loop ---
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Render our UI
        DrawGatewayUI(hub);

        // ImGui Rendering
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
    hub.Stop(); // Signal all threads to stop

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

