#define SPI_DRIVER_SELECT 2
#include <Arduino.h>
#include <MCUFRIEND_kbv.h>
#include <TouchScreen.h>
#include <SdFat.h>
#include <string.h>

// Arduino Mega 2560 + MCUFRIEND/MAR2808-style 2.8 inch shield.
// Read-only hardware test: TFT drawing, resistive touch, SD mount/list/read.

MCUFRIEND_kbv tft;

// MAR2808 touch wiring used by the Phone project.
const int XP = 8;
const int YP = A3;
const int XM = A2;
const int YM = 9;
TouchScreen ts(XP, YP, XM, YM, 300);

// Measured by MCUFRIEND_kbv TouchScreen_Calibr_native in landscape.
// This shield swaps raw X/Y relative to screen X/Y.
const int16_t TS_LEFT = 867;
const int16_t TS_RIGHT = 167;
const int16_t TS_TOP = 177;
const int16_t TS_BOTTOM = 953;
const int MIN_PRESSURE = 80;
const int MAX_PRESSURE = 1000;

// The shield's microSD socket uses the Uno-style D10-D13 wiring.
// On Mega these are GPIO 10-13, so use software SPI (not Mega hardware SPI 50-53).
const uint8_t SD_CS_PIN = 10;
const uint8_t SD_MISO_PIN = 12;
const uint8_t SD_MOSI_PIN = 11;
const uint8_t SD_SCK_PIN = 13;
SoftSpiDriver<SD_MISO_PIN, SD_MOSI_PIN, SD_SCK_PIN> softSpi;
SdFs sd;
#define SD_CONFIG SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(0), &softSpi)

const uint16_t C_BG = 0x0841;
const uint16_t C_PANEL = 0x10A2;
const uint16_t C_WHITE = 0xFFFF;
const uint16_t C_MUTED = 0xA514;
const uint16_t C_GREEN = 0x2E66;
const uint16_t C_RED = 0xE2A4;
const uint16_t C_BLUE = 0x34DF;
const uint16_t C_YELLOW = 0xFEC0;

enum Page : uint8_t { PAGE_HOME, PAGE_TOUCH, PAGE_SD, PAGE_COLOR, PAGE_PHONE };
Page page = PAGE_HOME;
uint16_t lcdId = 0;
bool sdMounted = false;
bool touchLatched = false;
bool sdReadOk = false;
uint16_t sdFileCount = 0;
uint16_t sdBytesRead = 0;
char sdFirstFile[36] = "(no files)";
uint32_t lastValidTouchAt = 0;
uint8_t colorIndex = 0;
const uint16_t testColors[] = { C_BLUE, C_GREEN, C_RED, C_YELLOW };

void printHex16(uint16_t value) {
  Serial.print(F("0x"));
  if (value < 0x1000) Serial.print('0');
  if (value < 0x0100) Serial.print('0');
  if (value < 0x0010) Serial.print('0');
  Serial.print(value, HEX);
}

void centerText(const char *text, int16_t y, uint8_t size, uint16_t color) {
  int16_t textWidth = (int16_t)strlen(text) * 6 * size;
  int16_t x = (tft.width() - textWidth) / 2;
  if (x < 2) x = 2;
  tft.setTextSize(size);
  tft.setTextColor(color, C_BG);
  tft.setCursor(x, y);
  tft.print(text);
}

void drawButton(int16_t x, int16_t y, int16_t w, int16_t h,
                const char *label, uint16_t fill, uint8_t size = 2) {
  tft.fillRoundRect(x, y, w, h, 7, fill);
  tft.drawRoundRect(x, y, w, h, 7, C_WHITE);
  int16_t textWidth = (int16_t)strlen(label) * 6 * size;
  int16_t textHeight = 8 * size;
  tft.setTextSize(size);
  tft.setTextColor(C_WHITE, fill);
  tft.setCursor(x + (w - textWidth) / 2, y + (h - textHeight) / 2 + 1);
  tft.print(label);
}

