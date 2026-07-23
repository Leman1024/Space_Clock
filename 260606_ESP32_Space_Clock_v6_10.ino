/*
 * =====================================================================================
 * Project:      esp32_space_clock_v6.10
 * Description:  Horloge Radar Spatiale - Comète Particulaire Organique & Syzygie Réelle
 * Target:       ESP32-C3 (Super Mini) | Écran GC9A01 | LovyanGFX
 * =====================================================================================
 */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <esp_random.h>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel_instance;
  lgfx::Bus_SPI      _bus_instance;
public:
  LGFX(void) {
    auto cfg = _bus_instance.config();
    cfg.spi_host = SPI2_HOST; 
    cfg.spi_mode = 0; 
    cfg.freq_write = 27000000; 
    cfg.pin_sclk = 0; // SLC
    cfg.pin_mosi = 1; // SDA
    cfg.pin_miso = -1;
    cfg.pin_dc = 2;  // DC
    cfg.dma_channel = SPI_DMA_CH_AUTO; 
    _bus_instance.config(cfg);
    _panel_instance.setBus(&_bus_instance);
    
    auto pcfg = _panel_instance.config();
    pcfg.pin_cs = 3; // CS
    pcfg.pin_rst = 4; // RST
    pcfg.invert = true;
    pcfg.panel_width = 240; 
    pcfg.panel_height = 240;
    _panel_instance.config(pcfg);
    setPanel(&_panel_instance);
  }
};

LGFX tft;                 
LGFX_Sprite canvas(&tft);

WebServer webServer(80);
DNSServer dnsServer;
Preferences preferences;

String wifi_ssid     = "";
String wifi_password = "";
String time_zone     = "CET-1CEST,M3.5.0,M10.5.0/3"; 
const char* ntpServer = "pool.ntp.org";
int lastSyncedHour = -1;

const int CENTER_X = 120, CENTER_Y = 120;
const int DIST_EARTH = 60, DIST_MARS = 90; 
const int RAD_EARTH = 9, RAD_MARS = 8;

unsigned long lastFrameTime = 0;
const unsigned long frameDuration = 100;

time_t last_tv_sec = 0;
int cached_h = 0, cached_m = 0, cached_s = 0;
int last_calculated_second = -1;
float last_second_angle = 0.0f;
bool first_frame = true;

const int NUM_STARS = 25;
struct Star { int x, y; uint8_t size; uint16_t color; };
Star stars[NUM_STARS];
bool hourlyStarsReset = false; 

struct RadarTarget { 
  int x = 0, y = 0; 
  float echoAge = 999.0f, textAge = 999.0f;
  int textOffsetX = 0, textOffsetY = 0; 
};
RadarTarget targetEarth, targetMars;

// Simplification des variables comète (vecteurs retirés)
bool cometActive = false;
float cStartX = 0.0f, cStartY = 0.0f, cEndX = 0.0f, cEndY = 0.0f;

void drawSetupScreen(String msg1, String msg2, String msg3, String msg4) {
  canvas.fillScreen(0);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(0x07E0); 
  canvas.drawString(msg1, CENTER_X, 50);
  canvas.drawString(msg2, CENTER_X, 90);
  canvas.setTextColor(0xFFFF); 
  canvas.drawString(msg3, CENTER_X, 130);
  canvas.setTextColor(0x07E0);
  canvas.drawString(msg4, CENTER_X, 180);
  canvas.pushSprite(0, 0);
}

void handlePortalSave() {
  String req_ssid = webServer.arg("ssid");
  String req_pass = webServer.arg("pass");
  String req_tz   = webServer.arg("tz");
  if (req_ssid.length() > 0) {
    preferences.begin("space_clock", false);
    preferences.putString("ssid", req_ssid);
    preferences.putString("pass", req_pass);
    preferences.putString("tz", req_tz);
    preferences.end();
    String html = "<html><head><meta charset='UTF-8'></head><body style='font-family:sans-serif; text-align:center; background:#000; color:#0f0; padding-top:50px;'>";
    html += "<h2>Configuration sauvegardee !</h2><p>L'horloge redemarre...</p></body></html>";
    webServer.send(200, "text/html", html);
    drawSetupScreen("Sauvegarde reussie", "Redemarrage systeme", "Initialisation...", "");
    delay(2000);
    ESP.restart();
  } else {
    webServer.send(400, "text/plain", "SSID Manquant");
  }
}

