#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "display_bsp.h"
#include <Wire.h>
#include <WiFiMulti.h>
#include "time.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"
#include <DHT.h>
 
WiFiMulti wifiMulti;
 
// ---------------- TIME ----------------
const char* ntp_server = "pool.ntp.org";
const char* tz_berlin = "CET-1CEST,M3.5.0,M10.5.0/3";
 
// ---------------- DISPLAY ----------------
static const int W = 400;
static const int H = 300;
 
DisplayPort RlcdPort(12, 11, 5, 40, 41, W, H);
GFXcanvas1 canvas(W, H);

// ---------------- DARK MODE ----------------
bool DARK_MODE = true;
 
// ---------------- SENSOR ----------------
const int BAT_ADC_PIN = 4;
const int KEY_PIN = 18;

#define DHTPIN 2           
#define DHTTYPE DHT11       
DHT dht(DHTPIN, DHTTYPE);
 
// ---------------- WEATHER ----------------
int temp_outdoor = -999;
String condition = "NO WEATHER DATA";
int humidity_outdoor = -1;
String wind_str = "--";
int rain_chance = -1;
bool weather_ok = false;
int temp_max = -999;
int temp_min = -999;
float uv_index = -1.0f;
 
// ---------------- INDOOR ----------------
float t_indoor = 0;
float h_indoor = 0;
float v_bat = 0;
 
// ---------------- TIME STRINGS ----------------
char date_str[12] = "--.--.----";
char time_str[9]  = "--:--:--";
struct tm current_time;
 
// ---------------- TIMERS ----------------
unsigned long lastClockUpdate = 0;
unsigned long lastSensorUpdate = 0;
unsigned long lastWeatherUpdate = 0;
int current_line_width = -1;
int last_shown_minute = -1;    // -1 erzwingt ersten refresh
 
// ---------------- ANIMATION ----------------
static const char* CHARSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ%.-+/";
static const int   CHARSET_LEN = 41;

// ---------------- SLEEP CONFIG ----------------
const int SLEEP_HOUR         = 22;   
const int SLEEP_MIN          = 45;   
const int WAKEUP_HOUR        = 6;    
const int WAKEUP_MIN         = 30;   
const int STAY_AWAKE_SECONDS = 30;   

unsigned long nightWakeupTime = 0;
bool is_night_wakeup = false;

// RTC-Speicher Variablen (überleben den Deep Sleep)
RTC_DATA_ATTR bool was_sleeping = false;
RTC_DATA_ATTR int rtc_temp_outdoor = -999;
RTC_DATA_ATTR int rtc_humidity_outdoor = -1;
RTC_DATA_ATTR int rtc_temp_max = -999;
RTC_DATA_ATTR int rtc_temp_min = -999;
RTC_DATA_ATTR float rtc_uv_index = -1.0f;
RTC_DATA_ATTR char rtc_condition[20] = "NO DATA";
RTC_DATA_ATTR char rtc_wind_str[15] = "--";
 
// Forward declaration
void drawUI(bool full);
void updateWeather();
void updateLocalTime();
 
// ===================================================
float getBatteryVoltage() {
  int rawADC = analogRead(BAT_ADC_PIN); 
  float pinVoltage = (rawADC / 4095.0f) * 3.3f;
  float batteryVoltage = pinVoltage * 3.0f; 
  return batteryVoltage;
}

int getBatteryPercentage(float voltage) {
  if (voltage >= 4.15f) return 100;
  if (voltage <= 3.40f) return 0;

  if (voltage > 3.95f) {
    return (int)(80.0f + (voltage - 3.95f) / (4.15f - 3.95f) * 20.0f);
  } 
  else if (voltage > 3.70f) {
    return (int)(30.0f + (voltage - 3.70f) / (3.95f - 3.70f) * 50.0f);
  } 
  else {
    return (int)(0.0f + (voltage - 3.40f) / (3.70f - 3.40f) * 30.0f);
  }
}

