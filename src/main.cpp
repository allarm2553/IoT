#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PicoMQTT.h>
#include <ESPmDNS.h>

// MicroLink C Component (Tailscale Client)
extern "C" {
  #include "microlink.h"
}

// ==========================================
// 1. Hardware Pin Definitions
// ==========================================
const int LED_PIN = 48;           // GPIO 48: Built-in RGB LED on ESP32-S3 DevKit (RGB@IO48)
const int RESET_BTN_PIN = 0;      // GPIO 0: BOOT button
const int OLED_SDA_PIN = 8;       // I2C SDA = 8
const int OLED_SCL_PIN = 9;       // I2C SCL = 9

// ==========================================
// 2. Configuration Parameters
// ==========================================
String wifi_ssid = "";
String wifi_pass = "";
String node_id = "esp32-gateway";
String tailscale_auth_key = "";   // Tailscale Auth Key (tskey-auth-...)
String registered_auth_key = "";  // Last registered Auth Key (NVS checker)
String cloud_server = "broker.emqx.io";
int cloud_port = 1883;
String cloud_user = "allarm";
String cloud_pass = "123456";

bool isAPMode = false;
bool isTailscaleStarted = false;

// MicroLink instance pointer
microlink_t *ml = nullptr;

// ==========================================
// 3. OLED Display Config (SSD1306)
// ==========================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ==========================================
// 4. Dynamic Sensor Nodes Structures
// ==========================================
#define MAX_SENSORS_PER_NODE 8
#define MAX_NODES 8

struct SensorReading {
  String key;
  float value = 0.0;
  bool active = false;
};

struct Node {
  String id;
  SensorReading sensors[MAX_SENSORS_PER_NODE];
  bool out_states[5] = {false, false, false, false, false};
  unsigned long last_seen = 0;
  bool active = false;
};

Node nodes[MAX_NODES];
int active_node_count = 0;

// Search or register a client node
int getOrRegisterNode(String nodeId) {
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].active && nodes[i].id == nodeId) {
      nodes[i].last_seen = millis();
      return i;
    }
  }
  for (int i = 0; i < MAX_NODES; i++) {
    if (!nodes[i].active) {
      nodes[i].id = nodeId;
      nodes[i].active = true;
      for (int j = 0; j < MAX_SENSORS_PER_NODE; j++) {
        nodes[i].sensors[j].active = false;
      }
      memset(nodes[i].out_states, 0, sizeof(nodes[i].out_states));
      nodes[i].last_seen = millis();
      active_node_count++;
      return i;
    }
  }
  return -1;
}

// Update sensor readings dynamically
void updateNodeSensor(int nodeIdx, String sensorKey, float val) {
  for (int i = 0; i < MAX_SENSORS_PER_NODE; i++) {
    if (nodes[nodeIdx].sensors[i].active && nodes[nodeIdx].sensors[i].key == sensorKey) {
      nodes[nodeIdx].sensors[i].value = val;
      return;
    }
  }
  for (int i = 0; i < MAX_SENSORS_PER_NODE; i++) {
    if (!nodes[nodeIdx].sensors[i].active) {
      nodes[nodeIdx].sensors[i].key = sensorKey;
      nodes[nodeIdx].sensors[i].value = val;
      nodes[nodeIdx].sensors[i].active = true;
      Serial.printf("[GATEWAY] Node '%s' registered new sensor key: '%s' = %.2f\n", 
                    nodes[nodeIdx].id.c_str(), sensorKey.c_str(), val);
      return;
    }
  }
}

// ==========================================
// 5. PicoMQTT Declarations
// ==========================================
PicoMQTT::Server* local_broker = nullptr;
PicoMQTT::Client* cloud_client = nullptr;
WebServer server(80);

// ==========================================
// 6. RGB LED Debug Helper (neopixelWrite)
// ==========================================
void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(LED_PIN, r, g, b);
}

