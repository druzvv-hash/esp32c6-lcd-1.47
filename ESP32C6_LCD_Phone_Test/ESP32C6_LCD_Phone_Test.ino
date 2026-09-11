#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

// Waveshare ESP32-C6-LCD-1.47-M
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
  LCD_DC,
  LCD_CS,
  LCD_SCLK,
  LCD_MOSI,
  GFX_NOT_DEFINED
);

Arduino_GFX *gfx = new Arduino_ST7789(
  bus,
  LCD_RST,
  0,
  true,
  LCD_W,
  LCD_H,
  LCD_X_OFFSET,
  LCD_Y_OFFSET,
  LCD_X_OFFSET,
  LCD_Y_OFFSET
);

WebServer otaServer(80);

static const char *OTA_SSID = "C6-OTA";
static const char *OTA_PASS = "C6update47";

static bool bright = false;
static bool otaMode = false;

static bool buttonDown = false;
static bool longPressHandled = false;
static uint32_t buttonDownAt = 0;
static uint32_t lastDraw = 0;

void setBacklight(uint8_t percent)
{
  percent = constrain(percent, 0, 50);

  ledcAttach(LCD_BL, 5000, 8);
  ledcWrite(
    LCD_BL,
    map(percent, 0, 100, 0, 255)
  );
}

void drawHeader()
{
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextWrap(false);

  gfx->setTextColor(RGB565_CYAN);
  gfx->setTextSize(2);
  gfx->setCursor(12, 18);
  gfx->println("ESP32-C6");

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(12, 48);
  gfx->println("PHONE + OTA TEST");

  gfx->drawFastHLine(
    10,
    66,
    LCD_W - 20,
    RGB565_DARKGREY
  );
}

void drawStaticInfo()
{
  gfx->setTextColor(RGB565_GREEN);
  gfx->setCursor(12, 82);
  gfx->println("LCD: ST7789 OK");

  gfx->setTextColor(RGB565_WHITE);

  gfx->setCursor(12, 104);
  gfx->printf(
    "Chip: %s\n",
    ESP.getChipModel()
  );

  gfx->setCursor(12, 122);
  gfx->printf(
    "Rev:  %u\n",
    ESP.getChipRevision()
  );

  gfx->setCursor(12, 140);
  gfx->printf(
    "CPU:  %u MHz\n",
    ESP.getCpuFreqMHz()
  );

  gfx->setCursor(12, 158);
  gfx->printf(
    "Flash:%u MB\n",
    (unsigned)(
      ESP.getFlashChipSize() /
      (1024UL * 1024UL)
    )
  );

  gfx->setCursor(12, 176);
  gfx->printf(
    "Heap: %u KB\n",
    (unsigned)(
      ESP.getFreeHeap() / 1024UL
    )
  );

  gfx->setTextColor(RGB565_YELLOW);
  gfx->setCursor(12, 202);
  gfx->println("BOOT short = BL");

  gfx->setCursor(12, 218);
  gfx->println("BOOT 2s = OTA");

  gfx->setTextColor(RGB565_LIGHTGREY);
  gfx->setCursor(12, 242);
  gfx->println("Waveshare 1.47-M");
}

void drawDynamic()
{
  gfx->fillRect(
    10,
    270,
    LCD_W - 20,
    40,
    RGB565_BLACK
  );

  gfx->setTextColor(RGB565_MAGENTA);
  gfx->setCursor(12, 274);
  gfx->printf(
    "UP %lus\n",
    millis() / 1000UL
  );

  gfx->setTextColor(
    bright ?
    RGB565_GREEN :
    RGB565_CYAN
  );

  gfx->setCursor(12, 292);
  gfx->printf(
    "BL %s (%u%%)",
    bright ? "HIGH" : "LOW",
    bright ? 50 : 20
  );
}

void drawOTA()
{
  gfx->fillScreen(RGB565_BLACK);

  gfx->setTextColor(RGB565_YELLOW);
  gfx->setTextSize(2);
  gfx->setCursor(12, 18);
  gfx->println("OTA MODE");

  gfx->setTextSize(1);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(12, 60);
  gfx->println("WiFi:");

  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(12, 78);
  gfx->println(OTA_SSID);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(12, 108);
  gfx->println("Password:");

  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(12, 126);
  gfx->println(OTA_PASS);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(12, 158);
  gfx->println("Open:");

  gfx->setTextColor(RGB565_GREEN);
  gfx->setCursor(12, 176);
  gfx->println("192.168.4.1");

  gfx->setTextColor(RGB565_LIGHTGREY);
  gfx->setCursor(12, 216);
  gfx->println("Select .bin");

  gfx->setCursor(12, 232);
  gfx->println("and upload.");
}