uint64_t getSleepDurationToWakeup() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return 8 * 3600 * 1000000ULL; 
  }

  long current_secs = timeinfo.tm_hour * 3600 + timeinfo.tm_min * 60 + timeinfo.tm_sec;
  long target_secs = WAKEUP_HOUR * 3600 + WAKEUP_MIN * 60;

  long diff_secs = 0;

  if (current_secs >= target_secs) {
    diff_secs = (24 * 3600 - current_secs) + target_secs;
  } else {
    diff_secs = target_secs - current_secs;
  }

  return (uint64_t)diff_secs * 1000000ULL;
}
 
// ===================================================
void updateLocalTime(){
  if(getLocalTime(&current_time)){
    strftime(date_str,sizeof(date_str),"%d.%m.%y",&current_time);
    strftime(time_str,sizeof(time_str),"%H:%M:%S",&current_time);
  }
}
 
// ===================================================
void updateWeather(){
  if(WiFi.status() != WL_CONNECTED && wifiMulti.run() != WL_CONNECTED){
    weather_ok = false;
    wind_str = "--";
    return;
  }
 
  HTTPClient http;
  String url =
    "https://api.open-meteo.com/v1/forecast?latitude=52.52&longitude=13.41"
    "&current_weather=true"
    "&hourly=relative_humidity_2m,precipitation_probability"
    "&daily=temperature_2m_max,temperature_2m_min,uv_index_max"
    "&timezone=Europe/Berlin";
 
  http.begin(url);
 
  int code = http.GET();
  if(code != 200){
    weather_ok = false;
    wind_str = "--";
    http.end();
    return;
  }
 
  String payload = http.getString();
  http.end();
 
  JsonDocument doc;
  if(deserializeJson(doc, payload)){
    weather_ok = false;
    wind_str = "--";
    return;
  }
 
  temp_outdoor = doc["current_weather"]["temperature"];
  float wind = doc["current_weather"]["windspeed"];
  int wc = doc["current_weather"]["weathercode"];
 
  switch(wc){
  case 0: condition = "KLAR"; break; // "CLEAR"
  case 1:
  case 2: condition = "TEILS WOLKIG"; break; // "PARTLY CLOUDY"
  case 3: condition = "WOLKIG"; break; //"CLOUDY"
  
  // Nebel
  case 45:
  case 48: condition = "NEBEL"; break; //"FOG"
  
  // Nieselregen
  case 51:
  case 53:
  case 55: condition = "NIESELREGEN"; break; // "DRIZZLE"
  
  // Regen (leicht, mäßig, stark)
  case 61:
  case 63:
  case 65: condition = "REGEN"; break; // "RAIN"
  
  // Schnee / Schneegriesel
  case 71:
  case 73:
  case 75:
  case 77: condition = "SCHNEE"; break; // "SNOW"
  
  // Regenschauer
  case 80:
  case 81:
  case 82: condition = "REGENSCHAUER"; break; // "SHOWERS"
  
  // Gewitter
  case 95:
  case 96:
  case 99: condition = "GEWITTER"; break; // "THUNDERSTORM"
  
  default: condition = "UNKNOWN"; break;
  }
 
  // Aktuelle Stunde für den Array-Index ermitteln (0 - 23)
  int current_hour = current_time.tm_hour;
  if (current_hour < 0 || current_hour > 23) {
    current_hour = 0; // Sicherheits-Fallback, falls die Zeitstruktur fehlerhaft ist
  }

  // Stündliche Werte basierend auf der aktuellen Stunde auslesen statt fest auf Index [0]
  humidity_outdoor = doc["hourly"]["relative_humidity_2m"][current_hour];
  rain_chance = doc["hourly"]["precipitation_probability"][current_hour];
  
  uv_index = doc["daily"]["uv_index_max"][0];
  wind_str = String(wind, 1) + " km/h";
  temp_max = (int)doc["daily"]["temperature_2m_max"][0];
  temp_min = (int)doc["daily"]["temperature_2m_min"][0];
  weather_ok = true;

  // Im RTC Speicher sichern
  rtc_temp_outdoor = temp_outdoor;
  rtc_humidity_outdoor = humidity_outdoor;
  rtc_temp_max = temp_max;
  rtc_temp_min = temp_min;
  rtc_uv_index = uv_index;
  strncpy(rtc_condition, condition.c_str(), sizeof(rtc_condition));
  strncpy(rtc_wind_str, wind_str.c_str(), sizeof(rtc_wind_str));
}
 