void handlePortalRoot() {
  int n = WiFi.scanNetworks();
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'><meta charset='UTF-8'>";
  html += "<style>body{background:#0a0a16; color:#e0e0ff; font-family:sans-serif; padding:20px;} ";
  html += "h2{color:#00ffcc; text-align:center;} .card{background:#12122c; padding:20px; border-radius:10px; border:1px solid #232356;} ";
  html += "label{display:block; margin:15px 0 5px;} input, select{width:100%; padding:10px; background:#1a1a3a; color:#fff; border:1px solid #3b3b78; border-radius:5px;} ";
  html += "input[type=submit]{background:#00ffcc; color:#000; font-weight:bold; cursor:pointer; margin-top:20px; border:none;} input[type=submit]:hover{background:#00ccaa;}</style></head>";
  html += "<body><h2>🚀 SpaceClock Setup</h2><div class='card'><form action='/save' method='POST'>";
  html += "<label>Réseau WiFi détecté :</label><select name='ssid'>";
  if (n == 0) {
    html += "<option value=''>Aucun réseau trouvé</option>";
  } else {
    for (int i = 0; i < n; ++i) {
      html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
  }
  html += "</select>";
  html += "<label>Mot de passe :</label><input type='password' name='pass' placeholder='Clé WPA/WEP'>";
  html += "<label>Fuseau horaire :</label><select name='tz'>";
  html += "<option value='CET-1CEST,M3.5.0,M10.5.0/3'>Europe / Paris</option>";
  html += "<option value='GMT0BST,M3.5.0/1,M10.5.0'>Europe / London</option>";
  html += "<option value='EST5EDT,M3.2.0,M11.1.0'>US / Eastern (New York)</option>";
  html += "<option value='PST8PDT,M3.2.0,M11.1.0'>US / Pacific (Los Angeles)</option>";
  html += "<option value='AEST-10AEDT,M10.1.0,M4.1.0/3'>Australia / Sydney</option>";
  html += "<option value='UTC0'>Universal Coordinated Time (UTC)</option>";
  html += "</select>";
  html += "<input type='submit' value='SAUVEGARDER & CONFIGURER'>";
  html += "</form></div></body></html>";
  webServer.send(200, "text/html", html);
}

void runCaptivePortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("SpaceClock_Setup");
  dnsServer.start(53, "*", WiFi.softAPIP());
  webServer.on("/", handlePortalRoot);
  webServer.on("/save", handlePortalSave);
  webServer.onNotFound([]() {
    webServer.sendHeader("Location", "http://192.168.4.1/", true);
    webServer.send(302, "text/plain", "");
  });
  webServer.begin();
  drawSetupScreen("Mode configuration", "AP : SpaceClock_Setup", "IP : 192.168.4.1", "En attente...");
  while (true) {
    dnsServer.processNextRequest();
    webServer.handleClient();
    delay(10);
  }
}

void syncTimeNTP() {
  drawSetupScreen("Connexion Wi-Fi", wifi_ssid, "Recherche de signal...", "");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 30) { delay(500); timeout++; }
  if (WiFi.status() != WL_CONNECTED) { runCaptivePortal(); }
  
  drawSetupScreen("Synchronisation NTP", "Serveur : pool.ntp.org", "Recuperation...", "");
  configTzTime(time_zone.c_str(), ntpServer);
  struct tm timeinfo;
  int sync_timeout = 0;
  while (!getLocalTime(&timeinfo, 1000) && sync_timeout < 15) { sync_timeout++; }
  WiFi.disconnect(true); 
  WiFi.mode(WIFI_OFF);
}

inline uint16_t blendColorBlack(uint16_t fg, uint8_t a) {
  if (a == 0) return 0x0000;
  if (a == 255) return fg;
  uint32_t r = (((fg >> 11) & 0x1F) * a) >> 8;
  uint32_t g = (((fg >> 5) & 0x3F) * a) >> 8;
  uint32_t b = ((fg & 0x1F) * a) >> 8;
  return (r << 11) | (g << 5) | b;
}

