#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <Updater.h>

#include <time.h>

// ------------------------------------------------------------
// WiFi & OTA configuration (update these to match your network)
// ------------------------------------------------------------
// const char *ssid = "Ridder Anton";
const char *ssid = "Ridder Anton";
const char *password = "appelsap";
const char *otaHostname = "aquarium-esp01";

// ------------------------------------------------------------
// GPIO configuration - ESP-01 has GPIO0, GPIO2 and GPIO1 (TX) / GPIO3 (RX)
// Update the third entry if you wired RX instead of TX.
// ------------------------------------------------------------
const uint8_t channelPins[3] = {0, 2, 3};  // GPIO0, GPIO2, GPIO1(TX)

// PWM configuration
constexpr uint16_t PWM_MAX = 1023;

enum ChannelColor : uint8_t {
  COLOR_UNKNOWN = 0,
  COLOR_RED,
  COLOR_GREEN,
  COLOR_BLUE
};

struct ChannelState {
  ChannelColor mappedColor;
  uint16_t rawValue;  // 0 - PWM_MAX
};

ChannelState channels[3] = {
  {COLOR_UNKNOWN, 0},
  {COLOR_UNKNOWN, 0},
  {COLOR_UNKNOWN, 0}
};

struct RGBLevel {
  float red;    // 0.0 - 1.0
  float green;  // 0.0 - 1.0
  float blue;   // 0.0 - 1.0
};

RGBLevel manualLevel = {0.0f, 0.0f, 0.0f};
RGBLevel autoLevel = {0.0f, 0.0f, 0.0f};

// Daylight simulation schedule (times in minutes since midnight)
struct DayPhase {
  uint16_t minutes;
  RGBLevel level;
};

const DayPhase daylightSchedule[] = {
  {  0, {0.00f, 0.00f, 0.00f}},  // 00:00 - lights off
  {360, {0.05f, 0.02f, 0.01f}},  // 06:00 - dawn
  {450, {0.35f, 0.25f, 0.20f}},  // 07:30 - sunrise ramp
  {720, {0.80f, 0.80f, 0.85f}},  // 12:00 - midday
  {1020,{0.45f, 0.35f, 0.30f}},  // 17:00 - afternoon
  {1110,{0.25f, 0.10f, 0.05f}},  // 18:30 - sunset
  {1260,{0.00f, 0.00f, 0.00f}}   // 21:00 - lights off
};

constexpr size_t daylightScheduleSize = sizeof(daylightSchedule) / sizeof(daylightSchedule[0]);

// Operating state
bool autoMode = true;
bool timeSynced = false;

String lastUpdateError;

// Test pulse state
int8_t testChannel = -1;
unsigned long testUntil = 0;

ESP8266WebServer server(80);

// NTP configuration (set your timezone offset and DST offset as needed)
const char *ntpServer = "pool.ntp.org";
constexpr long gmtOffsetSec = 0;      // update to your timezone offset (seconds)
constexpr int daylightOffsetSec = 0;  // daylight saving offset (seconds)

// ------------------------------------------------------------
// Helper utilities
// ------------------------------------------------------------
String colorName(ChannelColor color) {
  switch (color) {
    case COLOR_RED: return "Rood";
    case COLOR_GREEN: return "Groen";
    case COLOR_BLUE: return "Blauw";
    default: return "Onbekend";
  }
}

String colorCode(ChannelColor color) {
  switch (color) {
    case COLOR_RED: return "red";
    case COLOR_GREEN: return "green";
    case COLOR_BLUE: return "blue";
    default: return "unknown";
  }
}

ChannelColor colorFromString(const String &value) {
  if (value.equalsIgnoreCase("red")) return COLOR_RED;
  if (value.equalsIgnoreCase("green")) return COLOR_GREEN;
  if (value.equalsIgnoreCase("blue")) return COLOR_BLUE;
  return COLOR_UNKNOWN;
}