// ===================================================
void pushCanvasToRLCD(bool invert = false){
  uint8_t *buf = canvas.getBuffer();
  int bpr = (W + 7) / 8;
 
  RlcdPort.RLCD_ColorClear(ColorWhite);
 
  for(int y = 0; y < H; y++){
    uint8_t *row = buf + y * bpr;
    for(int bx = 0; bx < bpr; bx++){
      uint8_t v = row[bx];
      if(invert) v ^= 0xFF;
 
      int x0 = bx * 8;
      for(int bit = 0; bit < 8; bit++){
        int x = x0 + bit;
        if(x >= W) break;
        if(v & (0x80 >> bit))
          RlcdPort.RLCD_SetPixel(x, y, ColorBlack);
      }
    }
  }
 
  RlcdPort.RLCD_Display();
}
 
// ===================================================
static void drawAnimOrFinal(int x, int y, uint8_t sz,
                             const char* finalStr,
                             int clearW, int clearH,
                             bool rattling) {
  canvas.fillRect(x, y, clearW, clearH, 0);
  canvas.setTextSize(sz);
  canvas.setTextColor(1);
  canvas.setCursor(x, y);
  int len = strlen(finalStr);
  if (rattling) {
    for (int i = 0; i < len; i++) {
      char c = finalStr[i];
      if (c == ' ' || c == '.' || c == ':' || c == '/' || c == '%') canvas.print(c);
      else canvas.print((char)CHARSET[random(0, CHARSET_LEN)]);
    }
  } else {
    canvas.print(finalStr);
  }
}
 
