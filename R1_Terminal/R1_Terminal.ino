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
#define FW_VERSION "v0.4.0"
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

bool pageDirty = true;
String uiCache[12];

void clearScreen()
{
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextWrap(false);
}

void resetUiCache()
{
  for (auto &v : uiCache) {
    v = "";
  }
}

void drawCached(
  uint8_t slot,
  int16_t x,
  int16_t y,
  int16_t w,
  int16_t h,
  uint8_t size,
  uint16_t color,
  const char *text
)
{
  String v(text);

  if (uiCache[slot] == v) {
    return;
  }

  uiCache[slot] = v;

  gfx->fillRect(
    x, y, w, h,
    RGB565_BLACK
  );

  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(text);
}

void drawHeaderStatic(
  const char *title
)
{
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(6, 4);
  gfx->print(title);

  gfx->drawFastHLine(
    6,
    25,
    LCD_W - 12,
    RGB565_DARKGREY
  );
}

void drawFooter(
  const char *text
)
{
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_LIGHTGREY);
  gfx->setCursor(6, 307);
  gfx->print(text);
}

void updateOnline()
{
  drawCached(
    0,
    145, 6,
    24, 12,
    1,
    r1.online ?
      RGB565_GREEN :
      RGB565_RED,
    r1.online ? "ON" : "OFF"
  );
}

void drawMainStatic()
{
  drawHeaderStatic("R1 LIVE");
  drawFooter("1/4  BOOT = next");
}

void drawMainDynamic()
{
  char b[40];

  if (
    r1.online &&
    isfinite(r1.amps)
  ) {
    snprintf(
      b, sizeof(b),
      "%+.2fA",
      r1.amps
    );
  } else {
    strlcpy(
      b, "--.--A",
      sizeof(b)
    );
  }

  drawCached(
    1,
    7, 39,
    160, 38,
    4,
    RGB565_CYAN,
    b
  );

  if (
    r1.online &&
    isfinite(r1.volts)
  ) {
    snprintf(
      b, sizeof(b),
      "%.2fV",
      r1.volts
    );
  } else {
    strlcpy(
      b, "--.--V",
      sizeof(b)
    );
  }

  drawCached(
    2,
    7, 88,
    160, 38,
    4,
    RGB565_YELLOW,
    b
  );

  if (
    r1.online &&
    isfinite(r1.watts)
  ) {
    snprintf(
      b, sizeof(b),
      "%+.1fW",
      r1.watts
    );
  } else {
    strlcpy(
      b, "---.-W",
      sizeof(b)
    );
  }

  drawCached(
    3,
    8, 139,
    158, 30,
    3,
    RGB565_MAGENTA,
    b
  );

  bool recording =
    !strcmp(
      r1.recState,
      "RUNNING"
    );

  snprintf(
    b, sizeof(b),
    "REC %s",
    recording ? "ON" : "OFF"
  );

  drawCached(
    4,
    8, 184,
    156, 24,
    2,
    recording ?
      RGB565_RED :
      RGB565_GREEN,
    b
  );

  snprintf(
    b, sizeof(b),
    "Hz %u / %.1f",
    r1.requestedHz,
    r1.measuredHz
  );

  drawCached(
    5,
    8, 225,
    156, 24,
    2,
    RGB565_WHITE,
    b
  );
}

void drawRecordingStatic()
{
  drawHeaderStatic("RECORD");
  drawFooter("2/4  BOOT = next");
}

