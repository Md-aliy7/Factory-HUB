#pragma once

#include "GatewayUI.h"
#include "GatewayHub.h"

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