// ===================================================
void triggerStartAnimation() {
  const int FRAMES      = 12;
  const int FRAME_DELAY = 30;
 
  String tempStr = weather_ok
    ? (String(temp_outdoor < 0 ? "-" : "") + String(abs(temp_outdoor), DEC))
    : "--";
  String ssidStr = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "N/A";
  String condStr = weather_ok ? condition : "--";
 
  char minmax[16], hum_out[8], rain_buf[8], intemp[12], inhum[10], uvi[8], bat[8];
  if (weather_ok) snprintf(minmax, sizeof(minmax), "%d/%d C", temp_min, temp_max);
  else            snprintf(minmax, sizeof(minmax), "--");
  snprintf(hum_out,  sizeof(hum_out),  weather_ok ? "%d%%" : "--", humidity_outdoor);
  snprintf(rain_buf, sizeof(rain_buf), weather_ok ? "%d%%" : "--", rain_chance);
  snprintf(intemp,   sizeof(intemp),   "%.1fC", t_indoor);
  snprintf(inhum,    sizeof(inhum),    "%.1f%%", h_indoor);
  if (weather_ok) snprintf(uvi, sizeof(uvi), "%.1f", uv_index);
  else            snprintf(uvi, sizeof(uvi), "--");
  if      (v_bat < 3.0f)  snprintf(bat, sizeof(bat), "---");
  else if (v_bat > 4.15f) snprintf(bat, sizeof(bat), "CHG");
  else                    snprintf(bat, sizeof(bat), "%d%%", getBatteryPercentage(v_bat));
  String windDisp = weather_ok ? wind_str : "--";
 
  int gradOff = 6;
  if (weather_ok) {
    int einer = abs(temp_outdoor) % 10;
    if (einer == 1) gradOff = -12;
    else if (einer == 7) gradOff = 10;
  }
 
  for (int f = 0; f <= FRAMES; f++) {
    bool r = (f < FRAMES);
 
    canvas.fillScreen(0);
    canvas.setTextColor(1);
    canvas.fillRect(25, 140, W - 50, 1, 1);
 
    canvas.setTextSize(2);
    canvas.setCursor(25,  160); canvas.print("OUTDOOR");
    canvas.setCursor(25,  195); canvas.print("-/+:");
    canvas.setCursor(25,  220); canvas.print("HUM:");
    canvas.setCursor(25,  245); canvas.print("RAIN:");
    canvas.setCursor(25,  270); canvas.print("WIND:");
    canvas.setCursor(240, 160); canvas.print("INDOOR");
    canvas.setCursor(240, 195); canvas.print("TMP:");
    canvas.setCursor(240, 220); canvas.print("HUM:");
    canvas.setCursor(240, 245); canvas.print("UVI:");
    canvas.setCursor(240, 270); canvas.print("BAT:");
    canvas.setCursor(240,  55); canvas.print("BERLIN");
    canvas.setTextSize(1);
    canvas.setCursor(240, 20); canvas.print("WIFI:");
 
    drawAnimOrFinal(25,  20, 9, tempStr.c_str(),   160, 90, r);
    drawAnimOrFinal(282, 20, 1, ssidStr.c_str(),   118, 10, r);
    drawAnimOrFinal(240, 80, 2, date_str,           160, 18, r);
    drawAnimOrFinal(240,105, 2, time_str,           160, 18, r);
    drawAnimOrFinal(25, 105, 2, condStr.c_str(),    200, 18, r);
    drawAnimOrFinal(95, 195, 2, minmax,             150, 18, r);
    drawAnimOrFinal(95, 220, 2, hum_out,            100, 18, r);
    drawAnimOrFinal(95, 245, 2, rain_buf,           100, 18, r);
    drawAnimOrFinal(95, 270, 2, windDisp.c_str(),   150, 18, r);
    drawAnimOrFinal(300,195, 2, intemp,             100, 18, r);
    drawAnimOrFinal(300,220, 2, inhum,              100, 18, r);
    drawAnimOrFinal(300,245, 2, uvi,                 80, 18, r);
    drawAnimOrFinal(300,270, 2, bat,                 80, 18, r);
 
    {
      canvas.setTextSize(9);
      canvas.setCursor(25, 20);
      canvas.print(tempStr.c_str());
      int tex = canvas.getCursorX();
      canvas.fillCircle(tex + gradOff, 28, 7, 1);
      canvas.fillCircle(tex + gradOff, 28, 4, 0);
    }
 
    if (!r && weather_ok && uv_index >= 2.5f) {
      canvas.setTextSize(1);
      canvas.setCursor(336, 252);
      canvas.print("(SPF!)");
    }
 
    pushCanvasToRLCD(DARK_MODE);
    if (r) delay(FRAME_DELAY);
  }
 
  delay(200);
  drawUI(true);
}
 