void drawRecordingDynamic()
{
  char b[48];

  bool recording =
    !strcmp(
      r1.recState,
      "RUNNING"
    );

  drawCached(
    1,
    8, 39,
    158, 30,
    3,
    recording ?
      RGB565_RED :
      RGB565_GREEN,
    r1.recState
  );

  uint32_t total =
    (uint32_t)r1.recSeconds;

  uint32_t hours =
    total / 3600;

  uint32_t minutes =
    (total % 3600) / 60;

  uint32_t seconds =
    total % 60;

  snprintf(
    b, sizeof(b),
    "%02lu:%02lu:%02lu",
    (unsigned long)hours,
    (unsigned long)minutes,
    (unsigned long)seconds
  );

  drawCached(
    2,
    8, 88,
    158, 24,
    2,
    RGB565_WHITE,
    b
  );

  snprintf(
    b, sizeof(b),
    "Rows %llu",
    (unsigned long long)r1.recRows
  );

  drawCached(
    3,
    8, 121,
    158, 24,
    2,
    RGB565_WHITE,
    b
  );

  snprintf(
    b, sizeof(b),
    "Wh %.3f",
    r1.recWh
  );

  drawCached(
    4,
    8, 158,
    158, 24,
    2,
    RGB565_YELLOW,
    b
  );

  snprintf(
    b, sizeof(b),
    "Ah %.3f",
    r1.recAh
  );

  drawCached(
    5,
    8, 191,
    158, 24,
    2,
    RGB565_YELLOW,
    b
  );

  snprintf(
    b, sizeof(b),
    "FIFO %lu/%lu  OF %lu",
    (unsigned long)r1.recQueued,
    (unsigned long)r1.recHighWater,
    (unsigned long)r1.recOverflows
  );

  drawCached(
    6,
    8, 236,
    158, 12,
    1,
    RGB565_WHITE,
    b
  );

  snprintf(
    b, sizeof(b),
    "Miss %lu  Inv %lu",
    (unsigned long)r1.missed,
    (unsigned long)r1.invalid
  );

  drawCached(
    7,
    8, 256,
    158, 12,
    1,
    RGB565_WHITE,
    b
  );
}

void drawLoggerStatic()
{
  drawHeaderStatic("LOGGER");
  drawFooter("3/4  BOOT = next");
}

void drawLoggerDynamic()
{
  char b[40];

  snprintf(
    b, sizeof(b),
    "INA %s",
    r1.ina
  );

  drawCached(
    1,
    8, 42,
    158, 24,
    2,
    RGB565_GREEN,
    b
  );

  snprintf(
    b, sizeof(b),
    "SD %s",
    r1.sd
  );

  drawCached(
    2,
    8, 78,
    158, 24,
    2,
    RGB565_GREEN,
    b
  );

  snprintf(
    b, sizeof(b),
    "RTC %s",
    r1.rtc
  );

  drawCached(
    3,
    8, 114,
    158, 24,
    2,
    RGB565_GREEN,
    b
  );

  snprintf(
    b, sizeof(b),
    "EEP %s",
    r1.eeprom
  );

  drawCached(
    4,
    8, 150,
    158, 24,
    2,
    RGB565_GREEN,
    b
  );

  if (isfinite(r1.tempC)) {
    snprintf(
      b, sizeof(b),
      "TEMP %.1fC",
      r1.tempC
    );
  } else {
    strlcpy(
      b, "TEMP --.-C",
      sizeof(b)
    );
  }

  drawCached(
    5,
    8, 192,
    158, 24,
    2,
    RGB565_WHITE,
    b
  );

  snprintf(
    b, sizeof(b),
    "WiFi %ddBm",
    WiFi.RSSI()
  );

  drawCached(
    6,
    8, 228,
    158, 24,
    2,
    RGB565_WHITE,
    b
  );

  snprintf(
    b, sizeof(b),
    "R1 up %lus",
    (unsigned long)(
      r1.uptimeMs / 1000
    )
  );

  drawCached(
    7,
    8, 274,
    158, 12,
    1,
    RGB565_LIGHTGREY,
    b
  );
}

void drawR3Static()
{
  drawHeaderStatic("R3 STATUS");
  drawFooter("4/4  BOOT = next");
}

