#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>

#ifndef FW_VERSION
#define FW_VERSION "v0.3.1"
#endif

#if __has_include("build_info.h")
#include "build_info.h"
#else
#define FW_GIT_HASH "local"
#endif

// ---------- Waveshare ESP32-C6-LCD-1.47-M ----------
static constexpr int LCD_MOSI = 6;
static constexpr int LCD_SCLK = 7;
static constexpr int LCD_CS   = 14;
static constexpr int LCD_DC   = 15;
static constexpr int LCD_RST  = 21;
static constexpr int LCD_BL   = 22;
static constexpr int BOOT_BTN = 9;

static constexpr int LCD_W = 172;
static constexpr int LCD_H = 320;
static constexpr int LCD_X_OFFSET = 34;
static constexpr int LCD_Y_OFFSET = 0;

Arduino_DataBus *bus = new Arduino_ESP32SPI(
  LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, GFX_NOT_DEFINED
);

Arduino_GFX *gfx = new Arduino_ST7789(
  bus, LCD_RST, 0, true,
  LCD_W, LCD_H,
  LCD_X_OFFSET, LCD_Y_OFFSET,
  LCD_X_OFFSET, LCD_Y_OFFSET
);

// ---------- Modes ----------
enum RunMode {
  MODE_TERMINAL,
  MODE_SETUP,
  MODE_OTA
};

RunMode runMode = MODE_TERMINAL;

WebServer localServer(80);
bool localServerStarted = false;

Preferences prefs;

String r1Ssid;
String r1Password;

const IPAddress R1_IP(192, 168, 4, 1);

static const char *SETUP_SSID = "R1TERM-SETUP";
static const char *SETUP_PASS = "r1terminal";

static const char *OTA_SSID = "C6-OTA";
static const char *OTA_PASS = "C6update47";

// ---------- R1 state ----------
struct R1State {
  bool online = false;
  bool ready = false;
  bool valid = false;

  float volts = NAN;
  float amps = NAN;
  float watts = NAN;
  float tempC = NAN;

  uint16_t requestedHz = 0;
  float measuredHz = 0;

  char firmware[20] = "?";
  char recState[16] = "?";
  char sd[16] = "?";
  char ina[16] = "?";
  char rtc[16] = "?";
  char eeprom[16] = "?";

  uint64_t recRows = 0;
  uint64_t recBytes = 0;
  float recSeconds = 0;
  double recWh = 0;
  double recAh = 0;

  uint32_t recQueued = 0;
  uint32_t recHighWater = 0;
  uint32_t recOverflows = 0;

  uint32_t missed = 0;
  uint32_t invalid = 0;
  uint32_t previewDrops = 0;

  bool bleAuth = false;
  bool bleFresh = false;
  bool bleRtc = false;
  char bleState[20] = "OFF";

  uint32_t uptimeMs = 0;
  uint32_t lastOkMs = 0;
};

R1State r1;

uint64_t liveCursor = 0;
uint32_t lastStatePoll = 0;
uint32_t lastLivePoll = 0;
uint32_t lastDraw = 0;
uint32_t lastReconnect = 0;

uint8_t page = 0;

bool buttonDown = false;
uint32_t buttonDownAt = 0;

bool otaUploadOk = false;

int lastHttpCode = 0;
int lastContentLength = -1;
char lastNetError[48] = "not tried";


// ----------------------------------------------------

void setBacklight(uint8_t percent)
{
  percent = constrain(percent, 0, 50);

  static bool attached = false;

  if (!attached) {
    ledcAttach(LCD_BL, 5000, 8);
    attached = true;
  }

  ledcWrite(
    LCD_BL,
    map(percent, 0, 100, 0, 255)
  );
}

void clearScreen()
{
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextWrap(false);
}

void footer(const char *text)
{
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setCursor(7, 307);
  gfx->print(text);
}

void header(const char *title)
{
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(7, 7);
  gfx->print(title);

  gfx->setTextColor(
    r1.online ? RGB565_GREEN : RGB565_RED
  );

  gfx->setCursor(145, 7);
  gfx->print(r1.online ? "ON" : "OFF");

  gfx->drawFastHLine(
    6, 22, LCD_W - 12,
    RGB565_DARKGREY
  );
}