String describeUpdateError() {
  uint8_t error = Update.getError();
  switch (error) {
    case UPDATE_ERROR_OK: return "Geen fout";
    case UPDATE_ERROR_WRITE: return "Schrijffout tijdens update";
    case UPDATE_ERROR_ERASE: return "Fout bij wissen van flash";
    case UPDATE_ERROR_READ: return "Leesfout tijdens update";
    case UPDATE_ERROR_SPACE: return "Niet genoeg ruimte voor nieuwe firmware";
    case UPDATE_ERROR_SIZE: return "Bestand is te groot";
    case UPDATE_ERROR_STREAM: return "Probleem met datastream";
    case UPDATE_ERROR_MD5: return "MD5-controle mislukt";
#ifdef UPDATE_ERROR_NO_DATA
    case UPDATE_ERROR_NO_DATA: return "Geen data ontvangen";
#endif
#ifdef UPDATE_ERROR_ACTIVATE
    case UPDATE_ERROR_ACTIVATE: return "Kon nieuwe firmware niet activeren";
#endif
    default: return "Onbekende fout (" + String(error) + ")";
  }
}

float clamp01(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

void applyOutputs(const RGBLevel &rgb) {
  autoLevel = rgb;

  for (int i = 0; i < 3; ++i) {
    float output = 0.0f;
    switch (channels[i].mappedColor) {
      case COLOR_RED:   output = rgb.red; break;
      case COLOR_GREEN: output = rgb.green; break;
      case COLOR_BLUE:  output = rgb.blue; break;
      case COLOR_UNKNOWN:
      default:
        output = 0.0f;
        break;
    }
    channels[i].rawValue = static_cast<uint16_t>(roundf(clamp01(output) * PWM_MAX));
    analogWrite(channelPins[i], channels[i].rawValue);
  }
}

void applyManualOutputs() {
  applyOutputs(manualLevel);
}

time_t nowLocal() {
  time_t raw = time(nullptr);
  return raw;
}

int minutesSinceMidnight() {
  time_t nowVal = nowLocal();
  if (nowVal < 1000) {
    return -1;
  }
  struct tm timeInfo;
  localtime_r(&nowVal, &timeInfo);
  return timeInfo.tm_hour * 60 + timeInfo.tm_min;
}

RGBLevel interpolateLevels(const RGBLevel &a, const RGBLevel &b, float fraction) {
  RGBLevel result;
  result.red = a.red + (b.red - a.red) * fraction;
  result.green = a.green + (b.green - a.green) * fraction;
  result.blue = a.blue + (b.blue - a.blue) * fraction;
  return result;
}

RGBLevel evaluateSchedule(int minutes) {
  if (minutes < 0 || daylightScheduleSize == 0) {
    return {0.0f, 0.0f, 0.0f};
  }

  const DayPhase *prevPhase = &daylightSchedule[0];
  const DayPhase *nextPhase = nullptr;

  for (size_t i = 0; i < daylightScheduleSize; ++i) {
    if (daylightSchedule[i].minutes <= minutes) {
      prevPhase = &daylightSchedule[i];
    } else {
      nextPhase = &daylightSchedule[i];
      break;
    }
  }

  if (nextPhase == nullptr) {
    // Wrap to first element for overnight interpolation
    const DayPhase &first = daylightSchedule[0];
    float duration = (1440 - prevPhase->minutes) + first.minutes;
    float elapsed = minutes - prevPhase->minutes;
    if (elapsed < 0) elapsed += 1440;
    float frac = duration > 0 ? elapsed / duration : 0.0f;
    return interpolateLevels(prevPhase->level, first.level, frac);
  }

  float duration = nextPhase->minutes - prevPhase->minutes;
  float elapsed = minutes - prevPhase->minutes;
  float fraction = (duration > 0) ? (elapsed / duration) : 0.0f;
  return interpolateLevels(prevPhase->level, nextPhase->level, fraction);
}

void updateAutoMode() {
  if (!autoMode) {
    return;
  }

  int minutes = minutesSinceMidnight();
  if (minutes >= 0) {
    RGBLevel level = evaluateSchedule(minutes);
    applyOutputs(level);
  }
}

String channelSummaryJson() {
  String json = "[";
  for (int i = 0; i < 3; ++i) {
    if (i > 0) json += ",";
    json += "{\"pin\":" + String(channelPins[i]) +
            ",\"color\":\"" + colorName(channels[i].mappedColor) +
            "\",\"code\":\"" + colorCode(channels[i].mappedColor) +
            "\",\"raw\":" + String(channels[i].rawValue) + "}";
  }
  json += "]";
  return json;
}

void stopTestIfExpired() {
  if (testChannel >= 0 && millis() > testUntil) {
    testChannel = -1;
    if (autoMode) {
      updateAutoMode();
    } else {
      applyManualOutputs();
    }
  }
}

void triggerTestPulse(int channelIndex) {
  if (channelIndex < 0 || channelIndex >= 3) return;

  testChannel = channelIndex;
  testUntil = millis() + 4000;  // 4 seconds

  for (int i = 0; i < 3; ++i) {
    channels[i].rawValue = (i == channelIndex) ? PWM_MAX : 0;
    analogWrite(channelPins[i], channels[i].rawValue);
  }
}

// ------------------------------------------------------------
// HTTP Handlers
// ------------------------------------------------------------
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="nl">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Aquarium RGB Controller</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 16px; background: #f4f6f8; color:#333; }
    h1 { margin-bottom: 0.4em; }
    .card { background: #fff; border-radius: 8px; padding: 16px; margin-bottom: 16px; box-shadow: 0 2px 6px rgba(0,0,0,0.1); }
    table { width: 100%; border-collapse: collapse; margin-top: 8px; }
    th, td { padding: 8px; border-bottom: 1px solid #ddd; text-align: left; }
    th { background: #f0f2f5; }
    button { padding: 6px 12px; border: none; border-radius: 4px; cursor: pointer; background: #1976d2; color: #fff; }
    button.secondary { background: #455a64; }
    button.danger { background: #c62828; }
    .switch { display: flex; align-items: center; gap: 12px; }
    .sliders { display: flex; flex-direction: column; gap: 12px; }
    .slider-row { display: flex; align-items: center; gap: 12px; }
    input[type=range] { flex: 1; }
    .badge { display:inline-block; padding:3px 6px; border-radius:4px; background:#1976d2; color:#fff; font-size:0.75em; }
    .error { color:#c62828; font-weight:bold; }
    .command-log { padding:10px; margin-bottom:15px; background:#fff; border-radius:4px; border:1px solid #ddd; }
    .command-log h3 { margin:0 0 8px; color:#333; font-size:1em; }
    .command-entry { padding:6px 0; border-bottom:1px solid #eee; font-size:0.85em; display:flex; flex-direction:column; gap:2px; }
    .command-entry:last-child { border-bottom:none; }
    .command-func { font-weight:bold; color:#333; }
    .command-dir { font-weight:bold; }
    .command-dir.req { color:#007bff; }
    .command-dir.res { color:#28a745; }
    .command-meta { color:#666; font-size:0.75em; }
    .command-hex { font-family:monospace; font-size:0.75em; color:#555; }
    .command-empty { font-size:0.85em; color:#666; }
  </style>
</head>
<body>
  <h1>Aquarium RGB Controller</h1>
  <div class="card">
    <h2>Status</h2>
    <p>WiFi: <span id="wifiStatus">-</span></p>
    <p>Tijd: <span id="timeStatus">-</span></p>
    <p>Modus: <span id="modeStatus">-</span></p>
  </div>

  <div class="card">
    <h2>Kanalen testen & koppelen</h2>
    <table>
      <thead>
        <tr><th>Kanaal</th><th>Pin</th><th>Huidige kleur</th><th>Actie</th><th>Koppelen</th></tr>
      </thead>
      <tbody id="channelTable"></tbody>
    </table>
  </div>

  <div class="card">
    <div class="switch">
      <strong>Automatische daglicht simulatie</strong>
      <button id="toggleModeBtn" class="secondary">Toggle</button>
    </div>
  </div>

  <div class="card" id="manualCard">
    <h2>Handmatige RGB regeling</h2>
    <div class="sliders">
      <div class="slider-row"><label>Rood</label><input type="range" min="0" max="100" id="sliderR"><span id="valR">0%</span></div>
      <div class="slider-row"><label>Groen</label><input type="range" min="0" max="100" id="sliderG"><span id="valG">0%</span></div>
      <div class="slider-row"><label>Blauw</label><input type="range" min="0" max="100" id="sliderB"><span id="valB">0%</span></div>
    </div>
    <button id="applyManualBtn">Pas waarden toe</button>
  </div>

  <div class="command-log">
    <h3>Laatste Modbus Frames (60s)</h3>
    <div id="commandLog"><div class="command-empty">Geen recente frames</div></div>
  </div>

  <div class="card">
    <small>Tip: gebruik de test-knoppen om te zien welk kanaal welke kleur LED aanstuurt. Koppel daarna de juiste kleur via het keuzemenu.</small>
  </div>

  <div class="card">
    <h2>Firmware update</h2>
    <p>Gebruik deze pagina als OTA niet werkt.</p>
    <button onclick="window.location.href='/update'">Open updatepagina</button>
  </div>

  <script>
    let autoMode = true;

    ['sliderR','sliderG','sliderB'].forEach((id, idx) => {
      const el = document.getElementById(id);
      const label = ['valR','valG','valB'][idx];
      el.addEventListener('input', () => {
        document.getElementById(label).innerText = el.value + '%';
      });
    });

    async function fetchState() {
      try {
        const response = await fetch('/state');
        if (!response.ok) throw new Error('State fetch failed');
        const data = await response.json();
        updateUi(data);
      } catch (err) {
        document.getElementById('wifiStatus').innerText = 'Offline';
        console.error(err);
      }
    }

    function updateUi(data) {
      document.getElementById('wifiStatus').innerText = data.wifi || '-';
      document.getElementById('timeStatus').innerText = data.time || '-';
      autoMode = data.autoMode;
      document.getElementById('modeStatus').innerText = autoMode ? 'Automatisch' : 'Handmatig';
      document.getElementById('toggleModeBtn').innerText = autoMode ? 'Zet handmatig' : 'Zet automatisch';
      document.getElementById('manualCard').style.display = autoMode ? 'none' : 'block';

      // Update sliders
      document.getElementById('sliderR').value = Math.round(data.manual.red * 100);
      document.getElementById('sliderG').value = Math.round(data.manual.green * 100);
      document.getElementById('sliderB').value = Math.round(data.manual.blue * 100);
      document.getElementById('valR').innerText = Math.round(data.manual.red * 100) + '%';
      document.getElementById('valG').innerText = Math.round(data.manual.green * 100) + '%';
      document.getElementById('valB').innerText = Math.round(data.manual.blue * 100) + '%';

      renderChannels(data.channels);
    }

    function renderChannels(channels) {
      const tbody = document.getElementById('channelTable');
      tbody.innerHTML = '';
      channels.forEach((ch, index) => {
        const tr = document.createElement('tr');
        tr.innerHTML = `
          <td>Kanaal ${index+1}</td>
          <td>GPIO ${ch.pin}</td>
          <td><span class="badge">${ch.color}</span></td>
          <td><button onclick="testChannel(${index})">Test kanaal</button></td>
          <td>
            <select onchange="assignColor(${index}, this.value)">
              <option value="unknown">Onbekend</option>
              <option value="red">Rood</option>
              <option value="green">Groen</option>
              <option value="blue">Blauw</option>
            </select>
          </td>`;
        const select = tr.querySelector('select');
        if (select) {
          select.value = ch.code || 'unknown';
        }
        tbody.appendChild(tr);
      });
    }

    async function testChannel(index) {
      await fetch(`/test?channel=${index}`, { method:'POST' });
    }

    async function assignColor(index, color) {
      await fetch(`/assign?channel=${index}&color=${color}`, { method:'POST' });
      fetchState();
    }

    document.getElementById('toggleModeBtn').addEventListener('click', async () => {
      await fetch(`/mode?auto=${autoMode ? 0 : 1}`, { method:'POST' });
      fetchState();
    });

    document.getElementById('applyManualBtn').addEventListener('click', async () => {
      const r = document.getElementById('sliderR').value;
      const g = document.getElementById('sliderG').value;
      const b = document.getElementById('sliderB').value;
      await fetch(`/manual?r=${r}&g=${g}&b=${b}`, { method:'POST' });
      fetchState();
    });

    setInterval(fetchState, 5000);
    fetchState();
  </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleState() {
  stopTestIfExpired();

  String json = "{";
  json += "\"wifi\":\"" + WiFi.SSID() + (WiFi.isConnected() ? " (" + WiFi.localIP().toString() + ")" : "") + "\",";

  time_t nowVal = nowLocal();
  if (nowVal > 0) {
    struct tm timeInfo;
    localtime_r(&nowVal, &timeInfo);
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
    json += "\"time\":\"" + String(buffer) + "\",";
  } else {
    json += "\"time\":\"-\",";
  }

  json += "\"autoMode\":" + String(autoMode ? "true" : "false") + ",";
  json += "\"manual\":{\"red\":" + String(manualLevel.red, 3) +
          ",\"green\":" + String(manualLevel.green, 3) +
          ",\"blue\":" + String(manualLevel.blue, 3) + "},";
  json += "\"channels\":" + channelSummaryJson();
  json += "}";

  server.send(200, "application/json", json);
}

void handleAssign() {
  if (!server.hasArg("channel") || !server.hasArg("color")) {
    server.send(400, "text/plain", "Missing parameters");
    return;
  }

  int ch = server.arg("channel").toInt();
  String colorStr = server.arg("color");

  if (ch < 0 || ch >= 3) {
    server.send(400, "text/plain", "Invalid channel");
    return;
  }

  ChannelColor color = colorFromString(colorStr);
  channels[ch].mappedColor = color;

  if (autoMode) {
    updateAutoMode();
  } else {
    applyManualOutputs();
  }

  server.send(200, "text/plain", "OK");
}

void handleManual() {
  if (!server.hasArg("r") || !server.hasArg("g") || !server.hasArg("b")) {
    server.send(400, "text/plain", "Missing parameters");
    return;
  }

  manualLevel.red = clamp01(server.arg("r").toInt() / 100.0f);
  manualLevel.green = clamp01(server.arg("g").toInt() / 100.0f);
  manualLevel.blue = clamp01(server.arg("b").toInt() / 100.0f);

  if (!autoMode) {
    applyManualOutputs();
  }

  server.send(200, "text/plain", "OK");
}

void handleMode() {
  if (!server.hasArg("auto")) {
    server.send(400, "text/plain", "Missing parameters");
    return;
  }

  autoMode = server.arg("auto").toInt() != 0;
  if (autoMode) {
    updateAutoMode();
  } else {
    applyManualOutputs();
  }

  server.send(200, "text/plain", "OK");
}

void handleTest() {
  if (!server.hasArg("channel")) {
    server.send(400, "text/plain", "Missing channel");
    return;
  }

  int ch = server.arg("channel").toInt();
  if (ch < 0 || ch >= 3) {
    server.send(400, "text/plain", "Invalid channel");
    return;
  }

  triggerTestPulse(ch);
  server.send(200, "text/plain", "OK");
}

void handleUpdatePage() {
  String status = lastUpdateError.length() ? ("<p class=\"error\">Laatste fout: " + lastUpdateError + "</p>") : "";
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="nl">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Firmware update</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 32px; background:#f4f6f8; color:#333; }
    .container { max-width: 420px; margin: 0 auto; background:#fff; padding:24px; border-radius:8px; box-shadow:0 2px 6px rgba(0,0,0,0.1); }
    h1 { margin-top:0; }
    form { display:flex; flex-direction:column; gap:16px; }
    input[type=file] { padding:8px 0; }
    button { padding:8px 16px; border:none; border-radius:4px; background:#1976d2; color:#fff; cursor:pointer; }
    .back { margin-top:16px; display:inline-block; }
    .error { margin-top:12px; color:#c62828; font-weight:bold; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Firmware update</h1>
    <p>Selecteer een .bin bestand dat door PlatformIO/Arduino is gecompileerd.</p>
    %STATUS%
    <form method="POST" action="/update" enctype="multipart/form-data">
      <input type="file" name="update" accept=".bin" required />
      <button type="submit">Update starten</button>
    </form>
    <a class="back" href="/">&larr; Terug naar overzicht</a>
  </div>
</body>
</html>
)rawliteral";
  html.replace("%STATUS%", status);
  server.send(200, "text/html", html);
}

void handleUpdatePost() {
  if (Update.hasError() || lastUpdateError.length()) {
    String message = lastUpdateError.length() ? lastUpdateError : ("Update mislukt: " + describeUpdateError());
    server.send(500, "text/plain", "Update mislukt. " + message);
    lastUpdateError = "";
    return;
  }

  lastUpdateError = "";
  server.send(200, "text/plain", "Update geslaagd. Het apparaat herstart nu.");
  delay(200);
  ESP.restart();
}

void handleUpdateUpload() {
  HTTPUpload &upload = server.upload();
  switch (upload.status) {
    case UPLOAD_FILE_START: {
      lastUpdateError = "";
      WiFiUDP::stopAll();
      size_t freeSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(freeSpace)) {
        lastUpdateError = "Kan update niet starten: " + describeUpdateError();
        Update.end();
      }
      break;
    }
    case UPLOAD_FILE_WRITE: {
      if (!Update.hasError()) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          lastUpdateError = "Schrijven mislukt: " + describeUpdateError();
          Update.end();
        }
      }
      break;
    }
    case UPLOAD_FILE_END: {
      if (!Update.hasError()) {
        if (!Update.end(true)) {
          lastUpdateError = "Afronden mislukt: " + describeUpdateError();
        }
      } else {
        Update.end();
      }
      break;
    }
    case UPLOAD_FILE_ABORTED: {
      Update.end();
      lastUpdateError = "Upload afgebroken";
      break;
    }
    default:
      break;
  }

  yield();
}

// ------------------------------------------------------------
// Setup & Loop
// ------------------------------------------------------------
void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 20000) {
    delay(500);
  }
}

void setupPwm() {
  analogWriteRange(PWM_MAX);
  analogWriteFreq(1000);
  for (uint8_t pin : channelPins) {
    pinMode(pin, OUTPUT);
    analogWrite(pin, 0);
  }
}

void setupOta() {
  ArduinoOTA.setHostname(otaHostname);
  ArduinoOTA.onStart([]() {});
  ArduinoOTA.onEnd([]() {});
  ArduinoOTA.onError([](ota_error_t error) {
    (void)error;
  });
  ArduinoOTA.begin();
}

void setupServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/state", HTTP_GET, handleState);
  server.on("/assign", HTTP_POST, handleAssign);
  server.on("/manual", HTTP_POST, handleManual);
  server.on("/mode", HTTP_POST, handleMode);
  server.on("/test", HTTP_POST, handleTest);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void setupTime() {
  configTime(gmtOffsetSec, daylightOffsetSec, ntpServer);
  unsigned long start = millis();
  while (!time(nullptr) && millis() - start < 15000) {
    delay(200);
  }
  timeSynced = time(nullptr) > 1000;
}

void setup() {
  setupPwm();
  connectWifi();

  if (WiFi.isConnected()) {
    if (MDNS.begin(otaHostname)) {
      MDNS.addService("http", "tcp", 80);
    }
    setupOta();
    setupServer();
    setupTime();
  }

  updateAutoMode();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  stopTestIfExpired();

  static unsigned long lastAutoUpdate = 0;
  unsigned long nowMillis = millis();
  if (autoMode && nowMillis - lastAutoUpdate > 30000) {
    lastAutoUpdate = nowMillis;
    updateAutoMode();
  }
}