void drawR3Dynamic()
{
  char b[48];

  snprintf(
    b, sizeof(b),
    "BLE %s",
    r1.bleState
  );

  drawCached(
    1,
    8, 43,
    158, 24,
    2,
    r1.bleFresh ?
      RGB565_GREEN :
      RGB565_YELLOW,
    b
  );

  snprintf(
    b, sizeof(b),
    "AUTH %s",
    r1.bleAuth ?
      "YES" :
      "NO"
  );

  drawCached(
    2,
    8, 83,
    158, 24,
    2,
    r1.bleAuth ?
      RGB565_GREEN :
      RGB565_YELLOW,
    b
  );

  snprintf(
    b, sizeof(b),
    "LIVE %s",
    r1.bleFresh ?
      "YES" :
      "NO"
  );

  drawCached(
    3,
    8, 123,
    158, 24,
    2,
    r1.bleFresh ?
      RGB565_GREEN :
      RGB565_YELLOW,
    b
  );

  snprintf(
    b, sizeof(b),
    "RTC %s",
    r1.bleRtc ?
      "SYNC" :
      "--"
  );

  drawCached(
    4,
    8, 163,
    158, 24,
    2,
    r1.bleRtc ?
      RGB565_GREEN :
      RGB565_YELLOW,
    b
  );

  snprintf(
    b, sizeof(b),
    "DROP %lu",
    (unsigned long)
      r1.previewDrops
  );

  drawCached(
    5,
    8, 207,
    158, 24,
    2,
    r1.previewDrops == 0 ?
      RGB565_GREEN :
      RGB565_RED,
    b
  );

  snprintf(
    b, sizeof(b),
    "R1 %s",
    r1.firmware
  );

  drawCached(
    6,
    8, 258,
    158, 12,
    1,
    RGB565_LIGHTGREY,
    b
  );

  snprintf(
    b, sizeof(b),
    "TERM %s %s",
    FW_VERSION,
    FW_GIT_HASH
  );

  drawCached(
    7,
    8, 276,
    158, 12,
    1,
    RGB565_LIGHTGREY,
    b
  );
}

void drawTerminal()
{
  if (pageDirty) {
    clearScreen();
    resetUiCache();

    if (page == 0) {
      drawMainStatic();
    } else if (page == 1) {
      drawRecordingStatic();
    } else if (page == 2) {
      drawLoggerStatic();
    } else {
      drawR3Static();
    }

    pageDirty = false;
  }

  updateOnline();

  if (page == 0) {
    drawMainDynamic();
  } else if (page == 1) {
    drawRecordingDynamic();
  } else if (page == 2) {
    drawLoggerDynamic();
  } else {
    drawR3Dynamic();
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
  HTTPClient http;

  String url =
    String("http://") +
    R1_IP.toString() +
    path;

  http.setConnectTimeout(800);
  http.setTimeout(1800);

  if (!http.begin(client, url)) {
    lastHttpCode = -11;
    lastContentLength = -1;

    strlcpy(
      lastNetError,
      "HTTP begin failed",
      sizeof(lastNetError)
    );

    return false;
  }

  int code = http.GET();

  lastHttpCode = code;
  lastContentLength = http.getSize();

  if (code != HTTP_CODE_OK) {
    snprintf(
      lastNetError,
      sizeof(lastNetError),
      "HTTP %d",
      code
    );

    http.end();
    return false;
  }

  String payload =
    http.getString();

  http.end();

  lastContentLength =
    payload.length();

  if (payload.length() == 0) {
    strlcpy(
      lastNetError,
      "Empty body",
      sizeof(lastNetError)
    );

    return false;
  }

  DeserializationError error =
    deserializeJson(
      doc,
      payload
    );

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

  // Any valid R1 API response is our connection heartbeat.
  r1.online = true;
  r1.lastOkMs = millis();

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

  r1.online = true;
  r1.lastOkMs = millis();

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
  if (runMode == MODE_OTA) {
    return;
  }

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

    if (held >= 6000) {
      clearR1Config();
      ESP.restart();
      return;
    }

    if (held >= 2000) {
      startOTA();
      return;
    }

    if (
      held < 1200 &&
      runMode == MODE_TERMINAL
    ) {
      page =
        (page + 1) % 4;

      pageDirty = true;
      drawTerminal();
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
  if (runMode != MODE_OTA) {
    handleButton();
  }

  if (
    runMode == MODE_SETUP ||
    runMode == MODE_OTA
  ) {
    localServer.handleClient();
    delay(2);
    return;
  }

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

  // Use a fresh millis() value here.
  // HTTP polling can update lastOkMs after "now" was captured.
  if (
    (uint32_t)(millis() - r1.lastOkMs) > 5000U
  ) {
    r1.online = false;
  }

  if (
    now - lastDraw >= 500
  ) {
    lastDraw = now;
    drawTerminal();
  }

  delay(5);
}