// ==========================================
// 7. OLED Display Helper
// ==========================================
void updateOLED(String statusMsg) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  
  if (isAPMode) {
    display.println("--- SETUP PORTAL ---");
    display.setCursor(0, 16);
    display.print("SSID: Gateway-");
    display.println(node_id);
    display.setCursor(0, 28);
    display.print("IP: 192.168.4.1");
  } else {
    display.println("--- MQTT & VPN GW ---");
    display.setCursor(0, 14);
    display.print("IP: ");
    display.println(WiFi.localIP().toString());
    
    display.setCursor(0, 24);
    display.print("VPN: ");
    if (isTailscaleStarted && ml != nullptr && microlink_is_connected(ml)) {
      uint32_t vpn_ip = microlink_get_vpn_ip(ml);
      char ip_str[16];
      microlink_ip_to_str(vpn_ip, ip_str);
      display.println(ip_str);
    } else {
      display.println("Connecting...");
    }
    
    display.setCursor(0, 36);
    display.printf("Nodes Active: %d/%d", active_node_count, MAX_NODES);
  }
  
  display.setCursor(0, 46);
  if (!isAPMode && active_node_count > 0) {
    for (int i = 0; i < MAX_NODES; i++) {
      if (nodes[i].active) {
        String firstSensorName = "";
        float firstSensorVal = 0;
        for (int j = 0; j < MAX_SENSORS_PER_NODE; j++) {
          if (nodes[i].sensors[j].active) {
            firstSensorName = nodes[i].sensors[j].key;
            firstSensorVal = nodes[i].sensors[j].value;
            break;
          }
        }
        if (firstSensorName != "") {
          display.printf("%s:%s=%.1f", nodes[i].id.c_str(), firstSensorName.c_str(), firstSensorVal);
        } else {
          display.printf("Node %s online", nodes[i].id.c_str());
        }
        break;
      }
    }
  } else if (!isAPMode) {
    display.print("Waiting for Nodes...");
  } else {
    display.print("Connect & configure");
  }
  
  display.setCursor(0, 56);
  display.print("Status: ");
  display.print(statusMsg);
  
  display.display();
}

// ==========================================
// 8. Configuration Load/Save from LittleFS
// ==========================================
void loadConfig() {
  if (LittleFS.exists("/config.txt")) {
    File configFile = LittleFS.open("/config.txt", "r");
    if (configFile) {
      while (configFile.available()) {
        String line = configFile.readStringUntil('\n');
        line.trim();
        int separatorIdx = line.indexOf('=');
        if (separatorIdx != -1) {
          String key = line.substring(0, separatorIdx);
          String val = line.substring(separatorIdx + 1);
          
          if (key == "wifi_ssid") wifi_ssid = val;
          else if (key == "wifi_pass") wifi_pass = val;
          else if (key == "node_id") node_id = val;
          else if (key == "tailscale_auth_key") tailscale_auth_key = val;
          else if (key == "registered_auth_key") registered_auth_key = val;
          else if (key == "cloud_server") cloud_server = val;
          else if (key == "cloud_port") cloud_port = val.toInt();
          else if (key == "cloud_user") cloud_user = val;
          else if (key == "cloud_pass") cloud_pass = val;
        }
      }
      configFile.close();
      Serial.println("[SYSTEM] Configuration loaded successfully.");
    }
  } else {
    Serial.println("[SYSTEM] Configuration file not found. Using defaults.");
  }
}

void saveConfig() {
  File configFile = LittleFS.open("/config.txt", "w");
  if (configFile) {
    configFile.println("wifi_ssid=" + wifi_ssid);
    configFile.println("wifi_pass=" + wifi_pass);
    configFile.println("node_id=" + node_id);
    configFile.println("tailscale_auth_key=" + tailscale_auth_key);
    configFile.println("registered_auth_key=" + registered_auth_key);
    configFile.println("cloud_server=" + cloud_server);
    configFile.println("cloud_port=" + String(cloud_port));
    configFile.println("cloud_user=" + cloud_user);
    configFile.println("cloud_pass=" + cloud_pass);
    configFile.close();
    Serial.println("[SYSTEM] Configuration saved successfully.");
  } else {
    Serial.println("[SYSTEM] Failed to write configuration file.");
  }
}