void drawHeader(const char *title) {
  tft.fillRect(0, 0, 320, 30, C_PANEL);
  tft.setTextSize(2);
  tft.setTextColor(C_WHITE, C_PANEL);
  tft.setCursor(8, 7);
  tft.print(title);
  tft.setTextSize(1);
  tft.setCursor(260, 11);
  tft.print(F("MEGA"));
}

void drawHome() {
  tft.fillScreen(C_BG);
  drawHeader("SHIELD TEST");
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(16, 42);
  tft.print(F("TFT ID: "));
  tft.print(lcdId, HEX);
  tft.setCursor(174, 42);
  tft.print(F("SD: "));
  tft.setTextColor(sdMounted ? C_GREEN : C_RED, C_BG);
  tft.print(sdMounted ? F("READ OK") : F("NOT READY"));

  drawButton(15, 67, 137, 48, "TOUCH TEST", C_BLUE, 1);
  drawButton(168, 67, 137, 48, "SD READ", C_GREEN, 2);
  drawButton(15, 128, 137, 48, "COLOR TEST", C_PANEL, 1);
  drawButton(168, 128, 137, 48, "PHONE UI", C_PANEL, 2);

  tft.setTextSize(1);
  tft.setTextColor(C_MUTED, C_BG);
  tft.setCursor(12, 195);
  tft.print(F("Touch anywhere: raw and XY print to Serial."));
  tft.setCursor(12, 214);
  tft.print(F("USB Serial Monitor: 115200 baud"));
}

void drawTouchPage() {
  tft.fillScreen(C_BG);
  drawHeader("TOUCH TEST");
  centerText("Tap screen; watch coordinates", 43, 1, C_MUTED);
  tft.drawRect(12, 65, 296, 112, C_BLUE);
  tft.drawFastHLine(155, 65, 10, C_MUTED);
  tft.drawFastVLine(160, 60, 10, C_MUTED);
  tft.drawFastHLine(155, 172, 10, C_MUTED);
  tft.drawFastVLine(160, 172, 10, C_MUTED);
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(16, 187);
  tft.print(F("RAW X/Y/Z and mapped XY appear in Serial."));
  drawButton(215, 204, 93, 29, "BACK", C_PANEL, 1);
}

void drawSdPage() {
  tft.fillScreen(C_BG);
  drawHeader("SD CARD TEST");
  tft.setTextSize(2);
  tft.setTextColor(sdMounted ? C_GREEN : C_RED, C_BG);
  tft.setCursor(18, 48);
  tft.print(sdMounted ? F("CARD FOUND") : F("CARD ERROR"));
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(18, 86);
  tft.print(F("Files in root: "));
  tft.print(sdFileCount);
  tft.setCursor(18, 108);
  tft.print(F("First file: "));
  tft.print(sdFirstFile);
  tft.setCursor(18, 130);
  tft.print(F("Bytes read: "));
  tft.print(sdBytesRead);
  tft.setCursor(18, 152);
  tft.print(F("Read test: "));
  tft.setTextColor(sdReadOk ? C_GREEN : (sdMounted ? C_YELLOW : C_RED), C_BG);
  tft.print(sdReadOk ? F("OK") : (sdMounted ? F("NO FILE") : F("FAILED")));
  tft.setTextColor(C_MUTED, C_BG);
  tft.setCursor(18, 177);
  tft.print(F("Read only; no card files changed."));
  drawButton(18, 204, 125, 29, "RECHECK", C_BLUE, 1);
  drawButton(177, 204, 125, 29, "BACK", C_PANEL, 1);
}

