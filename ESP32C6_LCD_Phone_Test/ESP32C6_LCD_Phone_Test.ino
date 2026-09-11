#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// Waveshare ESP32-C6-LCD-1.47 / 1.47-M
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

// Shared SPI bus with microSD. MISO isn't needed by the LCD.
Arduino_DataBus *bus = new Arduino_ESP32SPI(
  LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, GFX_NOT_DEFINED
);

// ST7789, native 172x320 window inside 240-wide controller RAM.
Arduino_GFX *gfx = new Arduino_ST7789(
  bus,
  LCD_RST,
  0,          // rotation
  true,       // IPS
  LCD_W,
  LCD_H,
  LCD_X_OFFSET,
  LCD_Y_OFFSET,
  LCD_X_OFFSET,
  LCD_Y_OFFSET
);

static bool bright = false;
static uint32_t lastDraw = 0;

void setBacklight(uint8_t percent) {
  percent = constrain(percent, 0, 50); // Waveshare recommends <= 50% for long use
  ledcAttach(LCD_BL, 5000, 8);
  ledcWrite(LCD_BL, map(percent, 0, 100, 0, 255));
}

void drawHeader() {
  gfx->fillScreen(BLACK);
  gfx->setTextWrap(false);

  gfx->setTextColor(CYAN);
  gfx->setTextSize(2);
  gfx->setCursor(12, 18);
  gfx->println("ESP32-C6");

  gfx->setTextColor(WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(12, 48);
  gfx->println("PHONE FLASH TEST");

  gfx->drawFastHLine(10, 66, LCD_W - 20, DARKGREY);
}

void drawStaticInfo() {
  gfx->setTextColor(GREEN);
  gfx->setCursor(12, 82);
  gfx->println("LCD: ST7789 OK");

  gfx->setTextColor(WHITE);
  gfx->setCursor(12, 104);
  gfx->printf("Chip: %s\n", ESP.getChipModel());

  gfx->setCursor(12, 122);
  gfx->printf("Rev:  %u\n", ESP.getChipRevision());

  gfx->setCursor(12, 140);
  gfx->printf("CPU:  %u MHz\n", ESP.getCpuFreqMHz());

  gfx->setCursor(12, 158);
  gfx->printf("Flash:%u MB\n",
              (unsigned)(ESP.getFlashChipSize() / (1024UL * 1024UL)));

  gfx->setCursor(12, 176);
  gfx->printf("Heap: %u KB\n",
              (unsigned)(ESP.getFreeHeap() / 1024UL));

  gfx->setTextColor(YELLOW);
  gfx->setCursor(12, 208);
  gfx->println("BOOT = brightness");

  gfx->setTextColor(LIGHTGREY);
  gfx->setCursor(12, 228);
  gfx->println("Built for:");
  gfx->setCursor(12, 244);
  gfx->println("Waveshare 1.47-M");
}

void drawDynamic() {
  // overwrite dynamic area
  gfx->fillRect(10, 270, LCD_W - 20, 38, BLACK);

  gfx->setTextColor(MAGENTA);
  gfx->setCursor(12, 274);
  gfx->printf("UP %lus\n", millis() / 1000UL);

  gfx->setTextColor(bright ? GREEN : CYAN);
  gfx->setCursor(12, 292);
  gfx->printf("BL %s (%u%%)",
              bright ? "HIGH" : "LOW",
              bright ? 50 : 20);
}

void setup() {
  pinMode(BOOT_BTN, INPUT_PULLUP);

  // Start with a conservative backlight level.
  setBacklight(20);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("ESP32-C6-LCD-1.47-M phone flash test");

  if (!gfx->begin(80000000)) {
    Serial.println("LCD init failed");
    while (true) delay(1000);
  }

  drawHeader();
  drawStaticInfo();
  drawDynamic();
}

void loop() {
  static bool lastButton = HIGH;
  bool now = digitalRead(BOOT_BTN);

  if (lastButton == HIGH && now == LOW) {
    bright = !bright;
    setBacklight(bright ? 50 : 20);
    drawDynamic();
    delay(30);
  }
  lastButton = now;

  if (millis() - lastDraw >= 1000) {
    lastDraw = millis();
    drawDynamic();
    Serial.printf("uptime=%lus flash=%u bytes free_heap=%u\n",
                  millis() / 1000UL,
                  (unsigned)ESP.getFlashChipSize(),
                  (unsigned)ESP.getFreeHeap());
  }

  delay(5);
}