// ==========================================
// 9. WiFi Portal Configuration Mode
// ==========================================
void startAPMode() {
  isAPMode = true;
  Serial.println("[WIFI] Switching to AP Mode. Stopping active connection attempts...");
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_AP_STA);
  delay(200);
  String apName = "ESP32-S3-Gateway-" + node_id;
  WiFi.softAP(apName.c_str(), "");
  delay(100);
  
  Serial.println("[WIFI] Access Point Setup Portal Started!");
  Serial.print("[WIFI] SSID: ");
  Serial.println(apName);
  Serial.print("[WIFI] IP Address: ");
  Serial.println(WiFi.softAPIP());

  updateOLED("AP Setup Mode");
}

void setup_wifi() {
  loadConfig();
  
  if (wifi_ssid == "") {
    Serial.println("[WIFI] No WiFi SSID configured. Entering AP Mode.");
    startAPMode();
    return;
  }
  
  Serial.printf("[WIFI] Connecting to SSID: %s\n", wifi_ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());

  int counter = 0;
  updateOLED("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    // Fast blinking Orange/Yellow during Wifi connection
    setLedColor(128, 64, 0);
    delay(250);
    setLedColor(0, 0, 0);
    delay(250);
    Serial.print(".");
    counter++;
    if (counter > 30) {
      Serial.println("\n[WIFI] Connection failed. Switching to AP Setup Portal.");
      startAPMode();
      return;
    }
  }
  
  setLedColor(0, 128, 0); // Solid green for successful connection
  Serial.println("\n[WIFI] Connected successfully.");
  Serial.print("[WIFI] IP Address: ");
  Serial.println(WiFi.localIP());
  updateOLED("WiFi Connected");
}

// ==========================================
// 10. Tailscale Setup
// ==========================================
void setupTailscale() {
  if (tailscale_auth_key == "" || tailscale_auth_key.startsWith("tskey-auth-yourkey")) {
    Serial.println("[TAILSCALE] Missing or default Auth Key. Skipping VPN connection.");
    return;
  }

  Serial.println("[TAILSCALE] Initializing Tailscale client...");
  
  microlink_config_t config;
  memset(&config, 0, sizeof(config));
  config.auth_key = tailscale_auth_key.c_str();
  config.device_name = node_id.c_str();
  config.enable_derp = true;
  config.enable_disco = true;
  config.enable_stun = true;
  config.max_peers = 16;

  ml = microlink_init(&config);
  
  if (ml != nullptr) {
    Serial.println("[TAILSCALE] MicroLink Initialized. Starting VPN Tunnel...");
    microlink_start(ml);
    isTailscaleStarted = true;
  } else {
    Serial.println("[TAILSCALE] Failed to initialize MicroLink.");
  }
}

void printTailscaleStatus() {
  if (!isTailscaleStarted || ml == nullptr) return;

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 5000) {
    lastPrint = millis();

    bool connected = microlink_is_connected(ml);
    uint32_t vpn_ip = microlink_get_vpn_ip(ml);
    
    char ip_str[16];
    microlink_ip_to_str(vpn_ip, ip_str);

    Serial.printf("[TAILSCALE] Status: %s | VPN IP: %s\n", 
                  connected ? "CONNECTED (ONLINE)" : "CONNECTING...", ip_str);
  }
}

// ==========================================
// 11. Hardware Debug Indicators & Resets
// ==========================================
void handleLedIndicator() {
  if (isAPMode) {
    // Blinking blue in AP configuration mode
    static unsigned long lastBlink = 0;
    static bool ledState = false;
    if (millis() - lastBlink > 500) {
      lastBlink = millis();
      ledState = !ledState;
      if (ledState) setLedColor(0, 0, 128);
      else setLedColor(0, 0, 0);
    }
  } else if (WiFi.status() != WL_CONNECTED) {
    // Blinking orange/yellow when disconnected
    static unsigned long lastBlink = 0;
    static bool ledState = false;
    if (millis() - lastBlink > 500) {
      lastBlink = millis();
      ledState = !ledState;
      if (ledState) setLedColor(128, 64, 0);
      else setLedColor(0, 0, 0);
    }
  } else {
    // Connected to WiFi. Check Tailscale connection status.
    if (isTailscaleStarted && ml != nullptr && microlink_is_connected(ml)) {
      setLedColor(0, 128, 128); // Solid Cyan when fully connected online via VPN
    } else {
      setLedColor(0, 128, 0);   // Solid Green when connected to local WiFi but VPN is inactive/connecting
    }
  }
}