void drawColorPage() {
  tft.fillScreen(testColors[colorIndex]);
  tft.drawRect(8, 8, 304, 224, C_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(C_WHITE);
  tft.setCursor(62, 74);
  tft.print(F("COLOR TEST"));
  tft.setTextSize(1);
  tft.setCursor(74, 107);
  tft.print(F("Tap screen to change color"));
  drawButton(206, 193, 94, 29, "BACK", C_PANEL, 1);
}

void drawPhoneUiPreview() {
  tft.fillScreen(C_BG);
  drawHeader("MEGA PHONE");
  centerText("SIM800L CALL UI", 52, 2, C_WHITE);
  centerText("Test screen only - modem disconnected", 82, 1, C_MUTED);
  drawButton(38, 108, 244, 56, "OPEN DIALER", C_GREEN, 2);
  drawButton(18, 181, 86, 38, "1 2 3", C_PANEL, 1);
  drawButton(117, 181, 86, 38, "4 5 6", C_PANEL, 1);
  drawButton(216, 181, 86, 38, "CALL", C_BLUE, 1);
  drawButton(215, 204, 93, 29, "BACK", C_PANEL, 1);
}

void scanSdCard() {
  Serial.println(F("\n[SD] Starting read-only check"));
  sdMounted = false;
  sdReadOk = false;
  sdFileCount = 0;
  sdBytesRead = 0;
  strcpy(sdFirstFile, "(no files)");

  // Touch shields normally route their SD socket to D10-D13.
  if (!sd.begin(SD_CONFIG)) {
    Serial.println(F("[SD] Mount failed; library diagnostic:"));
    sd.initErrorPrint(&Serial);
    return;
  }
  sdMounted = true;
  Serial.println(F("[SD] Card mounted"));
  FsFile root;
  FsFile entry;
  if (!root.open("/")) {
    Serial.println(F("[SD] Could not open root directory"));
    sdMounted = false;
    return;
  }

  while (entry.openNext(&root, O_RDONLY)) {
    if (sdFileCount < 65535) ++sdFileCount;
    char name[36];
    name[0] = '\0';
    entry.getName(name, sizeof(name));
    Serial.print(F("[SD] "));
    Serial.print(name);
    if (entry.isDir()) {
      Serial.println('/');
    } else {
      Serial.print(F("  "));
      Serial.print((unsigned long)entry.fileSize());
      Serial.println(F(" bytes"));
      if (!sdReadOk) {
        strncpy(sdFirstFile, name, sizeof(sdFirstFile) - 1);
        sdFirstFile[sizeof(sdFirstFile) - 1] = '\0';
        uint8_t buffer[16];
        while (entry.available() && sdBytesRead < sizeof(buffer)) {
          int value = entry.read();
          if (value < 0) break;
          buffer[sdBytesRead++] = (uint8_t)value;
        }
        sdReadOk = true;  // Opening and reading the file both succeeded.
      }
    }
    entry.close();
  }
  root.close();

  if (sdFileCount == 0) {
    Serial.println(F("[SD] Card mounted, but root directory is empty."));
  } else if (!sdReadOk) {
    Serial.println(F("[SD] Root entries found, no regular file to read."));
  } else {
    Serial.print(F("[SD] Read "));
    Serial.print(sdBytesRead);
    Serial.println(F(" byte(s) from the first regular file."));
    Serial.print(F("[SD] First bytes (hex):"));
    FsFile first;
    if (first.open(sdFirstFile, O_RDONLY)) {
      for (uint8_t i = 0; i < sdBytesRead; ++i) {
        int value = first.read();
        if (value < 0) break;
        Serial.print(' ');
        if (value < 16) Serial.print('0');
        Serial.print(value, HEX);
      }
      first.close();
      Serial.println();
    }
  }
}

bool readTouch(int16_t &x, int16_t &y, TSPoint &p) {
  p = ts.getPoint();
  // Restore the shared display bus pins after reading the resistive panel.
  pinMode(XM, OUTPUT);
  pinMode(YP, OUTPUT);
  if (p.z < MIN_PRESSURE || p.z > MAX_PRESSURE) return false;
  x = map(p.y, TS_LEFT, TS_RIGHT, 0, tft.width() - 1);
  y = map(p.x, TS_TOP, TS_BOTTOM, 0, tft.height() - 1);
  x = constrain(x, 0, tft.width() - 1);
  y = constrain(y, 0, tft.height() - 1);
  return true;
}

void showTouchPoint(int16_t x, int16_t y, const TSPoint &p) {
  Serial.print(F("[TOUCH] rawX="));
  Serial.print(p.x);
  Serial.print(F(" rawY="));
  Serial.print(p.y);
  Serial.print(F(" pressure="));
  Serial.print(p.z);
  Serial.print(F(" -> x="));
  Serial.print(x);
  Serial.print(F(" y="));
  Serial.println(y);

  if (page == PAGE_TOUCH) {
    tft.fillRect(12, 65, 296, 112, C_BG);
    tft.drawRect(12, 65, 296, 112, C_BLUE);
    tft.drawFastHLine(155, 65, 10, C_MUTED);
    tft.drawFastVLine(160, 60, 10, C_MUTED);
    tft.drawFastHLine(155, 172, 10, C_MUTED);
    tft.drawFastVLine(160, 172, 10, C_MUTED);
    tft.fillCircle(x, y, 5, C_RED);
  } else if (page == PAGE_COLOR) {
    ++colorIndex;
    if (colorIndex >= sizeof(testColors) / sizeof(testColors[0])) colorIndex = 0;
    drawColorPage();
  } else if (page == PAGE_HOME) {
    tft.fillCircle(x, y, 3, C_YELLOW);
  }
}

void handleTouch(int16_t x, int16_t y) {
  if (page == PAGE_HOME) {
    if (y >= 67 && y <= 115 && x < 160) {
      page = PAGE_TOUCH;
      drawTouchPage();
    } else if (y >= 67 && y <= 115 && x >= 160) {
      page = PAGE_SD;
      scanSdCard();
      drawSdPage();
    } else if (y >= 128 && y <= 176 && x < 152) {
      page = PAGE_COLOR;
      drawColorPage();
    } else if (y >= 128 && y <= 176 && x >= 168) {
      drawPhoneUiPreview();
      page = PAGE_PHONE;
    } else {
      // If mapping is still off, an arbitrary tap opens touch diagnostics.
      page = PAGE_TOUCH;
      drawTouchPage();
    }
    return;
  }

  if (page == PAGE_TOUCH) {
    if (x >= 215 && y >= 204) {
      page = PAGE_HOME;
      drawHome();
    }
    return;
  }

  if (page == PAGE_SD) {
    if (y >= 204 && x < 150) {
      scanSdCard();
      drawSdPage();
    } else if (y >= 204 && x >= 170) {
      page = PAGE_HOME;
      drawHome();
    }
    return;
  }

  if (page == PAGE_COLOR) {
    if (x >= 206 && y >= 193) {
      page = PAGE_HOME;
      drawHome();
    } else {
      ++colorIndex;
      if (colorIndex >= sizeof(testColors) / sizeof(testColors[0])) colorIndex = 0;
      drawColorPage();
    }
    return;
  }

  if (page == PAGE_PHONE && x >= 215 && y >= 204) {
    page = PAGE_HOME;
    drawHome();
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println(F("\n=== Mega + 2.8 TFT Shield Test ==="));
  Serial.println(F("Connect only the Mega and shield for this test."));
  Serial.println(F("Touch the screen; coordinates print here."));

  tft.reset();
  lcdId = tft.readID();
  Serial.print(F("[TFT] readID="));
  printHex16(lcdId);
  Serial.println();
  uint16_t driverId = lcdId;
  if (driverId == 0xD3D3 || driverId == 0xFFFF || driverId == 0x0000) {
    Serial.println(F("[TFT] Unknown ID; trying ILI9341-compatible init."));
    driverId = 0x9341;
  }
  tft.begin(driverId);
  tft.setRotation(1);
  scanSdCard();
  drawHome();
  Serial.println(F("[READY] TFT, touch, and read-only SD test menu is shown."));
}

void loop() {
  int16_t x, y;
  TSPoint p;
  bool pressed = readTouch(x, y, p);
  if (pressed) {
    lastValidTouchAt = millis();
    if (!touchLatched) {
      touchLatched = true;
      showTouchPoint(x, y, p);
      handleTouch(x, y);
    }
  } else if (touchLatched && millis() - lastValidTouchAt > 120) {
    touchLatched = false;
  }
}