void drawOTAWriting(size_t bytes)
{
  gfx->fillRect(
    10,
    260,
    LCD_W - 20,
    48,
    RGB565_BLACK
  );

  gfx->setTextColor(RGB565_MAGENTA);
  gfx->setCursor(12, 270);
  gfx->println("Writing firmware");

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(12, 288);
  gfx->printf(
    "%u KB",
    (unsigned)(bytes / 1024)
  );
}

void startOTA()
{
  if (otaMode) {
    return;
  }

  otaMode = true;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(
    OTA_SSID,
    OTA_PASS
  );

  Serial.println();
  Serial.println("OTA MODE");
  Serial.print("SSID: ");
  Serial.println(OTA_SSID);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  drawOTA();

  otaServer.on(
    "/",
    HTTP_GET,
    []()
    {
      static const char page[] =
        "<!doctype html>"
        "<html>"
        "<head>"
        "<meta name='viewport' "
        "content='width=device-width,"
        "initial-scale=1'>"
        "<title>ESP32-C6 OTA</title>"
        "</head>"
        "<body style='font-family:sans-serif;"
        "max-width:500px;margin:40px auto;"
        "padding:20px'>"
        "<h2>ESP32-C6 OTA</h2>"
        "<p>Select the application .bin "
        "from GitHub Actions.</p>"
        "<form method='POST' "
        "action='/update' "
        "enctype='multipart/form-data'>"
        "<input type='file' "
        "name='firmware' "
        "accept='.bin'>"
        "<br><br>"
        "<input type='submit' "
        "value='Upload firmware'>"
        "</form>"
        "</body>"
        "</html>";

      otaServer.send(
        200,
        "text/html",
        page
      );
    }
  );

  otaServer.on(
    "/update",
    HTTP_POST,

    []()
    {
      bool ok = !Update.hasError();

      otaServer.send(
        200,
        "text/plain",
        ok ?
          "UPDATE OK - REBOOTING" :
          "UPDATE FAILED"
      );

      delay(1200);

      if (ok) {
        ESP.restart();
      }
    },

    []()
    {
      HTTPUpload &upload =
        otaServer.upload();

      if (
        upload.status ==
        UPLOAD_FILE_START
      ) {
        Serial.printf(
          "OTA start: %s\n",
          upload.filename.c_str()
        );

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

        drawOTAWriting(
          upload.totalSize
        );

      } else if (
        upload.status ==
        UPLOAD_FILE_END
      ) {
        if (Update.end(true)) {
          Serial.printf(
            "OTA success: %u bytes\n",
            upload.totalSize
          );
        } else {
          Update.printError(Serial);
        }
      }
    }
  );

  otaServer.begin();
}

void setup()
{
  pinMode(
    BOOT_BTN,
    INPUT_PULLUP
  );

  setBacklight(20);

  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println(
    "ESP32-C6-LCD-1.47-M"
  );

  if (!gfx->begin(80000000)) {
    Serial.println(
      "LCD init failed"
    );

    while (true) {
      delay(1000);
    }
  }

  drawHeader();
  drawStaticInfo();
  drawDynamic();
}

void loop()
{
  if (otaMode) {
    otaServer.handleClient();
    delay(2);
    return;
  }

  bool pressed =
    digitalRead(BOOT_BTN) == LOW;

  if (
    pressed &&
    !buttonDown
  ) {
    buttonDown = true;
    longPressHandled = false;
    buttonDownAt = millis();
  }

  if (
    pressed &&
    buttonDown &&
    !longPressHandled &&
    millis() - buttonDownAt >= 2000
  ) {
    longPressHandled = true;
    startOTA();
    return;
  }

  if (
    !pressed &&
    buttonDown
  ) {
    uint32_t duration =
      millis() - buttonDownAt;

    buttonDown = false;

    if (
      !longPressHandled &&
      duration < 2000
    ) {
      bright = !bright;

      setBacklight(
        bright ? 50 : 20
      );

      drawDynamic();
    }
  }

  if (
    millis() - lastDraw >= 1000
  ) {
    lastDraw = millis();

    drawDynamic();

    Serial.printf(
      "uptime=%lus "
      "flash=%u "
      "free_heap=%u\n",
      millis() / 1000UL,
      (unsigned)
        ESP.getFlashChipSize(),
      (unsigned)
        ESP.getFreeHeap()
    );
  }

  delay(5);
}
