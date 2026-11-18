//----------------------------------------Include the ESP32 WiFi and WebServer Libraries
#include <WiFi.h>
#include <WebServer.h>
//----------------------------------------Include the ThingSpeak Library, Download here : https://github.com/mathworks/thingspeak-arduino
#include "ThingSpeak.h"

#define ON_Board_LED 2   //--> Onboard LED (GPIO 2), WiFi connection indicator
#define LIGHT 23         //--> LIGHT/Relay pin (GPIO 23)

//----------------------------------------Relay settings (change these if the bulb behaves wrongly)
//--> RELAY_ACTIVE_LOW = true  : relay turns ON when the pin is LOW (most blue relay boards)
//--> RELAY_ACTIVE_LOW = false : relay turns ON when the pin is HIGH
#define RELAY_ACTIVE_LOW false

//--> OFF_BY_FLOATING = true : for active-LOW boards, "OFF" releases the pin (INPUT) instead of driving HIGH.
//-->                          Fixes boards that stay ON because 3.3V HIGH is not enough to switch them off.
#define OFF_BY_FLOATING true

//----------------------------------------WiFi credentials
const char* ssid     = "";
const char* password = "";

//----------------------------------------ThingSpeak
unsigned long channel = ;
const char* readAPIKey  = "";
const char* writeAPIKey = "";  //--> Paste your channel's WRITE API key here
unsigned int field1 = 1;

WiFiClient client;
WebServer server(80);  //--> Web server on port 80

//----------------------------------------State shared with the web page
float lastLightState = -1;       //--> -1 = no data yet
int lastStatusCode = 0;
unsigned long lastReadTime = 0;  //--> millis() of last ThingSpeak read
const unsigned long READ_INTERVAL = 15000;  //--> ThingSpeak free accounts: 15 s

//----------------------------------------Switch the relay ON/OFF in one place
void relayWrite(bool on) {
  if (RELAY_ACTIVE_LOW) {
    if (on) {
      pinMode(LIGHT, OUTPUT);
      digitalWrite(LIGHT, LOW);         //--> ON = pull pin LOW
    } else {
      if (OFF_BY_FLOATING) {
        pinMode(LIGHT, INPUT);          //--> OFF = release the pin (relay board pulls it HIGH)
      } else {
        pinMode(LIGHT, OUTPUT);
        digitalWrite(LIGHT, HIGH);      //--> OFF = drive pin HIGH
      }
    }
  } else {
    pinMode(LIGHT, OUTPUT);
    digitalWrite(LIGHT, on ? HIGH : LOW);
  }
}