void calculateSmartOffset(int tx, int ty, int otherX, int otherY, int &outX, int &outY) {
  int offsets[8][2] = {{0,-26}, {0,26}, {38,0}, {-38,0}, {26,-20}, {-26,-20}, {26,20}, {-26,20}};
  int best_idx = 0; 
  int best_score = -999999;
  for (int i = 0; i < 8; i++) {
    int cx = tx + offsets[i][0], cy = ty + offsets[i][1];
    int dx_c = cx - CENTER_X, dy_c = cy - CENTER_Y;
    int dx_o = cx - otherX, dy_o = cy - otherY;
    int d_center_sq = (dx_c * dx_c) + (dy_c * dy_c);
    int d_other_sq = (dx_o * dx_o) + (dy_o * dy_o);
    int score = 0;
    if (d_center_sq > 9000) score -= d_center_sq * 10; 
    if (d_center_sq < 2500) score -= 5000;
    if (d_other_sq < 2500) score -= 5000;
    score += d_other_sq / 10; 
    if (score > best_score) { best_score = score; best_idx = i; }
  }
  outX = offsets[best_idx][0]; outY = offsets[best_idx][1];
}

void drawLineAlpha(int x1, int y1, int x2, int y2, uint16_t color, uint8_t alpha) { 
  canvas.drawLine(x1, y1, x2, y2, blendColorBlack(color, alpha));
}

void fillCircleAlpha(int x, int y, int r, uint16_t color, uint8_t alpha) { 
  canvas.fillCircle(x, y, r, blendColorBlack(color, alpha));
}

void drawMars(int cx, int cy, int radius, uint8_t alpha) {
  canvas.drawCircle(cx, cy, radius + 1, blendColorBlack(0xD3A4, alpha)); 
  fillCircleAlpha(cx, cy, radius, 0xC240, alpha);                      
  if (alpha > 50) { 
    canvas.drawFastHLine(cx - 2, cy - radius + 1, 5, blendColorBlack(0xFFFF, alpha));
    canvas.fillTriangle(cx - 3, cy - 2, cx + 1, cy - 4, cx - 1, cy + 2, blendColorBlack(0x5800, alpha)); 
    canvas.fillTriangle(cx + 1, cy + 1, cx + 4, cy + 3, cx, cy + 4, blendColorBlack(0x5800, alpha));
  }
}

void drawEarth(int cx, int cy, int radius, uint8_t alpha) {
  canvas.drawCircle(cx, cy, radius + 2, blendColorBlack(0x1330, alpha));
  canvas.drawCircle(cx, cy, radius + 1, blendColorBlack(0x04D5, alpha));
  fillCircleAlpha(cx, cy, radius, 0x000F, alpha);
  if (alpha > 50) {
    canvas.fillTriangle(cx - 2, cy - radius + 1, cx + 2, cy - radius + 1, cx, cy - radius, blendColorBlack(0xFFFF, alpha));
    canvas.fillTriangle(cx - 2, cy + radius - 1, cx + 2, cy + radius - 1, cx, cy + radius, blendColorBlack(0xFFFF, alpha));
    canvas.fillTriangle(cx - 4, cy - 5, cx + 2, cy - 4, cx - 1, cy, blendColorBlack(0x05A0, alpha));
    canvas.fillTriangle(cx - 1, cy, cx + 4, cy + 2, cx + 1, cy + 6, blendColorBlack(0x05A0, alpha));
  }
}