void drawUI(bool full){
 
  if(full){
    canvas.fillScreen(0);
    canvas.setTextColor(1);
 
    // TEMP
    canvas.setTextSize(9);
    canvas.setCursor(25, 20);
    if(weather_ok) canvas.printf("%02d", temp_outdoor);
    else canvas.print("--");
 
    int temp_end_x = canvas.getCursorX();
 
    int offset = 6;
    if (weather_ok) {
      int einer = abs(temp_outdoor) % 10;
      if (einer == 1) offset = -12;
      else if (einer == 7) offset = 10;
      else offset = 6;
    } else {
      offset = 4;
    }
 
    int grad_x = temp_end_x + offset;
    canvas.fillCircle(grad_x, 28, 7, 1);
    canvas.fillCircle(grad_x, 28, 4, 0);
 
    // WIFI
    canvas.setTextSize(1);
    canvas.setCursor(240, 20);
    if(WiFi.status() == WL_CONNECTED){
      canvas.print("WIFI: ");
      canvas.print(WiFi.SSID());
    } else {
      canvas.print("NOT CONNECTED");
    }
 
    // CONDITION
    canvas.setTextSize(2);
    canvas.setCursor(25, 105);
    if(weather_ok) canvas.print(condition);
    else           canvas.print("--");
 
    // Trennlinie
    int max_width = W - 50;
    if (current_line_width >= 0) {
      int center_x = W / 2;
      int anim_x = center_x - (current_line_width / 2);
      canvas.fillRect(anim_x, 140, current_line_width, 1, 1);
    } else {
      canvas.fillRect(25, 140, max_width, 1, 1);
    }
 
    // OUTDOOR
    canvas.setTextSize(2);
    canvas.setCursor(25, 160); canvas.print("AUSSEN"); // "OUTDOOR"
 
    canvas.setCursor(25, 195); canvas.print("-/+:");
    canvas.setCursor(95, 195);
    if(weather_ok) {
      canvas.printf("%d/%d", temp_min, temp_max);
      int mcx = canvas.getCursorX();
      canvas.drawCircle(mcx + 3, 194, 2, 1);
      canvas.print(" C");
    } else {
      canvas.print("--");
    }
 
    canvas.setCursor(25, 220); canvas.print("HUM:");
    canvas.setCursor(95, 220);
    weather_ok ? canvas.printf("%d%%", humidity_outdoor) : canvas.print("--");
 
    canvas.setCursor(25, 245); canvas.print("RAIN:");
    canvas.setCursor(95, 245);
    weather_ok ? canvas.printf("%d%%", rain_chance) : canvas.print("--");
 
    canvas.setCursor(25, 270); canvas.print("WIND:");
    canvas.setCursor(95, 270);
    canvas.print(weather_ok ? wind_str : "--");
 
    // INDOOR
    canvas.setCursor(240, 160); canvas.print("INNEN"); // "INDOOR"
 
    canvas.setCursor(240, 195); canvas.print("TMP:");
    canvas.setCursor(300, 195);
    if (t_indoor == -99.0f) {
      canvas.print("--.-");
    } else {
      canvas.printf("%.1f", t_indoor);
      int cx = canvas.getCursorX();
      canvas.drawCircle(cx + 4, 196, 2, 1);
      canvas.setCursor(cx + 12, 195);
      canvas.print("C");
    }
 
    canvas.setCursor(240, 220); canvas.print("HUM:");
    canvas.setCursor(300, 220);
    if (h_indoor == -99.0f) {
      canvas.print("--.-");
    } else {
      canvas.printf("%.1f%%", h_indoor);
    }
 
    canvas.setCursor(240, 245); canvas.print("UVI:");
    canvas.setCursor(300, 245);
    if(weather_ok) {
      canvas.setTextSize(2);
      canvas.printf("%.1f", uv_index);
      if(uv_index >= 2.5f) {
        int next_x = canvas.getCursorX();
        canvas.setTextSize(1);
        canvas.setCursor(next_x + 6, 252);
        canvas.print("(SPF!)");
      }
      canvas.setTextSize(2);
    } else {
      canvas.print("--");
    }
 
    canvas.setCursor(240, 270); canvas.print("BAT:");
    canvas.setCursor(300, 270);
    if(v_bat < 3.0f)       canvas.print("---");
    else if(v_bat > 4.15f) canvas.print("CHG");
    else                   canvas.printf("%d%%", getBatteryPercentage(v_bat));
  }
 
  // CITY + DATUM + UHRZEIT
  canvas.fillRect(240, 50, 160, 80, 0);
  canvas.setTextSize(2);
  canvas.setCursor(240, 55); canvas.print("BERLIN");
  canvas.setCursor(240, 80); canvas.print(date_str);
  canvas.setCursor(240, 105); canvas.print(time_str);
 
  pushCanvasToRLCD(DARK_MODE);
}
 
// ===================================================
void triggerLineAnimation() {
  int max_width = W - 50;
  for (int w = 0; w <= max_width; w += 35) {
    current_line_width = w;
    drawUI(true);
    delay(10);
  }
  current_line_width = -1;
  drawUI(true);
}
 