void drawMain()
{
  clearScreen();
  header("R1 TERMINAL");

  if (!r1.online) {
    gfx->setTextColor(RGB565_RED);
    gfx->setTextSize(2);
    gfx->setCursor(15, 55);
    gfx->println("NO R1 DATA");

    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(1);
    gfx->setCursor(10, 95);

    if (WiFi.status() == WL_CONNECTED) {
      gfx->println("WiFi connected");

      gfx->printf(
        "IP %s\n",
        WiFi.localIP().toString().c_str()
      );

      gfx->printf(
        "GW %s\n",
        WiFi.gatewayIP().toString().c_str()
      );

      gfx->printf(
        "HTTP %d  LEN %d\n",
        lastHttpCode,
        lastContentLength
      );

      gfx->println(lastNetError);
    } else {
      gfx->println("Connecting to:");
      gfx->println(r1Ssid);
    }

    footer("BOOT: page / 2s OTA / 6s CFG");
    return;
  }

  char b[40];

  gfx->setTextSize(3);
  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(8, 38);

  if (isfinite(r1.amps)) {
    snprintf(b, sizeof(b), "%+.2fA", r1.amps);
    gfx->println(b);
  } else {
    gfx->println("--.--A");
  }

  gfx->setTextColor(RGB565_YELLOW);
  gfx->setCursor(8, 78);

  if (isfinite(r1.volts)) {
    snprintf(b, sizeof(b), "%.2fV", r1.volts);
    gfx->println(b);
  } else {
    gfx->println("--.--V");
  }

  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(9, 120);

  if (isfinite(r1.watts)) {
    snprintf(b, sizeof(b), "%+.1f W", r1.watts);
    gfx->println(b);
  } else {
    gfx->println("--- W");
  }

  bool recording =
    !strcmp(r1.recState, "RUNNING");

  gfx->setTextColor(
    recording ? RGB565_RED : RGB565_GREEN
  );

  gfx->setCursor(9, 155);
  gfx->printf(
    "REC %s",
    recording ? "ON" : "OFF"
  );

  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_WHITE);

  gfx->setCursor(9, 190);
  gfx->printf(
    "Rate %u / %.1f Hz",
    r1.requestedHz,
    r1.measuredHz
  );

  gfx->setCursor(9, 210);
  gfx->printf(
    "SD: %-8s",
    r1.sd
  );

  gfx->setCursor(9, 230);
  gfx->printf(
    "R3: %s%s",
    r1.bleAuth ? "AUTH " : "-- ",
    r1.bleFresh ? "LIVE" : ""
  );

  gfx->setCursor(9, 250);
  gfx->printf(
    "WiFi %d dBm",
    WiFi.RSSI()
  );

  gfx->setTextColor(RGB565_LIGHTGREY);
  gfx->setCursor(9, 274);
  gfx->printf(
    "R1 %s",
    r1.firmware
  );

  gfx->setCursor(9, 288);
  gfx->printf(
    "TERM %s %s",
    FW_VERSION,
    FW_GIT_HASH
  );

  footer("1/3  BOOT = next");
}

void drawRecording()
{
  clearScreen();
  header("R1 RECORDING");

  bool recording =
    !strcmp(r1.recState, "RUNNING");

  gfx->setTextSize(2);
  gfx->setTextColor(
    recording ? RGB565_RED : RGB565_GREEN
  );

  gfx->setCursor(8, 36);
  gfx->println(r1.recState);

  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_WHITE);

  uint64_t mb = r1.recBytes / 1048576ULL;

  gfx->setCursor(8, 75);
  gfx->printf(
    "Time: %.1f s",
    r1.recSeconds
  );

  gfx->setCursor(8, 96);

  char rows[30];
  snprintf(
    rows,
    sizeof(rows),
    "%llu",
    (unsigned long long)r1.recRows
  );

  gfx->printf("Rows: %s", rows);

  gfx->setCursor(8, 117);
  gfx->printf(
    "Size: %llu MB",
    (unsigned long long)mb
  );

  gfx->setTextColor(RGB565_YELLOW);

  gfx->setCursor(8, 145);
  gfx->printf(
    "Energy: %.5f Wh",
    r1.recWh
  );

  gfx->setCursor(8, 166);
  gfx->printf(
    "Charge: %.5f Ah",
    r1.recAh
  );

  gfx->setTextColor(RGB565_WHITE);

  gfx->setCursor(8, 198);
  gfx->printf(
    "FIFO: %lu / %lu",
    (unsigned long)r1.recQueued,
    (unsigned long)r1.recHighWater
  );

  gfx->setCursor(8, 219);
  gfx->printf(
    "Overflow: %lu",
    (unsigned long)r1.recOverflows
  );

  gfx->setCursor(8, 240);
  gfx->printf(
    "Missed: %lu",
    (unsigned long)r1.missed
  );

  gfx->setCursor(8, 261);
  gfx->printf(
    "Invalid: %lu",
    (unsigned long)r1.invalid
  );

  footer("2/3  BOOT = next");
}