void drawSpaceship(int cx, int cy, uint16_t color, unsigned long current_ms, uint8_t alpha, bool engineOn, bool alarmOn) {
  #define SX(val) (cx + (val))
  #define SY(val) (cy + (val))

  canvas.fillTriangle(SX(0), SY(-22), SX(-12), SY(18), SX(12), SY(18), 0x0000);

  drawLineAlpha(SX(0), SY(-24), SX(-6), SY(-10), color, alpha);
  drawLineAlpha(SX(0), SY(-24), SX(6), SY(-10), color, alpha);
  drawLineAlpha(SX(-6), SY(-10), SX(-8), SY(6), color, alpha); 
  drawLineAlpha(SX(6), SY(-10), SX(8), SY(6), color, alpha);
  drawLineAlpha(SX(0), SY(-18), SX(-4), SY(-8), color, alpha); 
  drawLineAlpha(SX(0), SY(-18), SX(4), SY(-8), color, alpha);
  drawLineAlpha(SX(-4), SY(-8), SX(4), SY(-8), color, alpha);
  drawLineAlpha(SX(-8), SY(-2), SX(-22), SY(12), color, alpha);
  drawLineAlpha(SX(-22), SY(12), SX(-10), SY(12), color, alpha); 
  drawLineAlpha(SX(8), SY(-2), SX(22), SY(12), color, alpha);
  drawLineAlpha(SX(22), SY(12), SX(10), SY(12), color, alpha); 
  drawLineAlpha(SX(-10), SY(12), SX(-12), SY(18), color, alpha);
  drawLineAlpha(SX(-12), SY(18), SX(-6), SY(16), color, alpha);
  drawLineAlpha(SX(10), SY(12), SX(12), SY(18), color, alpha);
  drawLineAlpha(SX(12), SY(18), SX(6), SY(16), color, alpha); 
  drawLineAlpha(SX(-6), SY(16), SX(6), SY(16), color, alpha);
  drawLineAlpha(SX(-4), SY(16), SX(-5), SY(20), color, alpha); 
  drawLineAlpha(SX(-5), SY(20), SX(-1), SY(20), color, alpha);
  drawLineAlpha(SX(-1), SY(20), SX(0), SY(16), color, alpha);
  drawLineAlpha(SX(4), SY(16), SX(5), SY(20), color, alpha);
  drawLineAlpha(SX(5), SY(20), SX(1), SY(20), color, alpha); 
  drawLineAlpha(SX(1), SY(20), SX(0), SY(16), color, alpha);
  drawLineAlpha(SX(0), SY(-8), SX(0), SY(16), color, alpha);
  
  if (alpha > 127) { 
    bool blink = alarmOn ? ((current_ms / 100) % 2 == 0) : ((current_ms / 500) % 2 == 0);
    if (alarmOn) {
        if(blink) { fillCircleAlpha(SX(-22), SY(12), 2, 0xF800, alpha); fillCircleAlpha(SX(22), SY(12), 2, 0xF800, alpha); }
    } else {
        if (blink) fillCircleAlpha(SX(-22), SY(12), 2, 0xF800, alpha);
        else fillCircleAlpha(SX(22), SY(12), 2, 0x07E0, alpha);
    }
  }
  
  if (engineOn && alpha > 50) {
    int flick = (current_ms / 30) % 3;
    canvas.fillTriangle(SX(-4), SY(16), SX(4), SY(16), SX(0), SY(26 + flick*5), blendColorBlack(0xFC00, alpha));
    canvas.fillTriangle(SX(-2), SY(16), SX(2), SY(16), SX(0), SY(20 + flick*3), blendColorBlack(0xFFE0, alpha));
  }
  #undef SX
  #undef SY
}

void initStars() {
  uint16_t palette[] = {0xFFFF, 0x8DF1, 0xFFE0, 0x07FF};
  for (int i = 0; i < NUM_STARS; i++) {
    bool valid = false;
    while (!valid) {
      int px = esp_random() % 240, py = esp_random() % 240;
      float dx = px - CENTER_X, dy = py - CENTER_Y;
      if ((dx * dx) + (dy * dy) < 13500.0f) { 
        stars[i].x = px;
        stars[i].y = py;
        stars[i].size = (esp_random() % 2) + 1; 
        stars[i].color = palette[esp_random() % 4];
        valid = true;
      }
    }
  }
}

void setup() {
  btStop(); 
  pinMode(8, INPUT);
  tft.init(); 
  tft.setRotation(0); 
  canvas.setColorDepth(16);
  if (canvas.createSprite(240, 240) == nullptr) { while (1) delay(1000); }
  
  initStars();
  
  preferences.begin("space_clock", true);
  wifi_ssid     = preferences.getString("ssid", "");
  wifi_password = preferences.getString("pass", "");
  time_zone     = preferences.getString("tz", "CET-1CEST,M3.5.0,M10.5.0/3");
  preferences.end();
  
  if (wifi_ssid.length() == 0) { runCaptivePortal(); }
  syncTimeNTP(); 
  
  struct tm timeinfo_boot;
  if (getLocalTime(&timeinfo_boot)) { lastSyncedHour = timeinfo_boot.tm_hour; }
}