void handleResetButton() {
  static unsigned long btnPressStart = 0;
  static bool btnWasPressed = false;
  
  if (digitalRead(RESET_BTN_PIN) == LOW) {
    if (!btnWasPressed) {
      btnPressStart = millis();
      btnWasPressed = true;
      Serial.println("\n[SYSTEM] Reset button pressed. Hold for 3 seconds to clear config...");
    } else {
      unsigned long pressDuration = millis() - btnPressStart;
      
      if (pressDuration > 3000) {
        Serial.println("\n[SYSTEM] Factory Reset Triggered! Erasing configurations...");
        
        // Show immediate visual and screen feedback
        updateOLED("Factory Resetting");
        
        // Delete LittleFS configuration file
        if (LittleFS.exists("/config.txt")) {
          LittleFS.remove("/config.txt");
        }
        
        // Wipe local variables
        wifi_ssid = "";
        wifi_pass = "";
        tailscale_auth_key = "";
        registered_auth_key = "";
        
        // Reset Tailscale keys in NVS namespaces
        microlink_factory_reset();
        
        // Fast blinking red LED indicator
        for (int i = 0; i < 20; i++) {
          setLedColor(255, 0, 0);
          delay(50);
          setLedColor(0, 0, 0);
          delay(50);
        }
        
        Serial.println("[SYSTEM] Configuration erased. Rebooting...");
        delay(500);
        ESP.restart();
      } else {
        // Warning fast blink when holding the reset button
        static unsigned long lastFastBlink = 0;
        if (millis() - lastFastBlink > 100) {
          lastFastBlink = millis();
          static bool fs = false;
          fs = !fs;
          if (fs) setLedColor(255, 128, 0);
          else setLedColor(0, 0, 0);
        }
      }
    }
  } else {
    if (btnWasPressed) {
      Serial.println("[SYSTEM] Reset button released. Cancelled.");
      btnWasPressed = false;
      // Restore LED based on connection status
      handleLedIndicator();
    }
  }
}