void drawHealth()
{
  clearScreen();
  header("R1 STATUS");

  gfx->setTextSize(1);

  auto line = [](
    int y,
    const char *name,
    const char *value,
    bool good
  ) {
    gfx->setTextColor(RGB565_LIGHTGREY);
    gfx->setCursor(8, y);
    gfx->printf("%-7s", name);

    gfx->setTextColor(
      good ? RGB565_GREEN : RGB565_YELLOW
    );

    gfx->print(value);
  };

  line(
    38, "INA",
    r1.ina,
    strstr(r1.ina, "PASS") ||
    strstr(r1.ina, "READ") ||
    strstr(r1.ina, "OK")
  );

  line(
    60, "SD",
    r1.sd,
    !strcmp(r1.sd, "READ")
  );

  line(
    82, "RTC",
    r1.rtc,
    strstr(r1.rtc, "PASS") ||
    strstr(r1.rtc, "OK")
  );

  line(
    104, "EEPROM",
    r1.eeprom,
    !strcmp(r1.eeprom, "READ")
  );

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(8, 132);
  gfx->printf(
    "INA temp: %.1f C",
    r1.tempC
  );

  gfx->setCursor(8, 154);
  gfx->printf(
    "WiFi: %d dBm",
    WiFi.RSSI()
  );

  gfx->setCursor(8, 176);
  gfx->printf(
    "R3 BLE: %s",
    r1.bleState
  );

  gfx->setCursor(8, 198);
  gfx->printf(
    "Auth/Fresh: %s/%s",
    r1.bleAuth ? "Y" : "N",
    r1.bleFresh ? "Y" : "N"
  );

  gfx->setCursor(8, 220);
  gfx->printf(
    "R3 RTC: %s",
    r1.bleRtc ? "SYNC" : "--"
  );

  gfx->setCursor(8, 242);
  gfx->printf(
    "Preview drops: %lu",
    (unsigned long)r1.previewDrops
  );

  gfx->setCursor(8, 264);
  gfx->printf(
    "R1 up: %lu s",
    (unsigned long)(r1.uptimeMs / 1000)
  );

  footer("3/3  BOOT = next");
}

void drawTerminal()
{
  if (page == 0) {
    drawMain();
  } else if (page == 1) {
    drawRecording();
  } else {
    drawHealth();
  }
}

// ---------- Config portal ----------

void drawSetup()
{
  clearScreen();

  gfx->setTextColor(RGB565_YELLOW);
  gfx->setTextSize(2);
  gfx->setCursor(10, 18);
  gfx->println("R1 SETUP");

  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_WHITE);

  gfx->setCursor(10, 65);
  gfx->println("WiFi:");
  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(10, 83);
  gfx->println(SETUP_SSID);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(10, 115);
  gfx->println("Password:");
  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(10, 133);
  gfx->println(SETUP_PASS);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(10, 170);
  gfx->println("Open:");

  gfx->setTextColor(RGB565_GREEN);
  gfx->setCursor(10, 188);
  gfx->println("192.168.4.1");

  gfx->setTextColor(RGB565_LIGHTGREY);
  gfx->setCursor(10, 225);
  gfx->println("Enter R1 SSID");
  gfx->setCursor(10, 241);
  gfx->println("and R1 password.");
}

void drawOTA()
{
  clearScreen();

  gfx->setTextColor(RGB565_YELLOW);
  gfx->setTextSize(2);
  gfx->setCursor(10, 18);
  gfx->println("OTA MODE");

  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_WHITE);

  gfx->setCursor(10, 65);
  gfx->println("WiFi:");
  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(10, 83);
  gfx->println(OTA_SSID);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(10, 115);
  gfx->println("Password:");

  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(10, 133);
  gfx->println(OTA_PASS);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(10, 170);
  gfx->println("Open:");

  gfx->setTextColor(RGB565_GREEN);
  gfx->setCursor(10, 188);
  gfx->println("192.168.4.1");
}

void startLocalServer()
{
  if (localServerStarted) {
    localServer.stop();
  }

  localServer.begin();
  localServerStarted = true;
}

void startSetupPortal()
{
  runMode = MODE_SETUP;

  WiFi.disconnect(true);
  delay(150);
  WiFi.mode(WIFI_AP);

  WiFi.softAP(
    SETUP_SSID,
    SETUP_PASS
  );

  startLocalServer();
  drawSetup();
}