void loop() {
  unsigned long current_ms = millis();
  if (current_ms - lastFrameTime < frameDuration) { delay(1); return; }
  
  float delta = (current_ms - lastFrameTime) * 0.001f;
  lastFrameTime = current_ms;
  
  struct timeval tv; 
  gettimeofday(&tv, NULL);
  if (tv.tv_sec != last_tv_sec) {
    struct tm timeinfo;
    localtime_r(&tv.tv_sec, &timeinfo);
    cached_h = timeinfo.tm_hour;
    cached_m = timeinfo.tm_min;
    cached_s = timeinfo.tm_sec;
    last_tv_sec = tv.tv_sec;
  }
  
  int h = cached_h, m = cached_m, s = cached_s;
  float exact_seconds = s + (tv.tv_usec * 0.000001f);

  if (m == 25 && s == 30 && lastSyncedHour != h) {
    syncTimeNTP();
    struct tm timeinfo_after;
    if (getLocalTime(&timeinfo_after)) { lastSyncedHour = timeinfo_after.tm_hour; } 
    else { lastSyncedHour = h; }
    lastFrameTime = millis(); 
    return; 
  }

  uint8_t radarAlpha = 255;
  uint8_t spheresAlpha = 255;
  uint8_t shipAlpha = 255;
  
  float pWarp = 0.0f;
  int shipX = CENTER_X, shipY = CENTER_Y;
  bool engineOn = false, alarmOn = false, showStars = true;
  bool isSyzygyActive = false, isNormalPingActive = false;

  bool isHourEvent = (m == 0);
  
  // CALCUL SYZYGIE REELLE
  float angleEarth = (h % 12) * 30.0f + m * 0.5f; 
  float angleMars  = m * 6.0f;                    
  float diffAngle  = fabs(angleEarth - angleMars);
  while (diffAngle >= 360.0f) diffAngle -= 360.0f;
  
  bool isPlanetsAligned = (diffAngle < 6.0f) || (fabs(diffAngle - 180.0f) < 6.0f);
  bool isSyzygyEvent = isPlanetsAligned && (!isHourEvent); 

  float p_central = 0.0f;

  if (exact_seconds >= 56.0f) {
    float p = (exact_seconds - 56.0f) * 0.25f; 
    shipAlpha = (uint8_t)((1.0f - p) * 255);
    shipY = CENTER_Y - (int)(150.0f * p);
    engineOn = true;
    alarmOn = true; 
  } 
  else if (exact_seconds < 4.0f) {
    p_central = exact_seconds * 0.25f;
    shipAlpha = 0; 

    if (isSyzygyEvent) {
      isSyzygyActive = true;
    }

    if (isHourEvent) {
      if (exact_seconds < 0.5f) {
        spheresAlpha = (uint8_t)((1.0f - (exact_seconds * 2.0f)) * 255);
        radarAlpha = 255; 
      } 
      else if (exact_seconds < 1.0f) {
        spheresAlpha = 0;
        radarAlpha = (uint8_t)((1.0f - ((exact_seconds - 0.5f) * 2.0f)) * 255);
      } 
      else if (exact_seconds < 2.0f) {
        spheresAlpha = 0;
        radarAlpha = 0;
        pWarp = (exact_seconds - 1.0f); 
      } 
      else if (exact_seconds < 2.5f) {
        spheresAlpha = 0;
        radarAlpha = 0;
        showStars = false;
        if (!hourlyStarsReset) {
            initStars();
            hourlyStarsReset = true;
        }
      } 
      else {
        float p_in = (exact_seconds - 2.5f) / 1.5f; 
        spheresAlpha = (uint8_t)(p_in * 255);
        radarAlpha = (uint8_t)(p_in * 255);
        showStars = true;
      }
    } 
    else if (!isSyzygyEvent) {
      isNormalPingActive = true;
    }
  }
  else if (exact_seconds < 8.0f) {
    float p = (exact_seconds - 4.0f) * 0.25f;
    shipAlpha = (uint8_t)(p * 255);
    shipY = CENTER_Y + 150 - (int)(150.0f * p);
    engineOn = true;
    alarmOn = false; 
  }

  if (m == 59) { hourlyStarsReset = false; }

  float cometStartTime = 8.0f + ((m * 17) % 30); 
  bool isCometTime = (m != 25) && (exact_seconds >= cometStartTime && exact_seconds < cometStartTime + 15.0f);
  if (m == 25 && exact_seconds >= 20.0f) { cometActive = false; } 
  
  if (isCometTime) {
    if (!cometActive) {
      bool valid = false; int attempts = 0;
      while (!valid && attempts < 20) {
        float angle1 = (esp_random() % 360) * DEG_TO_RAD;
        float angle2 = angle1 + PI + ((esp_random() % 90) - 45) * DEG_TO_RAD;
        cStartX = CENTER_X + 160 * cos(angle1); 
        cStartY = CENTER_Y + 160 * sin(angle1);
        cEndX = CENTER_X + 160 * cos(angle2); 
        cEndY = CENTER_Y + 160 * sin(angle2);
        valid = true;
        for (float step = 0.2f; step <= 0.8f; step += 0.2f) {
           float px = cStartX + (cEndX - cStartX) * step; float py = cStartY + (cEndY - cStartY) * step;
           float dCenter = (px - CENTER_X)*(px - CENTER_X) + (py - CENTER_Y)*(py - CENTER_Y);
           float dMars = (px - targetEarth.x)*(px - targetEarth.x) + (py - targetEarth.y)*(py - targetEarth.y);
           float dEarth = (px - targetMars.x)*(px - targetMars.x) + (py - targetMars.y)*(py - targetMars.y);
           
           if (dCenter < 625.0f || dMars < 225.0f || dEarth < 225.0f) { valid = false; break; }
        }
        attempts++;
      }
      // Vecteurs cFwd et cRight retirés (plus besoin pour les sphères)
      cometActive = true;
    }
  } else { cometActive = false; }

  if (spheresAlpha > 0) {
    if (s != last_calculated_second) {
      last_calculated_second = s;
      float radE = ((h % 12) * 30.0f + (m * 0.5f) - 90.0f) * DEG_TO_RAD;
      targetEarth.x = CENTER_X + (int)(DIST_EARTH * cos(radE));
      targetEarth.y = CENTER_Y + (int)(DIST_EARTH * sin(radE));
      
      float radM = (m * 6.0f + (s * 0.1f) - 90.0f) * DEG_TO_RAD;
      targetMars.x = CENTER_X + (int)(DIST_MARS * cos(radM)); 
      targetMars.y = CENTER_Y + (int)(DIST_MARS * sin(radM));
      
      calculateSmartOffset(targetEarth.x, targetEarth.y, targetMars.x, targetMars.y, targetEarth.textOffsetX, targetEarth.textOffsetY); 
      calculateSmartOffset(targetMars.x, targetMars.y, targetEarth.x, targetEarth.y, targetMars.textOffsetX, targetMars.textOffsetY); 
    }
    
    float current_second_angle = exact_seconds * 6.0f;
    if (first_frame) { last_second_angle = current_second_angle; first_frame = false; }
    
    float angle_S_prev = last_second_angle; float angle_S_curr = current_second_angle;
    if (angle_S_prev > 300.0f && angle_S_curr < 60.0f) angle_S_curr += 360.0f;
    float chk_H = (h % 12 + m * 0.0166667f) * 30.0f; if (chk_H < 60.0f && angle_S_prev > 300.0f) chk_H += 360.0f;
    float chk_M = (m + s * 0.0166667f) * 6.0f; if (chk_M < 60.0f && angle_S_prev > 300.0f) chk_M += 360.0f;
    
    if (angle_S_prev <= chk_H && angle_S_curr >= chk_H) { targetEarth.echoAge = 0; targetEarth.textAge = 0; }
    if (angle_S_prev <= chk_M && angle_S_curr >= chk_M) { targetMars.echoAge = 0; targetMars.textAge = 0; }
    last_second_angle = current_second_angle;
  }

  canvas.fillScreen(0);
  
  if (showStars) {
    for (int i = 0; i < NUM_STARS; i++) {
      if (pWarp > 0.0f) {
        int dx = stars[i].x - CENTER_X, dy = stars[i].y - CENTER_Y;
        int x1 = stars[i].x + (int)(dx * pWarp * pWarp * 2.0f); 
        int y1 = stars[i].y + (int)(dy * pWarp * pWarp * 2.0f);
        int x2 = stars[i].x + (int)(dx * pWarp * 6.0f); 
        int y2 = stars[i].y + (int)(dy * pWarp * 6.0f);
        canvas.drawLine(x1, y1, x2, y2, stars[i].color);
      } else {
        if (stars[i].size == 1) canvas.drawPixel(stars[i].x, stars[i].y, stars[i].color);
        else canvas.fillRect(stars[i].x, stars[i].y, 2, 2, stars[i].color);
      }
    }
  }
    
  if (radarAlpha > 0) {
    uint16_t c_grid = blendColorBlack(0x01B2, radarAlpha);
    int jitterX = 0, jitterY = 0;
    if (isSyzygyActive) {
      jitterX = (esp_random() % 5) - 2; 
      jitterY = (esp_random() % 5) - 2; 
    }
    canvas.drawCircle(CENTER_X + jitterX, CENTER_Y + jitterY, 116, c_grid); 
    canvas.drawCircle(CENTER_X + jitterX, CENTER_Y + jitterY, 90, c_grid);
    canvas.drawCircle(CENTER_X + jitterX, CENTER_Y + jitterY, 60, c_grid); 
    canvas.drawCircle(CENTER_X + jitterX, CENTER_Y + jitterY, 30, c_grid);
    canvas.drawFastHLine(10 + jitterX, CENTER_Y + jitterY, 220, c_grid);
    canvas.drawFastVLine(CENTER_X + jitterX, 10 + jitterY, 220, c_grid);
  }

  if (isNormalPingActive) {
    int wave_rad = (int)(p_central * 120.0f);
    uint8_t wave_a = (uint8_t)((1.0f - p_central) * 255);
    canvas.drawCircle(CENTER_X, CENTER_Y, wave_rad, blendColorBlack(0x07E0, wave_a));
  }

  if (isSyzygyActive) {
    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(middle_center);
    int txtGlitchX = (esp_random() % 3) - 1;
    int txtGlitchY = (esp_random() % 3) - 1;
    canvas.setTextColor(blendColorBlack(0xFC00, radarAlpha));
    canvas.drawString("SYZYGY DETECTED", CENTER_X + txtGlitchX, CENTER_Y + txtGlitchY);
    canvas.setTextColor(blendColorBlack(0xF800, radarAlpha));
    canvas.drawString("SYZYGY DETECTED", CENTER_X, CENTER_Y);
  }
    
  if (spheresAlpha > 0) {
    targetEarth.echoAge += delta; targetMars.echoAge += delta;
    if (targetEarth.echoAge < 3.0f) {
        uint8_t a = (uint8_t)((1.0f - targetEarth.echoAge * 0.333333f) * spheresAlpha);
        canvas.drawCircle(targetEarth.x, targetEarth.y, RAD_EARTH + (int)(targetEarth.echoAge*40), blendColorBlack(0x07E0, a));
    }
    if (targetMars.echoAge < 3.0f) {
        uint8_t a = (uint8_t)((1.0f - targetMars.echoAge * 0.333333f) * spheresAlpha);
        canvas.drawCircle(targetMars.x, targetMars.y, RAD_MARS + (int)(targetMars.echoAge*40), blendColorBlack(0x07E0, a));
    }
    
    drawEarth(targetEarth.x, targetEarth.y, RAD_EARTH, spheresAlpha);
    drawMars(targetMars.x, targetMars.y, RAD_MARS, spheresAlpha);
    
    canvas.setFont(&fonts::Font2); canvas.setTextDatum(middle_center); char buf[16]; 
    targetEarth.textAge += delta; targetMars.textAge += delta;
    if (targetEarth.textAge < 60.0f) { 
      uint8_t a = (uint8_t)((1.0f - targetEarth.textAge * 0.0166667f) * spheresAlpha);
      canvas.setTextColor(blendColorBlack(0x87F0, a));
      snprintf(buf, sizeof(buf), "Hr:%02d", h); 
      canvas.drawString(buf, targetEarth.x + targetEarth.textOffsetX, targetEarth.y + targetEarth.textOffsetY);
    }
    if (targetMars.textAge < 60.0f) { 
      uint8_t a = (uint8_t)((1.0f - targetMars.textAge * 0.0166667f) * spheresAlpha);
      canvas.setTextColor(blendColorBlack(0x87F0, a));
      snprintf(buf, sizeof(buf), "Min:%02d", m); 
      canvas.drawString(buf, targetMars.x + targetMars.textOffsetX, targetMars.y + targetMars.textOffsetY);
    }
  }

  // =====================================================================================
  // 4. COMÈTE V6.10 : RENDU "PARTICLE TRAIL" ORGANIQUE
  // =====================================================================================
  if (cometActive) {
    float p = (exact_seconds - cometStartTime) * 0.066667f; // Traversée en 15 secondes
    
    if (p <= 1.3f) {
      int num_particles = 18;       // Résolution de la queue
      float tail_spread = 0.22f;    // Longueur de la traînée (22% de la ligne)
      
      // On dessine de la queue vers la tête (Painter's algorithm)
      for (int i = num_particles - 1; i >= 0; i--) {
        // Ratio de 0.0 (Tête) à 1.0 (Bout de la queue)
        float ratio = (float)i / (num_particles - 1); 
        float tp = p - (tail_spread * ratio);

        if (tp > 0.0f && tp < 1.0f) {
          int tx = cStartX + (int)((cEndX - cStartX) * tp); 
          int ty = cStartY + (int)((cEndY - cStartY) * tp);
          
          // Profil de taille non-linéaire (tête bombée, queue très fine)
          float size_core = 3.5f * (1.0f - (ratio * ratio));
          if (size_core < 0.5f) size_core = 0.5f;
          float size_aura = size_core * 2.5f;

          // Profil d'opacité décroissant
          uint8_t alpha_core = 255 - (int)(230 * ratio);
          uint8_t alpha_aura = 80 - (int)(70 * ratio);

          // Profil colorimétrique du feu incandescent
          uint16_t color;
          if (ratio < 0.1f) color = 0xFFFF;       // Noyau Blanc
          else if (ratio < 0.3f) color = 0xFFE0;  // Jaune chaud
          else if (ratio < 0.6f) color = 0xFC00;  // Orange brillant
          else if (ratio < 0.85f) color = 0xF800; // Rouge
          else color = 0x8000;                    // Rouge sombre/fumé

          // DESSIN DU HALO (Aura diffuse)
          canvas.fillCircle(tx, ty, size_aura, blendColorBlack(color, alpha_aura));
          
          // DESSIN DU CŒUR (Matière dense)
          canvas.fillCircle(tx, ty, size_core, blendColorBlack(color, alpha_core));

          // EFFET PARTICULES/ÉTINCELLES (dispersion aléatoire maîtrisée sur la queue)
          if (i > 3 && i % 3 == 0) {
             int spark_x = tx + ((i * 7) % 11) - 5; // Jitter de -5 à +5 pixels
             int spark_y = ty + ((i * 11) % 11) - 5;
             canvas.drawPixel(spark_x, spark_y, blendColorBlack(0xFFE0, alpha_core));
          }
        }
      }
    }
  }

  if (radarAlpha > 0) {
    float radS = (exact_seconds * 6.0f - 90.0f) * DEG_TO_RAD;
    float cosS = cos(radS); float sinS = sin(radS);
    canvas.drawLine(CENTER_X + (int)(32 * cosS), CENTER_Y + (int)(30 * sinS), 
                    CENTER_X + (int)(112 * cosS), CENTER_Y + (int)(112 * sinS), blendColorBlack(0x07E0, radarAlpha));
  }

  if (shipAlpha > 0) {  
    drawSpaceship(shipX, shipY, 0x07E0, current_ms, shipAlpha, engineOn, alarmOn);
  }
  
  canvas.pushSprite(0, 0); 
}