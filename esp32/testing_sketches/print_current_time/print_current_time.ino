#include "secrets.h"
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_sleep.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include "time.h"

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

// --- Display Initialization ---
// Instantiate the 4.2" B/W display (400x300 resolution)
// Note Waveshare changed the underlying screen hardware in the V2.1/V2.2 revisions from the older GDEW042T2 controller to the GDEY042T81 controller
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> display(GxEPD2_420_GDEY042T81(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// --- State Variables ---
unsigned long maintenance_start_time = 0;
bool is_double_click = false;

// --- NTP Time Configuration ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -18000;      // GMT-5 (Eastern Time)
const int   daylightOffset_sec = 3600;   // +1 hour for Daylight Saving Time

void setup() {
  Serial.begin(115200);
  delay(100); 

  pinMode(buttonPin, INPUT_PULLUP);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    // Initial debounce
    delay(50);
    if (digitalRead(buttonPin) == LOW) {
      Serial.println("Button held down, ignoring");
      go_to_sleep();
      return;
    }

    // Wait up to 0.5 seconds for additional clicks
    unsigned long wait_start = millis();
    is_double_click = false;
    while (millis() - wait_start < 500) {
      if (digitalRead(buttonPin) == LOW) {
        delay(50); // Debounce
        if (digitalRead(buttonPin) == LOW) {
          is_double_click = true;
          // Wait for button release before exiting
          while (digitalRead(buttonPin) == LOW) {
            delay(10);
          }
          break; // Second click detected, exit loop
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
    // Power-on reset or other wake source
    Serial.println("Power-on reset detected");
    go_to_sleep();
    return;
  }

  // Connect to WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected");

  // Setup OTA
  ArduinoOTA.setHostname("erg-fridge-display");
  ArduinoOTA.begin();

  // Handle button logic
  if (is_double_click) {
    Serial.println("Entering maintenance mode (120s OTA window)");
    maintenance_start_time = millis();
  } else {
    Serial.println("Single click: Fetching time and updating display...");
    updateDisplayWithTime();
    go_to_sleep();
  }
}

void loop() {
  ArduinoOTA.handle();

  // Check if maintenance mode should end after 120 seconds
  if (is_double_click && millis() - maintenance_start_time > 120000) {
    Serial.println("Maintenance mode ended. Going to sleep.");
    go_to_sleep();
  }
}

void updateDisplayWithTime() {
  // Remap hardware SPI pins to match the Waveshare Driver board wiring
  SPI.begin(EPD_CLK, -1, EPD_DIN, EPD_CS);
  
  // Init display logic
  display.init(115200, true, 2, false); 
  display.setRotation(0); //0 is top of the display. 1,2,3 rotate clockwise in 90 degree increments
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);

  // Sync Network Time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  char timeStringBuff[50] = "Time Sync Failed";
  
  // Wait up to 5 seconds for the NTP packet to return
  int retry = 0;
  Serial.print("Waiting for NTP time sync");
  while (!getLocalTime(&timeinfo) && retry < 10) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  Serial.println();

  if(retry < 10){
    strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S", &timeinfo);
    Serial.print("Current NTP time fetched: ");
    Serial.println(timeStringBuff);
  } else {
    Serial.println("Failed to obtain time from NTP server after 5 seconds.");
  }

  // Write payload to E-Ink display via paging buffer
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    
    display.setCursor(10, 50);
    display.print("Connection Test Successful");
    
    display.setCursor(10, 90);
    display.print("ESP32 Awake Status: OK");

    display.setCursor(10, 130);
    display.print("Current Time:");
    
    display.setCursor(10, 160);
    display.print(timeStringBuff);
    
  } while (display.nextPage());

  Serial.println("Display update complete. Powering down E-Ink controller.");
  display.hibernate();
}

void go_to_sleep() {
  // Disable WiFi to save power
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Clear all wake sources to prevent accidental loops
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  // Configure button as wake source (wakes on LOW)
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_32, 0);
  
  Serial.println("Entering deep sleep...");
  esp_deep_sleep_start();
}
