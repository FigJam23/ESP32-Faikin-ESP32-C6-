#include <Arduino.h> #include <WiFi.h> #include <HardwareSerial.h> #include <PubSubClient.h> #include <ESPAsyncWebServer.h> #include <AsyncTCP.h> #include <ArduinoJson.h>

// ======== User Configuration ======== const char* ssid = "YOUR_SSID"; const char* password = "YOUR_PASSWORD";

const char* mqtt_server   = "192.168.1.x"; const char* mqtt_user     = "mqtt_user"; const char* mqtt_pass     = "mqtt_password"; const char* status_topic  = "ac/status"; const char* command_topic = "ac/set";

#define RX_PIN     17 #define TX_PIN     16 #define BAUD_RATE  9600

// Serial & network objects HardwareSerial ACSerial(1); WiFiClient    wifiClient; PubSubClient  mqttClient(wifiClient); AsyncWebServer webServer(80); AsyncWebSocket webSocket("/ws");

// Daikin frame buffer enum { BUF_SIZE = 256 }; static uint8_t buffer[BUF_SIZE]; static uint8_t idx = 0;

// Parsed AC status structure struct ACStatus { bool     power; uint8_t  mode; uint8_t  temperature; uint8_t  fan_speed; bool     swing_v; bool     swing_h; } currentStatus;

// ======= Daikin Commands ======= enum DaikinCmd { CMD_POWER            = 0x71, CMD_MODE             = 0x80, CMD_SETPOINT         = 0x40, CMD_FAN_SPEED        = 0x42, CMD_SWING_VERTICAL   = 0x44, CMD_SWING_HORIZONTAL = 0x4E };

// ======= Helper: Build & send Daikin frame ======= void sendACCommand(uint8_t cmd, const uint8_t* payload, uint8_t len) { uint8_t frameLen = len + 6; uint8_t buf[BUF_SIZE]; buf[0] = 0x06; buf[1] = cmd; buf[2] = frameLen; buf[3] = 0x01; buf[4] = (cmd == CMD_SWING_HORIZONTAL) ? 0x12 : 0x06; if (len) memcpy(&buf[5], payload, len); uint8_t sum = 0; for (int i = 0; i < 5 + len; ++i) sum += buf[i]; buf[5 + len] = 0xFF - sum; ACSerial.write(buf, frameLen); }

// ======= Action Wrappers ======= void setPower(bool on)           { uint8_t p[1] = { (uint8_t)on }; sendACCommand(CMD_POWER, p, 1); } void setMode(uint8_t m)          { uint8_t p[1] = { m & 0x0F };      sendACCommand(CMD_MODE, p, 1); } void setTemperature(uint8_t t)   { uint8_t p[1] = { (t-16) & 0x0F }; sendACCommand(CMD_SETPOINT, p, 1); } void setFanSpeed(uint8_t f)      { uint8_t p[1] = { f & 0x07 };      sendACCommand(CMD_FAN_SPEED, p, 1); } void setSwingVertical(bool on)   { uint8_t p[1] = { (uint8_t)on };   sendACCommand(CMD_SWING_VERTICAL, p, 1); } void setSwingHorizontal(bool on) { uint8_t p[1] = { (uint8_t)on };   sendACCommand(CMD_SWING_HORIZONTAL, p, 1); }

// ======= Convert status to JSON ======= String statusToJson() { StaticJsonDocument<128> doc; doc["power"]       = currentStatus.power; doc["mode"]        = currentStatus.mode; doc["temperature"] = currentStatus.temperature; doc["fan_speed"]   = currentStatus.fan_speed; doc["swing_v"]     = currentStatus.swing_v; doc["swing_h"]     = currentStatus.swing_h; String out; serializeJson(doc, out); return out; }

// ======= MQTT Handlers ======= void mqttCallback(char* topic, byte* payload, unsigned int length) { StaticJsonDocument<200> doc; if (deserializeJson(doc, payload, length)) return; if (doc.containsKey("power"))       setPower(doc["power"]); if (doc.containsKey("mode"))        setMode(doc["mode"]); if (doc.containsKey("temperature")) setTemperature(doc["temperature"]); if (doc.containsKey("fan_speed"))   setFanSpeed(doc["fan_speed"]); if (doc.containsKey("swing_v"))     setSwingVertical(doc["swing_v"]); if (doc.containsKey("swing_h"))     setSwingHorizontal(doc["swing_h"]); }

void mqttReconnect() { while (!mqttClient.connected()) { if (mqttClient.connect("ESP32C6_AC", mqtt_user, mqtt_pass)) { mqttClient.subscribe(command_topic); } else delay(5000); } }

// ======= Web UI (AC-style interface) ======= const char index_html[] PROGMEM = R"rawliteral(

