#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <rom/rtc.h>
#include <Preferences.h>
#include <math.h>

#include "secrets.h"  // Dane dostępowe poza repozytorium -> skopiuj secrets.example.h jako secrets.h

// ==========================================
// 1. KONFIGURACJA SIECI I USŁUGI
// ==========================================
const char* primary_ssid     = SECRET_PRIMARY_SSID;
const char* primary_password = SECRET_PRIMARY_PASSWORD;

const char* backup_ssid      = SECRET_BACKUP_SSID;      // "" = failover wyłączony
const char* backup_password  = SECRET_BACKUP_PASSWORD;

const char* hc_uuid          = SECRET_HC_UUID;

#define TIME_TO_SLEEP 60
#define MICROSECONDS_TO_SECONDS 1000000ULL

#define LED_PIN   2
#define TOUCH_PIN 4  // GPIO 4 (T0 / D4) -> Dotyk wybudzający

// ==========================================
// 2. PAMIĘĆ RTC I FLASH NVS
// ==========================================
RTC_DATA_ATTR int sessionBootCount = 0;
RTC_DATA_ATTR int sessionSuccessfulPings = 0;
RTC_DATA_ATTR int sessionFailedPings = 0;
RTC_DATA_ATTR unsigned long cumulativeActiveTimeMs = 0;

Preferences prefs;
uint32_t lifetimePowerOutages = 0;

// ==========================================
// 3. EFEKTY ŚWIETLNE LED (PWM)
// ==========================================
void ledAviatorStrobe() {
  analogWrite(LED_PIN, 255); delay(15);
  analogWrite(LED_PIN, 0);   delay(25);
  analogWrite(LED_PIN, 255); delay(15);
  analogWrite(LED_PIN, 0);
}

void ledHeartbeatPulse() {
  for (int b = 0; b <= 180; b += 35) { analogWrite(LED_PIN, b); delay(2); }
  for (int b = 180; b >= 20; b -= 45) { analogWrite(LED_PIN, b); delay(2); }
  delay(15);
  for (int b = 20; b <= 255; b += 50) { analogWrite(LED_PIN, b); delay(2); }
  for (int b = 255; b >= 0; b -= 15) {
    analogWrite(LED_PIN, b);
    delay(5);
  }
  analogWrite(LED_PIN, 0);
}

void ledTacticalError() {
  for (int i = 0; i < 3; i++) {
    analogWrite(LED_PIN, 255); delay(40);
    analogWrite(LED_PIN, 0);   delay(40);
  }
}

// ==========================================
// 4. POMIARY SYSTEMOWE
// ==========================================
String getResetReasonName(RESET_REASON reason) {
  switch (reason) {
    case 1:  return "POWERON_RESET";
    case 3:  return "SW_RESET";
    case 5:  return "DEEPSLEEP_RESET";
    case 15: return "BROWNOUT_RESET";
    default: return "RESET_CODE_" + String(reason);
  }
}

unsigned long measureGatewayLatency(IPAddress gw) {
  unsigned long start = millis();
  WiFiClient tcp;
  tcp.setTimeout(350);
  if (tcp.connect(gw, 53) || tcp.connect(gw, 80)) {
    unsigned long lat = millis() - start;
    tcp.stop();
    return lat;
  }
  return 0;
}

void goToSleep() {
  analogWrite(LED_PIN, 0);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Dynamiczna adaptacja progu do aktualnego uziemienia i zasilacza
  int baseline = touchRead(TOUCH_PIN);
  int dynamicThreshold = (baseline > 35) ? (int)(baseline * 0.72) : 48;

  touchSleepWakeUpEnable(TOUCH_PIN, dynamicThreshold);  // API przyjmuje numer GPIO, nie kanał T0
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * MICROSECONDS_TO_SECONDS);

  Serial.printf("[SYSTEM] Deep Sleep (60s)... [Base: %d | Thresh: %d]\n", baseline, dynamicThreshold);
  Serial.flush();
  esp_deep_sleep_start();
}