// ===================================================
// ===================================================
void setup(){
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
 
  analogReadResolution(12);                   
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_ATTENDB_MAX); 

  Wire.begin(13,14);
  RlcdPort.RLCD_Init();
  pinMode(KEY_PIN, INPUT_PULLUP);
  
  dht.begin(); 
  
  // ZUERST LOKALE SENSOREN AUSLESEN
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  
  int retry = 0;
  while ((isnan(h) || isnan(t)) && retry < 5) {
    delay(50);
    h = dht.readHumidity();
    t = dht.readTemperature();
    retry++;
  }

  if (!isnan(h) && !isnan(t)) {
    t_indoor = t;
    h_indoor = h;
  } else {
    t_indoor = -99.0f; 
    h_indoor = -99.0f;
  }
  
  v_bat = getBatteryVoltage(); 

  // WAKEUP-GRUND PRÜFEN
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();


  wifiMulti.addAP(SECRET_SSID,   SECRET_PASSWORD);

  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    // Manueller Knopfdruck in der Nacht
    is_night_wakeup = true;
    nightWakeupTime = millis(); 

    // Zeitzone sofort laden, damit die Uhrzeit stimmt
    configTzTime(tz_berlin, ntp_server);
    updateLocalTime();
    
    // Wetterwerte explizit auf "nicht vorhanden" setzen, damit überall direkt Platzhalter stehen
    weather_ok = false;
    temp_outdoor = -999;
    condition = "--";
    
    // 1. SOFORT RENDERN (Uhrzeit stimmt, Wetter zeigt sofort --)
    drawUI(true); 

    // 2. WLAN gezielt und asynchron starten (ohne blockierendes Scannen)
    WiFi.mode(WIFI_STA);
    WiFi.begin(SECRET_SSID, SECRET_PASSWORD); 

    // 3. Direkt ab in den loop()! Keine blockierenden Schleifen im setup.
    lastWeatherUpdate = millis(); 
    return; 
  }

  // REGULÄRER START (Tagesbetrieb)
  while(wifiMulti.run() != WL_CONNECTED) delay(300);
 
  configTzTime(tz_berlin, ntp_server);
 
  struct tm t_time;
  while(!getLocalTime(&t_time)) delay(200);
 
  updateLocalTime();
  updateWeather();
 
  triggerStartAnimation();
  was_sleeping = false;
}
 
// ===================================================
void loop(){
  unsigned long now = millis();
 
  if(digitalRead(KEY_PIN)==LOW){
    DARK_MODE=!DARK_MODE;
    drawUI(true);
    delay(300);
  }
 
  if(now-lastClockUpdate>1000){
    lastClockUpdate=now;
    updateLocalTime();

    if(current_time.tm_min != last_shown_minute){
      last_shown_minute = current_time.tm_min;
      drawUI(false);
    }
  }
 
  if(now-lastSensorUpdate>30000){ 
    lastSensorUpdate=now;
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h) && !isnan(t)) {
      t_indoor = t;
      h_indoor = h;
    } else {
      t_indoor = -99.0f;
      h_indoor = -99.0f;
    }
    v_bat = getBatteryVoltage();
    drawUI(true);
  }
 
  // Wetter-Update Logik
  if (is_night_wakeup) {
    // Nachts: Nach 5 Sekunden im Hintergrund einmal frische Daten probieren
    if (lastWeatherUpdate != 0 && (now - nightWakeupTime > 5000)) {
      lastWeatherUpdate = 0; // Nur einmal ausführen
      
      // Nur abfragen, wenn das WLAN bis dahin wirklich steht
      if (WiFi.status() == WL_CONNECTED) {
        updateWeather();       
        drawUI(true);          
      }
    }
  } else {
    // Normaler Tagesbetrieb (alle 15 Minuten)
    if (now - lastWeatherUpdate > 900000) {
      lastWeatherUpdate = now;
      updateWeather();
      triggerLineAnimation();
    }
  }

  if (is_night_wakeup && (now - nightWakeupTime > (STAY_AWAKE_SECONDS * 1000))) {
    Serial.println("Kurz-Wachphase vorbei. Gehe zurück in Deep Sleep...");
    
    canvas.fillScreen(0);
    pushCanvasToRLCD(DARK_MODE);

    was_sleeping = true;
    esp_sleep_enable_timer_wakeup(getSleepDurationToWakeup());
    esp_sleep_enable_ext0_wakeup((gpio_num_t)KEY_PIN, 0); 
    esp_deep_sleep_start();
  }
  
  if (!is_night_wakeup && getLocalTime(&current_time)) {
    if (current_time.tm_hour == SLEEP_HOUR && current_time.tm_min == SLEEP_MIN) {
      is_night_wakeup = true;
      nightWakeupTime = now; 
    }
  }
}