<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1"><style>
  body{font-family:sans-serif;background:#222;color:#fff;text-align:center;padding:10px;}
  .display{font-size:24px;margin:10px;padding:10px;border:2px inset #444;border-radius:8px;}
  .grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:10px;}
  button, input[type=range], select{background:#444;color:#fff;border:none;border-radius:4px;padding:12px;font-size:16px;}
  button:hover{background:#555;}
</style></head><body><h2>Daikin AC Controller</h2>
<div class="display" id="disp">-- °</div>
<div class="grid">
  <button onclick="ctrl({power:1})">On</button>
  <button onclick="ctrl({power:0})">Off</button>
  <select onchange="ctrl({mode:parseInt(this.value)})">
    <option value="0">Auto</option>
    <option value="1">Heat</option>
    <option value="2">Cool</option>
    <option value="3">Dry</option>
    <option value="4">Fan</option>
  </select>
  <input type="range" min="16" max="30" value="22" oninput="updateTemp(this.value)" onchange="ctrl({temperature:parseInt(this.value)})">
  <input type="range" min="0" max="5" value="0" oninput="updateFan(this.value)" onchange="ctrl({fan_speed:parseInt(this.value)})">
  <button onclick="ctrl({swing_v:1})">V Swing On</button>
  <button onclick="ctrl({swing_v:0})">V Swing Off</button>
  <button onclick="ctrl({swing_h:1})">H Swing On</button>
  <button onclick="ctrl({swing_h:0})">H Swing Off</button>
</div>
<script>
  let ws = new WebSocket('ws://'+location.host+'/ws');
  ws.onmessage = e => {
    let s = JSON.parse(e.data);
    document.getElementById('disp').innerText = (s.power? '●':'○') + ' ' + ['Auto','Heat','Cool','Dry','Fan'][s.mode] + ' ' + s.temperature + '°C F' + s.fan_speed;
    document.querySelector('input[type=range]').value = s.temperature;
    document.querySelectorAll('input[type=range]')[1].value = s.fan_speed;
  };
  function ctrl(cmd){ fetch('/control',{method:'POST',body:JSON.stringify(cmd)}); }
  function updateTemp(v){ document.getElementById('disp').innerText = document.getElementById('disp').innerText.replace(/\d+°C/,v+'°C'); }
  function updateFan(v){ /* optional show */ }
</script></body></html>
)rawliteral";void onWsEvent(AsyncWebSocket *s, AsyncWebSocketClient *c, AwsEventType t, void *arg, uint8_t *d, size_t len){ if (t == WS_EVT_CONNECT) c->text(statusToJson()); }

// ======== Setup ======== void setup(){ Serial.begin(115200); ACSerial.begin(BAUD_RATE, SERIAL_8E1, RX_PIN, TX_PIN); WiFi.begin(ssid,password); while(WiFi.status()!=WL_CONNECTED) delay(500); mqttClient.setServer(mqtt_server,1883); mqttClient.setCallback(mqttCallback);

webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ r->send_P(200,"text/html",index_html); }); webServer.on("/control", HTTP_POST, [](AsyncWebServerRequest *r){ if(r->hasParam("body",true)){ String b=r->getParam("body",true)->value(); StaticJsonDocument<200> doc; if(!deserializeJson(doc,b)){ if(doc.containsKey("power")) setPower(doc["power"]); if(doc.containsKey("mode")) setMode(doc["mode"]); if(doc.containsKey("temperature")) setTemperature(doc["temperature"]); if(doc.containsKey("fan_speed")) setFanSpeed(doc["fan_speed"]); if(doc.containsKey("swing_v")) setSwingVertical(doc["swing_v"]); if(doc.containsKey("swing_h")) setSwingHorizontal(doc["swing_h"]); } } r->send(200); }); webSocket.onEvent(onWsEvent); webServer.addHandler(&webSocket); webServer.begin(); }

// ======== Main Loop ======== void loop(){ if(!mqttClient.connected()) mqttReconnect(); mqttClient.loop(); while(ACSerial.available()){ uint8_t b=ACSerial.read(); if(idx==0 && b!=0x06) continue; buffer[idx++]=b; if(idx>=3 && idx==buffer[2]){ uint8_t sum=0; for(uint8_t i=0;i<idx;i++) sum+=buffer[i]; if(sum==0xFF){ uint8_t cmd=buffer[1]; uint8_t *p=&buffer[5]; switch(cmd){ case CMD_POWER: currentStatus.power=p[0]; break; case CMD_MODE: currentStatus.mode=p[0]; break; case CMD_SETPOINT: currentStatus.temperature=p[0]+16; break; case CMD_FAN_SPEED: currentStatus.fan_speed=p[0]; break; case CMD_SWING_VERTICAL: currentStatus.swing_v=p[0]; break; case CMD_SWING_HORIZONTAL: currentStatus.swing_h=p[0]; break; } String j=statusToJson(); mqttClient.publish(status_topic,j.c_str()); webSocket.textAll(j); } idx=0; } } }