// ==========================================
// 5. GŁÓWNY SETUP PROGRAMU
// ==========================================
void setup() {
  unsigned long executionStart = millis();
  Serial.begin(115200);

  sessionBootCount++;
  RESET_REASON rReason = rtc_get_reset_reason(0);
  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();

  prefs.begin("sentinel", false);
  lifetimePowerOutages = prefs.getUInt("pwr_cuts", 0);

  String triggerText = "TIMER (60s Auto)";

  if (wakeupCause == ESP_SLEEP_WAKEUP_TOUCHPAD) {
    ledAviatorStrobe();
    triggerText = "TOUCH (GPIO 4 Ping)";
  } else if (rReason == 1 || rReason == 15) {
    triggerText = "COLD_BOOT / POWER_ON";
    lifetimePowerOutages++;
    prefs.putUInt("pwr_cuts", lifetimePowerOutages);
  }

  // --- KROK 1: ŁĄCZENIE Z WI-FI Z PULSOWANIEM ---
  WiFi.mode(WIFI_STA);
  String activeSSID = primary_ssid;
  WiFi.begin(primary_ssid, primary_password);

  unsigned long wifiStart = millis();
  int breathStep = 0;

  while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart) < 8000) {
    int brightness = (int)((sin(breathStep * 0.15) + 1.0) * 110.0);
    analogWrite(LED_PIN, constrain(brightness, 0, 255));
    breathStep++;
    delay(20);
  }

  // Failover na sieć zapasową
  if (WiFi.status() != WL_CONNECTED && strlen(backup_ssid) > 0) {
    WiFi.disconnect();
    activeSSID = String(backup_ssid) + " (FAILOVER)";
    WiFi.begin(backup_ssid, backup_password);
    unsigned long backupStart = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - backupStart) < 8000) {
      int brightness = (int)((sin(breathStep * 0.15) + 1.0) * 110.0);
      analogWrite(LED_PIN, constrain(brightness, 0, 255));
      breathStep++;
      delay(20);
    }
  }

  analogWrite(LED_PIN, 0);
  unsigned long wifiDuration = millis() - wifiStart;

  if (WiFi.status() != WL_CONNECTED) {
    sessionFailedPings++;
    prefs.end();
    ledTacticalError();
    cumulativeActiveTimeMs += (millis() - executionStart);
    goToSleep();
  }

  // --- KROK 2: POMIARY TELEMETRYCZNE ---
  unsigned long gwLatency = measureGatewayLatency(WiFi.gatewayIP());

  unsigned long dnsStart = millis();
  IPAddress resolvedIP;
  bool dnsSuccess = WiFi.hostByName("hc-ping.com", resolvedIP);
  unsigned long dnsDuration = millis() - dnsStart;

  int rssi = WiFi.RSSI();
  int linkQuality = constrain(2 * (rssi + 100), 0, 100);
  float chipTemp = temperatureRead();

  float sessionSla = 100.0;
  if ((sessionSuccessfulPings + sessionFailedPings) > 0) {
    sessionSla = ((float)sessionSuccessfulPings / (float)(sessionSuccessfulPings + sessionFailedPings)) * 100.0;
  }

  // --- KROK 3: BUDOWA RAPORTU POST ---
  String payload = "Status: OK\n";
  payload += "Report Trigger: " + triggerText + "\n";
  payload += "Reset Reason: " + getResetReasonName(rReason) + "\n";
  payload += "Lifetime Power Cuts: " + String(lifetimePowerOutages) + "\n";
  payload += "Session Sent Pings: " + String(sessionSuccessfulPings + 1) + "\n";
  payload += "Session Boot Count: " + String(sessionBootCount) + "\n";
  payload += "Session SLA: " + String(sessionSla, 1) + " %\n";
  payload += "Active SSID: " + activeSSID + "\n";
  payload += "BSSID (AP MAC): " + WiFi.BSSIDstr() + "\n";
  payload += "Wi-Fi RSSI: " + String(rssi) + " dBm (Link Quality: " + String(linkQuality) + " %)\n";
  payload += "Wi-Fi Join Time: " + String(wifiDuration) + " ms\n";
  payload += "Assigned IP: " + WiFi.localIP().toString() + "\n";
  payload += "Gateway IP: " + WiFi.gatewayIP().toString() + "\n";
  payload += "LAN Gateway RTT: " + String(gwLatency > 0 ? String(gwLatency) + " ms" : "N/A") + "\n";
  payload += "DNS Resolve: " + String(dnsDuration) + " ms (" + resolvedIP.toString() + ")\n";
  payload += "CPU Clock: " + String(ESP.getCpuFreqMHz()) + " MHz\n";
  payload += "Free Heap: " + String(ESP.getFreeHeap()) + " bytes\n";
  payload += "Min Free Heap: " + String(ESP.getMinFreeHeap()) + " bytes\n";
  payload += "Chip Temp: " + String(chipTemp, 1) + " °C\n";
  payload += "Active Exec Time: " + String(millis() - executionStart) + " ms";

  // --- KROK 4: WYSYŁANIE HTTPS POST ---
  analogWrite(LED_PIN, 180);
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String finalUrl = "https://hc-ping.com/" + String(hc_uuid);
  http.begin(client, finalUrl);
  http.setTimeout(7000);
  http.addHeader("Content-Type", "text/plain");

  int httpCode = http.POST(payload);
  analogWrite(LED_PIN, 0);
  http.end();

  if (httpCode == 200) {
    sessionSuccessfulPings++;
    ledHeartbeatPulse();
  } else {
    sessionFailedPings++;
    ledTacticalError();
  }

  prefs.end();
  cumulativeActiveTimeMs += (millis() - executionStart);

  goToSleep();
}

void loop() {}