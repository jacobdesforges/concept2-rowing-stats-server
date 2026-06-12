#include "secrets.h"
#include <WiFi.h>
#include <HTTPClient.h>
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

// Adafruit doesn't contain pre-rendered 15pt fonts, retrieved from https://rop.nl/truetype2gfx/ and included in the repo
#include "FreeSansBold15pt7b.h" 

// --- WiFi Credentials ---
const char* ssid     = SECRET_SSID;
const char* password = SECRET_PASSWORD;

// --- Server Configuration ---
// The URL to trigger the Python script (using the webhook listener)
const char* triggerUrl = "http://" BACKEND_IP ":8080/hooks/update-stats";
// The URL to fetch the updated JSON file
const char* jsonUrl    = "http://" BACKEND_IP ":8081/rowing_stats.json";

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
  
  // Implemented safety exit for WiFi connection to prevent infinite loop
  int wifi_attempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_attempts < 30) {
    delay(500);
    Serial.print(".");
    wifi_attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi Failed. Sleeping.");
    go_to_sleep();
    return;
  }
  
  Serial.println("\nConnected");

  ArduinoOTA.setHostname("erg-fridge-display");
  ArduinoOTA.begin();

  if (is_double_click) {
    Serial.println("Entering maintenance mode (120s OTA window)");
    maintenance_start_time = millis();
  } else {
    Serial.println("Single click: Triggering update and fetching live stats...");
    triggerAndUpdateDisplay();
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

void triggerAndUpdateDisplay() {
  // 1. Trigger the Server-Side Script Update
  Serial.println("Triggering server-side Python update script...");
  { // <-- Scope block explicitly manages memory lifecycle
    WiFiClient clientHook;
    HTTPClient httpHook;
    httpHook.begin(clientHook, triggerUrl);
    
    // Explicitly bump the timeout to 25 seconds in case C2 API is bogged down
    httpHook.setTimeout(25000); 
    
    int triggerHttpCode = httpHook.GET();

    if (triggerHttpCode > 0) {
      Serial.printf("Trigger Request Status Code: %d\n", triggerHttpCode);
      if (triggerHttpCode == HTTP_CODE_OK) {
        // FORCE the ESP32 to wait for and read the text returned by the script before moving on
        String webhookResponse = httpHook.getString();
        Serial.println("Webhook execution completed. Output:");
        Serial.println(webhookResponse);
        
        if (webhookResponse.indexOf("[SUCCESS]") != -1) {
          Serial.println("Verified script success token in response.");
        }
      } else {
        Serial.println("Server responded, but update may have failed.");
      }
    } else {
      Serial.printf("Trigger Request Failed: %s\n", httpHook.errorToString(triggerHttpCode).c_str());
    }
    httpHook.end();
    clientHook.stop(); // Forcefully close the TCP connection to port 8080
  }

  // Allow a brief moment for any lingering Docker bind-mount micro-delays
  delay(200);

  // 2. Fetch the Live JSON File
  Serial.println("Fetching live rowing_stats.json...");
  String payload = "";
  
  { // <-- Fresh scope for a fresh socket
    WiFiClient clientJson;
    HTTPClient httpJson;
    httpJson.begin(clientJson, jsonUrl);
    
    int jsonHttpCode = httpJson.GET();

    if (jsonHttpCode == HTTP_CODE_OK) {
      payload = httpJson.getString();
      Serial.println("JSON Fetch Successful.");
    } else {
      Serial.printf("JSON Fetch Failed. HTTP Error: %d\n", jsonHttpCode);
      httpJson.end();
      clientJson.stop();
      return;
    }
    httpJson.end();
    clientJson.stop(); // Forcefully annihilate the TCP socket to port 8081
  }

  // 3. Parse the Live JSON Payload
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return;
  }

  // 4. Update the Display
  // Init SPI and Display
  SPI.begin(EPD_CLK, -1, EPD_DIN, EPD_CS);
  display.init(115200, true, 2, false); 
  display.setRotation(0); 

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
    
    // Values (Dynamic Font Sizing - Centered)
    const char* lifeStr = doc["lifetime"].as<const char*>();
    
    // Drop font size if string is 10 characters or longer (includes comma and 'm' so anything >= 10,000,000m)
    if (strlen(lifeStr) >= 10) {
      display.setFont(&FreeSansBold15pt7b);
    } else {
      display.setFont(&FreeSansBold18pt7b);
    }
    
    // Calculate bounds using whichever font was selected above
    display.getTextBounds(lifeStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(100 - (w / 2), 70);
    display.print(lifeStr);

    const char* seasonStr = doc["season"].as<const char*>();
    
    // Repeat dynamic sizing for the season side
    if (strlen(seasonStr) >= 10) {
      display.setFont(&FreeSansBold15pt7b);
    } else {
      display.setFont(&FreeSansBold18pt7b);
    }
    
    display.getTextBounds(seasonStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(300 - (w / 2), 70);
    display.print(seasonStr);
    
    // --- Grid Lines ---
    display.drawLine(0, 90, 400, 90, GxEPD_BLACK);
    display.drawLine(200, 0, 200, 300, GxEPD_BLACK);
    
    // --- Bottom Section: 2 Columns ---
    int leftColX = 8;
    int rightColX = 208;
    
    int rowStart = 135;
    int rowHeight = 45;

    // Left Column (Distance PRs)
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(leftColX, rowStart);
    display.print("1k PR:  ");
    display.setFont(&FreeSans12pt7b);
    display.print(doc["pr_1k"].as<const char*>());

    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(leftColX, rowStart + rowHeight);
    display.print("2k PR:  ");
    display.setFont(&FreeSans12pt7b);
    display.print(doc["pr_2k"].as<const char*>());

    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(leftColX, rowStart + rowHeight*2);
    display.print("5k PR:  "); //Extra space for better alignment
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
    
    // Extracted as int to accurately handle singular vs plural strings
    int streakVal = doc["daily_streak"].as<int>();
    String streakStr = String(streakVal) + (streakVal == 1 ? " day" : " days");
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