void startOTA()
{
  runMode = MODE_OTA;

  WiFi.disconnect(true);
  delay(150);
  WiFi.mode(WIFI_AP);

  WiFi.softAP(
    OTA_SSID,
    OTA_PASS
  );

  startLocalServer();
  drawOTA();
}

// ---------- HTTP server ----------

void configureLocalServer()
{
  localServer.on(
    "/",
    HTTP_GET,
    []()
    {
      if (runMode == MODE_SETUP) {
        static const char page[] = R"HTML(
<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>R1 Terminal Setup</title>
</head>
<body style="font-family:sans-serif;max-width:500px;margin:30px auto;padding:20px">
<h2>R1 Terminal Setup</h2>
<p>Enter the Wi-Fi credentials shown by the R1-S3 panel.</p>
<form method="POST" action="/save">
<label>R1 SSID</label><br>
<input name="ssid" placeholder="R1-S3-XXXX" required style="width:100%;padding:8px"><br><br>
<label>R1 password</label><br>
<input name="pass" type="password" required style="width:100%;padding:8px"><br><br>
<button type="submit">Save and connect</button>
</form>
</body>
</html>
)HTML";

        localServer.send(
          200,
          "text/html",
          page
        );

      } else if (runMode == MODE_OTA) {

        static const char page[] = R"HTML(
<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>R1 Terminal OTA</title>
</head>
<body style="font-family:sans-serif;max-width:500px;margin:30px auto;padding:20px">
<h2>R1 Terminal OTA</h2>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="firmware" accept=".bin">
<br><br>
<button type="submit">Upload firmware</button>
</form>
</body>
</html>
)HTML";

        localServer.send(
          200,
          "text/html",
          page
        );

      } else {
        localServer.send(
          200,
          "text/plain",
          "R1 Terminal"
        );
      }
    }
  );

  localServer.on(
    "/save",
    HTTP_POST,
    []()
    {
      if (runMode != MODE_SETUP) {
        localServer.send(403, "text/plain", "Not in setup mode");
        return;
      }

      String ssid =
        localServer.arg("ssid");

      String pass =
        localServer.arg("pass");

      ssid.trim();
      pass.trim();

      if (
        ssid.length() == 0 ||
        pass.length() < 8
      ) {
        localServer.send(
          400,
          "text/plain",
          "Invalid SSID/password"
        );
        return;
      }

      Preferences p;

      if (!p.begin("r1term", false)) {
        localServer.send(
          500,
          "text/plain",
          "NVS error"
        );
        return;
      }

      p.putString("ssid", ssid);
      p.putString("pass", pass);
      p.end();

      localServer.send(
        200,
        "text/plain",
        "Saved. Rebooting..."
      );

      delay(1000);
      ESP.restart();
    }
  );

  localServer.on(
    "/update",
    HTTP_POST,

    []()
    {
      otaUploadOk =
        !Update.hasError();

      localServer.send(
        otaUploadOk ? 200 : 500,
        "text/plain",
        otaUploadOk ?
          "UPDATE OK - REBOOTING" :
          "UPDATE FAILED"
      );

      delay(1200);

      if (otaUploadOk) {
        ESP.restart();
      }
    },

    []()
    {
      if (runMode != MODE_OTA) {
        return;
      }

      HTTPUpload &upload =
        localServer.upload();

      if (
        upload.status ==
        UPLOAD_FILE_START
      ) {
        otaUploadOk = false;

        if (
          !Update.begin(
            UPDATE_SIZE_UNKNOWN
          )
        ) {
          Update.printError(Serial);
        }

      } else if (
        upload.status ==
        UPLOAD_FILE_WRITE
      ) {

        if (
          Update.write(
            upload.buf,
            upload.currentSize
          ) != upload.currentSize
        ) {
          Update.printError(Serial);
        }

      } else if (
        upload.status ==
        UPLOAD_FILE_END
      ) {

        otaUploadOk =
          Update.end(true);

        if (!otaUploadOk) {
          Update.printError(Serial);
        }
      }
    }
  );
}

// ---------- R1 Wi-Fi ----------

bool loadR1Config()
{
  if (!prefs.begin("r1term", true)) {
    return false;
  }

  r1Ssid =
    prefs.getString("ssid", "");

  r1Password =
    prefs.getString("pass", "");

  prefs.end();

  return
    r1Ssid.length() &&
    r1Password.length() >= 8;
}