//----------------------------------------Web page (HTML + JS served from the ESP32)
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Light Monitor</title>
  <style>
    body { font-family: Arial, sans-serif; background:#111; color:#eee; text-align:center; margin:0; padding:30px 15px; }
    h1 { font-size:1.5rem; }
    .bulb { width:120px; height:120px; border-radius:50%; margin:25px auto; background:#333; border:4px solid #555; transition:all .4s; }
    .bulb.on { background:#ffd93b; border-color:#ffe98a; box-shadow:0 0 50px 15px rgba(255,217,59,.6); }
    .state { font-size:2rem; font-weight:bold; }
    .info { color:#999; margin-top:12px; font-size:.95rem; }
    .err { color:#ff6b6b; }
    button { padding:12px 28px; font-size:1.1rem; margin:5px; border:none; border-radius:8px; cursor:pointer; }
    .on-btn { background:#2ecc71; color:#fff; }
    .off-btn { background:#e74c3c; color:#fff; }
  </style>
</head>
<body>
  <h1>ESP32 + ThingSpeak Light</h1>
  <div id="bulb" class="bulb"></div>
  <div id="state" class="state">Loading...</div>
  <div class="info">ThingSpeak Field1: <span id="val">--</span></div>
  <div class="info">Last read: <span id="ago">--</span></div>
  <div class="info" id="msg"></div>

  <div style="margin-top:25px">
    <button class="on-btn" onclick="setLight(1)">ON</button>
    <button class="off-btn" onclick="setLight(0)">OFF</button>
  </div>

  <script>
    async function update() {
      try {
        const r = await fetch('/status');
        const d = await r.json();
        const bulb = document.getElementById('bulb');
        const st = document.getElementById('state');
        if (d.http === 200) {
          bulb.className = 'bulb' + (d.light === 1 ? ' on' : '');
          st.textContent = d.light === 1 ? 'LIGHT is ON' : 'LIGHT is OFF';
        } else {
          st.textContent = 'No data';
          document.getElementById('msg').innerHTML = '<span class="err">ThingSpeak read error, HTTP ' + d.http + '</span>';
        }
        document.getElementById('val').textContent = d.light;
        document.getElementById('ago').textContent = d.ago_s + ' s ago';
      } catch (e) {
        document.getElementById('state').textContent = 'ESP32 unreachable';
      }
    }

    async function setLight(v) {
      const msg = document.getElementById('msg');
      msg.textContent = 'Sending...';
      try {
        const r = await fetch('/set?state=' + v);
        const t = await r.text();
        msg.innerHTML = r.ok ? '' : '<span class="err">' + t + '</span>';
      } catch (e) {
        msg.innerHTML = '<span class="err">Request failed</span>';
      }
      setTimeout(update, 500);
    }

    update();
    setInterval(update, 2000);
  </script>
</body>
</html>
)rawliteral";

//----------------------------------------Web handlers
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  String json = "{\"light\":" + String((int)lastLightState) +
                ",\"http\":" + String(lastStatusCode) +
                ",\"ago_s\":" + String((millis() - lastReadTime) / 1000) + "}";
  server.send(200, "application/json", json);
}

//----------------------------------------Turn light ON/OFF from the web page (writes to ThingSpeak)
void handleSet() {
  if (!server.hasArg("state")) {
    server.send(400, "text/plain", "missing state");
    return;
  }
  int v = server.arg("state").toInt() ? 1 : 0;

  relayWrite(v == 1);   //--> Switch the relay right away
  Serial.println(v ? "LIGHT set On from web" : "LIGHT set Off from web");

  int code = ThingSpeak.writeField(channel, field1, v, writeAPIKey);
  lastLightState = v;
  lastReadTime = millis();
  if (code == 200) {
    lastStatusCode = 200;
    server.send(200, "text/plain", "ok");
  } else {
    Serial.println("ThingSpeak write failed: " + String(code));
    server.send(500, "text/plain", "Relay switched, but ThingSpeak write failed: " + String(code) + " (check Write API key, wait 15 s between writes)");
  }
}

//----------------------------------------Read ThingSpeak and drive the relay
void readThingSpeak() {
  float state = ThingSpeak.readFloatField(channel, field1, readAPIKey);
  lastStatusCode = ThingSpeak.getLastReadStatus();
  lastReadTime = millis();

  if (lastStatusCode == 200) {
    lastLightState = state;
    if (state == 1) {
      relayWrite(true);
      Serial.println("LIGHT is On");
    } else if (state == 0) {
      relayWrite(false);
      Serial.println("LIGHT is Off");
    }
    Serial.print("The latest data from Field1 on ThingSpeak is : ");
    Serial.println(state);
  } else {
    Serial.println("Problem reading channel. HTTP error code " + String(lastStatusCode));
  }
}

//----------------------------------------SETUP
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(ON_Board_LED, OUTPUT);
  digitalWrite(ON_Board_LED, LOW);
  relayWrite(false);   //--> Make sure the light starts OFF

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("");
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    digitalWrite(ON_Board_LED, HIGH);
    delay(250);
    digitalWrite(ON_Board_LED, LOW);
    delay(250);
  }
  digitalWrite(ON_Board_LED, LOW);

  Serial.println("");
  Serial.print("Successfully connected to : ");
  Serial.println(ssid);
  Serial.print("Open this in your browser: http://");
  Serial.println(WiFi.localIP());

  ThingSpeak.begin(client);

  //----------------------------------------Start web server
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/set", handleSet);
  server.begin();

  readThingSpeak();  //--> First read right away
}

//----------------------------------------LOOP
void loop() {
  server.handleClient();  //--> Serve the web page (no long delay() so the page stays responsive)

  if (millis() - lastReadTime >= READ_INTERVAL) {
    readThingSpeak();
  }
}