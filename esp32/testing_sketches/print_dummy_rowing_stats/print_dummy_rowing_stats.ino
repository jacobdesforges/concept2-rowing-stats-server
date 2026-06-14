#include "secrets.h"
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_sleep.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <ArduinoJson.h>

// Adafruit GFX Fonts
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

// --- WiFi Credentials ---
const char* ssid     = SECRET_SSID;
const char* password = SECRET_PASSWORD;

// --- Hardware Pins ---
const int buttonPin  = 32;

// E-Ink Pins mapped from custom driver board
#define EPD_BUSY 25
#define EPD_RST  26
#define EPD_DC   27
#define EPD_CS   15
#define EPD_CLK  13
#define EPD_DIN  14

// --- Display Initialization (Rev 2.2 Panel) ---
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> display(GxEPD2_420_GDEY042T81(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// --- State Variables ---
unsigned long maintenance_start_time = 0;
bool is_double_click = false;

void setup() {
  Serial.begin(115200);
  delay(100); 

  pinMode(buttonPin, INPUT_PULLUP);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    delay(50); // Initial debounce
    if (digitalRead(buttonPin) == LOW) {
      Serial.println("Button held down, ignoring");
      go_to_sleep();
      return;
    }

    unsigned long wait_start = millis();
    is_double_click = false;
    while (millis() - wait_start < 500) {
      if (digitalRead(buttonPin) == LOW) {
        delay(50); // Debounce
        if (digitalRead(buttonPin) == LOW) {
          is_double_click = true;
          while (digitalRead(buttonPin) == LOW) {
            delay(10);
          }
          break; 
        }
      }
      delay(10);
    }

    if (is_double_click) {
      Serial.println("Double click detected");
    } else {
      Serial.println("Single click detected");
    }
  } else {
    Serial.println("Power-on reset detected");
    go_to_sleep();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected");

  ArduinoOTA.setHostname("erg-fridge-display");
  ArduinoOTA.begin();

  if (is_double_click) {
    Serial.println("Entering maintenance mode (120s OTA window)");
    maintenance_start_time = millis();
  } else {
    Serial.println("Single click: Parsing dummy JSON and updating display...");
    updateDisplayWithStats();
    go_to_sleep();
  }
}

void loop() {
  ArduinoOTA.handle();

  if (is_double_click && millis() - maintenance_start_time > 120000) {
    Serial.println("Maintenance mode ended. Going to sleep.");
    go_to_sleep();
  }
}

void updateDisplayWithStats() {
  // Init SPI and Display
  SPI.begin(EPD_CLK, -1, EPD_DIN, EPD_CS);
  display.init(115200, true, 2, false); 
  
  // Set rotation to 270 degrees counter-clockwise (opposite of 1)
  display.setRotation(0); 
  //1 orients text with top being the 8 pinout, 2 and 3 rotate +90 degrees clockwise each from there

  // --- Parse Dummy JSON Payload ---
  const char* dummy_payload = "{\"lifetime\": \"733,761m\", \"season\": \"202,757m\", \"pr_1k\": \"3:37.2\", \"pr_2k\": \"7:36.3\", \"pr_5k\": \"20:32\", \"pr_10k\": \"43:31\", \"pr_30m\": \"6,583m\", \"pr_1hr\": \"13,531m\", \"marathons\": \"2\", \"daily_streak\": \"22\"}";
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, dummy_payload);
  
  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return;
  }

  // --- Render Layout ---
  // In Adafruit GFX, text coordinates (X, Y) represent the bottom-left baseline of the text, not the top-left corner.
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    int16_t x1, y1;
    uint16_t w, h;

    // --- Top Section: Lifetime & Season ---
    // Headers (Centered)
    display.setFont(&FreeSansBold9pt7b);
    
    display.getTextBounds("LIFETIME METERS", 0, 0, &x1, &y1, &w, &h);
    display.setCursor(100 - (w / 2), 30);
    display.print("LIFETIME METERS");

    display.getTextBounds("SEASON METERS", 0, 0, &x1, &y1, &w, &h);
    display.setCursor(300 - (w / 2), 30);
    display.print("SEASON METERS");

    // Values (Largest Font 18pt - Centered)
    display.setFont(&FreeSansBold18pt7b);
    
    const char* lifeStr = doc["lifetime"].as<const char*>();
    display.getTextBounds(lifeStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(100 - (w / 2), 70);
    display.print(lifeStr);

    const char* seasonStr = doc["season"].as<const char*>();
    display.getTextBounds(seasonStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(300 - (w / 2), 70);
    display.print(seasonStr);

    // --- Grid Lines ---
    // Horizontal divider below top section
    display.drawLine(0, 90, 400, 90, GxEPD_BLACK);
    // Vertical divider down the center
    display.drawLine(200, 0, 200, 300, GxEPD_BLACK); 

    // --- Bottom Section: 2 Columns ---
    int leftColX = 8;
    int rightColX = 208;
    int rowStart = 135;
    int rowHeight = 45;

    // Left Column (Distance PRs)
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(leftColX, rowStart);
    display.print("1k PR: ");
    display.setFont(&FreeSans12pt7b);
    display.print(doc["pr_1k"].as<const char*>());

    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(leftColX, rowStart + rowHeight);
    display.print("2k PR: ");
    display.setFont(&FreeSans12pt7b);
    display.print(doc["pr_2k"].as<const char*>());

    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(leftColX, rowStart + rowHeight*2);
    display.print("5k PR: ");
    display.setFont(&FreeSans12pt7b);
    display.print(doc["pr_5k"].as<const char*>());

    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(leftColX, rowStart + rowHeight*3);
    display.print("10k PR: ");
    display.setFont(&FreeSans12pt7b);
    display.print(doc["pr_10k"].as<const char*>());

    // Right Column (Time PRs & Streaks)
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(rightColX, rowStart);
    display.print("30m PR: ");
    display.setFont(&FreeSans12pt7b);
    display.print(doc["pr_30m"].as<const char*>());

    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(rightColX, rowStart + rowHeight);
    display.print("1hr PR: ");
    display.setFont(&FreeSans12pt7b);
    display.print(doc["pr_1hr"].as<const char*>());

    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(rightColX, rowStart + rowHeight*2);
    display.print("Marathons: ");
    display.setFont(&FreeSans12pt7b);
    display.print(doc["marathons"].as<const char*>());

    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(rightColX, rowStart + rowHeight*3);
    display.print("Streak: ");
    display.setFont(&FreeSans12pt7b);
    String streakStr = doc["daily_streak"].as<String>() + " days";
    display.print(streakStr.c_str());

  } while (display.nextPage());

  Serial.println("Display update complete. Powering down E-Ink controller.");
  display.hibernate();
}

void go_to_sleep() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_32, 0);
  Serial.println("Entering deep sleep...");
  esp_deep_sleep_start();
}