// ==========================================
// 12. HTTP Web API Route Handlers
// ==========================================
void handleRoot() {
  File file = LittleFS.open("/index.html", "r");
  if (!file) {
    server.send(404, "text/plain", "index.html not found! Please upload LittleFS image.");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

void handleCSS() {
  File file = LittleFS.open("/index.css", "r");
  if (!file) {
    server.send(404, "text/plain", "index.css not found!");
    return;
  }
  server.streamFile(file, "text/css");
  file.close();
}

void handleGetStatus() {
  String ipStr = "0.0.0.0";
  bool tsConnected = false;
  
  if (ml != nullptr) {
    tsConnected = microlink_is_connected(ml);
    uint32_t vpn_ip = microlink_get_vpn_ip(ml);
    char buf[16];
    microlink_ip_to_str(vpn_ip, buf);
    ipStr = String(buf);
  }

  String json = "{";
  json += "\"mode\":\"" + String(isAPMode ? "AP" : "STA") + "\",";
  
  json += "\"config\":{";
  json += "\"ssid\":\"" + wifi_ssid + "\",";
  json += "\"node_id\":\"" + node_id + "\",";
  json += "\"tailscale_key_set\":" + String(tailscale_auth_key.length() > 0 ? "true" : "false") + ",";
  json += "\"cloud_server\":\"" + cloud_server + "\",";
  json += "\"cloud_port\":" + String(cloud_port) + ",";
  json += "\"cloud_user\":\"" + cloud_user + "\"";
  json += "},";
  
  json += "\"tailscale\":{";
  json += "\"connected\":" + String(tsConnected ? "true" : "false") + ",";
  json += "\"vpn_ip\":\"" + ipStr + "\"";
  json += "},";

  json += "\"nodes\":[";
  if (!isAPMode) {
    bool firstNode = true;
    for (int i = 0; i < MAX_NODES; i++) {
      if (nodes[i].active) {
        if (!firstNode) json += ",";
        firstNode = false;
        
        bool isOnline = (millis() - nodes[i].last_seen < 30000);
        
        json += "{";
        json += "\"id\":\"" + nodes[i].id + "\",";
        json += "\"online\":" + String(isOnline ? "true" : "false") + ",";
        
        json += "\"sensors\":{";
        bool firstSensor = true;
        for (int j = 0; j < MAX_SENSORS_PER_NODE; j++) {
          if (nodes[i].sensors[j].active) {
            if (!firstSensor) json += ",";
            firstSensor = false;
            json += "\"" + nodes[i].sensors[j].key + "\":" + String(nodes[i].sensors[j].value);
          }
        }
        json += "},";
        
        json += "\"out\":[";
        for (int j = 0; j < 5; j++) {
          json += String(nodes[i].out_states[j] ? 1 : 0);
          if (j < 4) json += ",";
        }
        json += "]";
        json += "}";
      }
    }
  }
  json += "]}";
  
  server.send(200, "application/json", json);
}

void handleSaveConfig() {
  if (!server.hasArg("ssid") || !server.hasArg("node_id") || server.arg("ssid") == "") {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing required fields\"}");
    return;
  }

  wifi_ssid = server.arg("ssid");
  if (server.hasArg("pass")) wifi_pass = server.arg("pass");
  node_id = server.arg("node_id");

  // Only overwrite Tailscale key if a new one is provided
  if (server.hasArg("tailscale_key") && server.arg("tailscale_key") != "") {
    tailscale_auth_key = server.arg("tailscale_key");
  }
  
  // Save cloud broker configurations
  if (server.hasArg("server")) cloud_server = server.arg("server");
  if (server.hasArg("port")) cloud_port = server.arg("port").toInt();
  if (server.hasArg("user")) cloud_user = server.arg("user");
  if (server.hasArg("cpass")) cloud_pass = server.arg("cpass");

  saveConfig();
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Rebooting gateway...\"}");
  delay(500);
  ESP.restart();
}