void clearR1Config()
{
  Preferences p;

  if (p.begin("r1term", false)) {
    p.clear();
    p.end();
  }
}

bool connectR1()
{
  clearScreen();

  gfx->setTextColor(RGB565_CYAN);
  gfx->setTextSize(2);
  gfx->setCursor(10, 20);
  gfx->println("R1 TERMINAL");

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(10, 65);
  gfx->println("Connecting:");

  gfx->setTextColor(RGB565_YELLOW);
  gfx->setCursor(10, 83);
  gfx->println(r1Ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  WiFi.begin(
    r1Ssid.c_str(),
    r1Password.c_str()
  );

  uint32_t began = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - began < 15000
  ) {
    delay(100);
  }

  return
    WiFi.status() == WL_CONNECTED;
}

// ---------- R1 HTTP API ----------

bool getJson(
  const String &path,
  JsonDocument &doc
)
{
  if (WiFi.status() != WL_CONNECTED) {
    lastHttpCode = -10;
    lastContentLength = -1;
    strlcpy(
      lastNetError,
      "WiFi disconnected",
      sizeof(lastNetError)
    );
    return false;
  }

  WiFiClient client;
  client.setTimeout(1500);

  if (!client.connect(R1_IP, 80)) {
    lastHttpCode = -11;
    lastContentLength = -1;
    strlcpy(
      lastNetError,
      "TCP connect failed",
      sizeof(lastNetError)
    );
    return false;
  }

  client.print("GET ");
  client.print(path);
  client.print(
    " HTTP/1.1\r\n"
    "Host: 192.168.4.1\r\n"
    "Accept: application/json\r\n"
    "Connection: close\r\n"
    "\r\n"
  );

  String status =
    client.readStringUntil('
');

  status.trim();

  int sp =
    status.indexOf(' ');

  lastHttpCode =
    sp >= 0 ?
    status.substring(sp + 1).toInt() :
    -12;

  lastContentLength = -1;

  while (client.connected()) {
    String line =
      client.readStringUntil('
');

    line.trim();

    if (line.length() == 0) {
      break;
    }

    if (
      line.startsWith(
        "Content-Length:"
      )
    ) {
      lastContentLength =
        line.substring(15).toInt();
    }
  }

  if (lastHttpCode != 200) {
    snprintf(
      lastNetError,
      sizeof(lastNetError),
      "HTTP %d",
      lastHttpCode
    );

    client.stop();
    return false;
  }

  DeserializationError error =
    deserializeJson(doc, client);

  client.stop();

  if (error) {
    snprintf(
      lastNetError,
      sizeof(lastNetError),
      "JSON: %.36s",
      error.c_str()
    );

    return false;
  }

  strlcpy(
    lastNetError,
    "JSON OK",
    sizeof(lastNetError)
  );

  return true;
}

bool pollState()
{
  JsonDocument doc;

  if (!getJson("/api/state", doc)) {
    return false;
  }

  if (!(doc["ready"] | false)) {
    strlcpy(
      lastNetError,
      "state ready=false",
      sizeof(lastNetError)
    );
    return false;
  }

  r1.ready = true;
  r1.valid =
    doc["valid"] | false;

  r1.volts =
    doc["volts"] | NAN;

  r1.amps =
    doc["amps"] | NAN;

  r1.watts =
    doc["watts"] | NAN;

  r1.tempC =
    doc["temp_c"] | NAN;

  r1.requestedHz =
    doc["requested_hz"] | 0;

  r1.measuredHz =
    doc["measured_hz"] | 0.0f;

  strlcpy(
    r1.firmware,
    doc["firmware"] | "?",
    sizeof(r1.firmware)
  );

  strlcpy(
    r1.recState,
    doc["recording_state"] | "?",
    sizeof(r1.recState)
  );

  strlcpy(
    r1.sd,
    doc["sd"] | "?",
    sizeof(r1.sd)
  );

  strlcpy(
    r1.ina,
    doc["ina"] | "?",
    sizeof(r1.ina)
  );

  strlcpy(
    r1.rtc,
    doc["rtc"] | "?",
    sizeof(r1.rtc)
  );

  strlcpy(
    r1.eeprom,
    doc["eeprom"] | "?",
    sizeof(r1.eeprom)
  );

  r1.recRows =
    doc["recording_rows"] | 0ULL;

  r1.recBytes =
    doc["recording_bytes"] | 0ULL;

  r1.recSeconds =
    doc["recording_seconds"] | 0.0f;

  r1.recWh =
    doc["recording_wh"] | 0.0;

  r1.recAh =
    doc["recording_ah"] | 0.0;

  r1.recQueued =
    doc["recording_queued"] | 0;

  r1.recHighWater =
    doc["recording_high_water"] | 0;

  r1.recOverflows =
    doc["recording_overflows"] | 0;

  r1.missed =
    doc["missed_samples"] | 0;

  r1.invalid =
    doc["invalid_samples"] | 0;

  r1.previewDrops =
    doc["preview_drops"] | 0;

  r1.uptimeMs =
    doc["uptime_ms"] | 0;

  JsonObject ble =
    doc["ble"].as<JsonObject>();

  if (!ble.isNull()) {
    r1.bleAuth =
      ble["authenticated"] | false;

    r1.bleFresh =
      ble["fresh"] | false;

    r1.bleRtc =
      ble["rtc_synced"] | false;

    strlcpy(
      r1.bleState,
      ble["state"] | "?",
      sizeof(r1.bleState)
    );
  }

  r1.online = true;
  r1.lastOkMs = millis();

  return true;
}

bool pollLive()
{
  JsonDocument doc;

  String path =
    "/api/live?after=" +
    String(
      (unsigned long long)liveCursor
    );

  if (!getJson(path, doc)) {
    return false;
  }

  if (doc["lost"] | false) {
    liveCursor = 0;
    return true;
  }

  JsonArray samples =
    doc["samples"].as<JsonArray>();

  for (JsonVariant item : samples) {

    JsonArray row =
      item.as<JsonArray>();

    if (row.size() != 6) {
      continue;
    }

    uint64_t id =
      row[0].as<uint64_t>();

    float volts =
      row[2].as<float>();

    float amps =
      row[3].as<float>();

    uint32_t q =
      row[4].as<uint32_t>();

    if (id <= liveCursor) {
      continue;
    }

    liveCursor = id;

    // Same validity mask used by the R1 browser UI.
    if ((q & 5U) == 0U) {
      r1.volts = volts;
      r1.amps = amps;
      r1.watts = volts * amps;
      r1.valid = true;
    }
  }

  return true;
}

// ---------- Input ----------

void handleButton()
{
  bool pressed =
    digitalRead(BOOT_BTN) == LOW;

  if (
    pressed &&
    !buttonDown
  ) {
    buttonDown = true;
    buttonDownAt = millis();
  }

  if (
    !pressed &&
    buttonDown
  ) {
    uint32_t held =
      millis() - buttonDownAt;

    buttonDown = false;

    if (held < 1200) {
      page =
        (page + 1) % 3;

      drawTerminal();

    } else if (
      held >= 2000 &&
      held < 6000
    ) {
      startOTA();

    } else if (
      held >= 6000
    ) {
      clearR1Config();
      ESP.restart();
    }
  }
}

// ---------- Arduino ----------

void setup()
{
  pinMode(
    BOOT_BTN,
    INPUT_PULLUP
  );

  setBacklight(20);

  Serial.begin(115200);
  delay(250);

  if (!gfx->begin(80000000)) {
    while (true) {
      delay(1000);
    }
  }

  configureLocalServer();

  if (!loadR1Config()) {
    startSetupPortal();
    return;
  }

  if (!connectR1()) {
    startSetupPortal();
    return;
  }

  runMode = MODE_TERMINAL;

  pollState();
  pollLive();
  drawTerminal();
}

void loop()
{
  if (
    runMode == MODE_SETUP ||
    runMode == MODE_OTA
  ) {
    localServer.handleClient();
    delay(2);
    return;
  }

  handleButton();

  if (
    runMode != MODE_TERMINAL
  ) {
    return;
  }

  uint32_t now = millis();

  if (
    WiFi.status() != WL_CONNECTED
  ) {
    r1.online = false;

    if (
      now - lastReconnect > 3000
    ) {
      lastReconnect = now;
      WiFi.reconnect();
    }

  } else {

    if (
      now - lastStatePoll >= 1000
    ) {
      lastStatePoll = now;

      if (!pollState()) {
        if (
          now - r1.lastOkMs > 4000
        ) {
          r1.online = false;
        }
      }
    }

    if (
      now - lastLivePoll >= 200
    ) {
      lastLivePoll = now;
      pollLive();
    }
  }

  if (
    now - r1.lastOkMs > 5000
  ) {
    r1.online = false;
  }

  if (
    now - lastDraw >= 300
  ) {
    lastDraw = now;
    drawTerminal();
  }

  delay(5);
}