void handleScanWiFi() {
  Serial.println("[WIFI] Scanning for networks...");
  int n = WiFi.scanNetworks();
  Serial.printf("[WIFI] Scan complete. Found %d networks.\n", n);
  
  String json = "[";
  for (int i = 0; i < n; ++i) {
    if (i > 0) json += ",";
    json += "\"" + WiFi.SSID(i) + "\"";
  }
  json += "]";
  
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

void handleControl() {
  if (!isAPMode && server.hasArg("node") && server.hasArg("out") && server.hasArg("state")) {
    String nodeId = server.arg("node");
    int outNum = server.arg("out").toInt();
    String stateVal = server.arg("state");
    
    if (outNum >= 1 && outNum <= 5) {
      int nodeIdx = -1;
      for (int i = 0; i < MAX_NODES; i++) {
        if (nodes[i].active && nodes[i].id == nodeId) {
          nodeIdx = i;
          break;
        }
      }
      
      if (nodeIdx != -1 && local_broker && cloud_client) {
        String cmdTopic = "factory/" + nodeId + "/cmd/out" + String(outNum);
        local_broker->publish(cmdTopic.c_str(), stateVal.c_str());
        
        nodes[nodeIdx].out_states[outNum - 1] = (stateVal == "1");
        
        String stateTopic = "factory/" + nodeId + "/state/out" + String(outNum);
        cloud_client->publish(stateTopic.c_str(), stateVal.c_str());

        server.send(200, "application/json", "{\"status\":\"success\"}");
        return;
      }
    }
  }
  server.send(400, "application/json", "{\"status\":\"error\"}");
}

// ==========================================
// 13. System Initialization (Setup)
// ==========================================
void setup() {
  Serial.begin(115200);

  // Wait for Serial Monitor to open and show countdown
  for (int i = 5; i > 0; i--) {
    Serial.printf("[SYSTEM] Starting in %d seconds...\n", i);
    delay(1000);
  }

  // Configure reset button
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);

  // Initialize RGB LED (off initially)
  setLedColor(0, 0, 0);

  // Initialize I2C and SSD1306 Display
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

  // Scan I2C bus for debugging
  Serial.println("\n[I2C] Scanning I2C bus...");
  byte error, address;
  int nDevices = 0;
  for (address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0) {
      Serial.printf("[I2C] Device found at address 0x%02X\n", address);
      nDevices++;
    } else if (error == 4) {
      Serial.printf("[I2C] Unknown error at address 0x%02X\n", address);
    }
  }
  if (nDevices == 0) {
    Serial.println("[I2C] No I2C devices found!\n");
  } else {
    Serial.printf("[I2C] Scan complete. Found %d device(s).\n\n", nDevices);
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("[SYSTEM] SSD1306 OLED allocation failed"));
  } else {
    display.clearDisplay();
    display.display();
    Serial.println(F("[SYSTEM] SSD1306 OLED initialized."));
  }

  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("[SYSTEM] Error mounting LittleFS");
    updateOLED("LittleFS Err");
  } else {
    Serial.println("[SYSTEM] LittleFS mounted successfully.");
  }

  // Connect to WiFi and load parameters
  setup_wifi();

  if (!isAPMode) {
    // Check if Tailscale Auth Key has changed
    if (tailscale_auth_key != registered_auth_key) {
      Serial.println("[TAILSCALE] Auth Key change detected. Resetting NVS keys...");
      updateOLED("Resetting VPN");
      microlink_factory_reset();
      registered_auth_key = tailscale_auth_key;
      saveConfig();
    }

    if (MDNS.begin(node_id.c_str())) {
      Serial.printf("[mDNS] Hostname Registered: %s.local\n", node_id.c_str());
      MDNS.addService("mqtt", "tcp", 1883);
      MDNS.addService("http", "tcp", 80);
    }
    
    // Setup Tailscale VPN Client
    setupTailscale();

    // Initialize PicoMQTT Local Broker and Cloud Client Bridge
    local_broker = new PicoMQTT::Server(1883);
    cloud_client = new PicoMQTT::Client(cloud_server.c_str(), cloud_port, nullptr, cloud_user.c_str(), cloud_pass.c_str());

    local_broker->subscribe("factory/#", [](const char* topic, const char* payload) {
      String t = topic;
      String p = payload;
      
      if (t.startsWith("factory/")) {
        int secondSlash = t.indexOf('/', 8);
        if (secondSlash != -1) {
          String nodeId = t.substring(8, secondSlash);
          String subTopic = t.substring(secondSlash + 1);
          
          int idx = getOrRegisterNode(nodeId);
          if (idx != -1) {
            // Check dynamic sensors: factory/<node_id>/sensor/<sensor_key>
            if (subTopic.startsWith("sensor/")) {
              String sensorKey = subTopic.substring(7);
              float val = p.toFloat();
              updateNodeSensor(idx, sensorKey, val);
            } 
            // Check output states
            else if (subTopic.startsWith("state/out")) {
              int outNum = subTopic.substring(9).toInt();
              if (outNum >= 1 && outNum <= 5) {
                nodes[idx].out_states[outNum - 1] = (p == "1" || p == "ON" || p == "on");
              }
            }
          }
        }
      }

      // Forward to Cloud Client
      if (cloud_client && (t.indexOf("/sensor/") != -1 || t.indexOf("/state/") != -1)) {
        cloud_client->publish(topic, payload);
      }
    });

    cloud_client->subscribe("factory/+/cmd/#", [](const char* topic, const char* payload) {
      // Forward to Local Broker
      if (local_broker) {
        local_broker->publish(topic, payload);
      }
      
      String t = topic;
      if (t.startsWith("factory/")) {
        int secondSlash = t.indexOf('/', 8);
        if (secondSlash != -1) {
          String nodeId = t.substring(8, secondSlash);
          String subTopic = t.substring(secondSlash + 1);
          
          int idx = getOrRegisterNode(nodeId);
          if (idx != -1 && subTopic.startsWith("cmd/out")) {
            int outNum = subTopic.substring(7).toInt();
            if (outNum >= 1 && outNum <= 5) {
              nodes[idx].out_states[outNum - 1] = (strcmp(payload, "1") == 0);
              if (cloud_client) {
                String stateTopic = "factory/" + nodeId + "/state/out" + String(outNum);
                cloud_client->publish(stateTopic.c_str(), payload);
              }
            }
          }
        }
      }
    });

    local_broker->begin();
    cloud_client->begin();
    updateOLED("Ready");
  }

  // Setup Web Server Endpoints
  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.css", HTTP_GET, handleCSS);
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/api/save_config", HTTP_POST, handleSaveConfig);
  server.on("/api/save_config", HTTP_GET, handleSaveConfig);
  server.on("/api/scan", HTTP_GET, handleScanWiFi);
  server.on("/api/control", HTTP_POST, handleControl);
  server.on("/api/control", HTTP_GET, handleControl);
  
  server.begin();
  Serial.println("[SYSTEM] HTTP Web Server started.");
}

void checkConnectionRecovery() {
  if (isAPMode) return;

  static unsigned long lastCheck = 0;
  static unsigned long disconnectTime = 0;
  static bool wasConnected = true;
  unsigned long now = millis();

  if (now - lastCheck < 5000) return; // ตรวจสอบทุกๆ 5 วินาที
  lastCheck = now;

  if (WiFi.status() == WL_CONNECTED) {
    if (!wasConnected) {
      Serial.println("[SYSTEM] WiFi Reconnected successfully!");
      updateOLED("WiFi Recovered");
      
      // สั่ง Rebind VPN Sockets ของ Tailscale ใหม่เพื่อให้รับส่งข้อมูลต่อได้ทันที
      if (isTailscaleStarted && ml != nullptr) {
        Serial.println("[TAILSCALE] Rebinding VPN sockets to new connection...");
        esp_err_t err = microlink_rebind(ml);
        if (err == ESP_OK) {
          Serial.println("[TAILSCALE] VPN Rebind successful.");
        } else {
          Serial.printf("[TAILSCALE] VPN Rebind failed: %d. Restarting Tailscale...\n", err);
          microlink_stop(ml);
          setupTailscale();
        }
      }
      wasConnected = true;
      disconnectTime = 0;
    }
  } else {
    // WiFi หลุดการเชื่อมต่อ
    if (wasConnected) {
      Serial.println("[SYSTEM] WiFi Disconnected!");
      disconnectTime = now;
      wasConnected = false;
      updateOLED("WiFi Lost");
    }

    unsigned long offlineDuration = now - disconnectTime;
    Serial.printf("[SYSTEM] WiFi is offline for %lu seconds. Attempting reconnect...\n", offlineDuration / 1000);
    
    // พยายามเชื่อมต่อใหม่ทุกๆ 15 วินาที
    if (offlineDuration % 15000 < 5000) {
      WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    }

    // หากหลุดเกิน 60 วินาที ให้ทำ Hard Reset ตัว WiFi Stack เพื่อแก้ปัญหา Driver ค้าง
    if (offlineDuration > 60000) {
      Serial.println("[SYSTEM] WiFi offline for >60s. Hard resetting WiFi stack...");
      WiFi.disconnect(true);
      delay(200);
      WiFi.mode(WIFI_OFF);
      delay(200);
      WiFi.mode(WIFI_STA);
      WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
      disconnectTime = now; // รีเซ็ตเวลาเริ่มต้นนับใหม่
    }
  }
}

// ==========================================
// 14. Main Execution Loop
// ==========================================
void loop() {
  handleLedIndicator();
  handleResetButton();

  if (!isAPMode) {
    checkConnectionRecovery();
    if (local_broker) local_broker->loop();
    if (cloud_client) cloud_client->loop();
    
    printTailscaleStatus();

    static unsigned long lastOledUpdate = 0;
    if (millis() - lastOledUpdate > 5000) {
      lastOledUpdate = millis();
      updateOLED("Monitoring");
    }
  } else {
    static unsigned long lastOledUpdate = 0;
    if (millis() - lastOledUpdate > 3000) {
      lastOledUpdate = millis();
      updateOLED("AP Config Open");
    }
  }
  
  server.handleClient();
}
