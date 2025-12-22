#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>

#include <Adafruit_NeoPixel.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

static const char* AP_SSID = "Adrian's ESPKeychain™";
static const char* AP_PASS = "12345678";

static const int LED_PIN   = 5;
static const int W = 5;
static const int H = 5;
static const int LED_COUNT = W * H;

static const int I2C_SDA = 8;
static const int I2C_SCL = 7;

static const int TOUCH_PIN = 6;


static const bool SERPENTINE = true;


static const bool TOUCH_ACTIVE_HIGH = true;

static const uint32_t HOLD_OVERLAY_COLOR = 0x008080; 


WebServer server(80);
Preferences prefs;

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

Adafruit_MPU6050 mpu;
bool mpu_ok = false;

enum DisplayMode : uint8_t {
  MODE_DRAW = 0,
  MODE_DICE = 1,
  MODE_RAINBOW = 2,
  MODE_LIGHTNING = 3
};

DisplayMode currentMode = MODE_DRAW;  
bool diceAuto = false;                 
uint32_t lastDiceMs = 0;

uint32_t lastModeMs = 0;               


uint32_t drawGrid[LED_COUNT];      
uint32_t previewGrid[LED_COUNT];   

uint8_t  activePreset = 0;        
uint8_t  presetFrame = 0;
bool     presetPaused = false;
uint32_t presetNextMs = 0;         
static const uint32_t PRESET_PAUSE_MS = 2000;

uint8_t  brightness = 50;     
bool     sparkles = false;
bool     animationsEnabled = true; 
uint8_t  arrowDir = 0;

bool     aod = true;         
float    motionThreshold = 1.8f;
bool     wifiEnabled = true;

uint32_t wakeUntilMs = 0;

bool     dirty = false;
uint32_t lastDirtyMs = 0;

uint32_t lastSparkleMs = 0;
int lastSx = -999, lastSy = -999;

bool     indicatorActive = false;
uint32_t indicatorUntilMs = 0;
uint32_t indicatorColor = 0;

bool     touchPrev = false;
bool     touchLongFired = false;
uint32_t touchPressMs = 0;

bool     holdOverlayActive = false;

static inline uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
  if (c >= 'a' && c <= 'f') return (uint8_t)(10 + (c - 'a'));
  if (c >= 'A' && c <= 'F') return (uint8_t)(10 + (c - 'A'));
  return 0;
}

static uint32_t parseHexColor(const String& s) {
  int i = (s.length() >= 7 && s[0] == '#') ? 1 : 0;
  if ((int)s.length() - i < 6) return 0;
  uint8_t r = (hexNibble(s[i]) << 4) | hexNibble(s[i + 1]);
  uint8_t g = (hexNibble(s[i + 2]) << 4) | hexNibble(s[i + 3]);
  uint8_t b = (hexNibble(s[i + 4]) << 4) | hexNibble(s[i + 5]);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static int xyToIndex(int x, int y) {
  if (x < 0 || x >= W || y < 0 || y >= H) return -1;
  if (!SERPENTINE) return y * W + x;

  if ((y % 2) == 0) return y * W + x;
  return y * W + (W - 1 - x);
}

static void markDirty() {
  dirty = true;
  lastDirtyMs = millis();
}

static void clearGrid(uint32_t* g) {
  for (int i = 0; i < LED_COUNT; i++) g[i] = 0;
}

static inline uint8_t r8(uint32_t c){ return (c >> 16) & 0xFF; }
static inline uint8_t g8(uint32_t c){ return (c >> 8) & 0xFF; }
static inline uint8_t b8(uint32_t c){ return (c) & 0xFF; }

static void loadState() {
  prefs.begin("matrix", true);

  brightness      = prefs.getUChar("br", 50);
  sparkles        = prefs.getBool("sp", false);
  animationsEnabled = prefs.getBool("anim", true);
  arrowDir = prefs.getUChar("adir", 0);
  aod             = prefs.getBool("aod", true);
  motionThreshold = prefs.getFloat("mth", 1.8f);
  wifiEnabled     = prefs.getBool("wifi", true);

  currentMode = (DisplayMode)prefs.getUChar("mode", (uint8_t)MODE_DRAW);
  diceAuto    = prefs.getBool("dauto", false);
  if (currentMode != MODE_DICE) diceAuto = false;

  size_t got = prefs.getBytesLength("grid");
  if (got == sizeof(drawGrid)) {
    prefs.getBytes("grid", drawGrid, sizeof(drawGrid));
  } else {
    clearGrid(drawGrid);
  }

  prefs.end();

  clearGrid(previewGrid);
}

static void saveState() {
  prefs.begin("matrix", false);

  prefs.putUChar("br", brightness);
  prefs.putBool("sp", sparkles);
  prefs.putBool("anim", animationsEnabled);
  prefs.putUChar("adir", arrowDir);
  prefs.putBool("aod", aod);
  prefs.putFloat("mth", motionThreshold);
  prefs.putBool("wifi", wifiEnabled);

  prefs.putBytes("grid", drawGrid, sizeof(drawGrid));

  prefs.putUChar("mode", (uint8_t)currentMode);
  prefs.putBool("dauto", diceAuto);

  prefs.end();
  dirty = false;
}

static bool isMatrixOn() {
  if (aod) return true;
  return (millis() < wakeUntilMs);
}

static inline uint32_t* displayBuffer() {
  if (currentMode == MODE_DRAW) return drawGrid;
  return previewGrid;
}

static void transitionTo(const uint32_t* fromBuf, const uint32_t* toBuf, uint16_t steps = 10, uint16_t stepDelayMs = 16) {
  if (!isMatrixOn()) return;
  if (indicatorActive) return;
  if (holdOverlayActive) return;

  strip.setBrightness(brightness);

  for (uint16_t s = 0; s <= steps; s++) {
    float t = (steps == 0) ? 1.0f : (float)s / (float)steps;
    for (int i = 0; i < LED_COUNT; i++) {
      uint32_t a = fromBuf[i];
      uint32_t b = toBuf[i];
      uint8_t rr = (uint8_t)(r8(a) + (int)(r8(b) - r8(a)) * t);
      uint8_t gg = (uint8_t)(g8(a) + (int)(g8(b) - g8(a)) * t);
      uint8_t bb = (uint8_t)(b8(a) + (int)(b8(b) - b8(a)) * t);
      strip.setPixelColor(i, strip.Color(rr, gg, bb));
    }
    strip.show();
    delay(stepDelayMs);
  }
}

static void applyMatrix() {
  bool on = isMatrixOn();


  if (holdOverlayActive) {
    strip.setBrightness(brightness);
    uint8_t r = (HOLD_OVERLAY_COLOR >> 16) & 0xFF;
    uint8_t g = (HOLD_OVERLAY_COLOR >> 8) & 0xFF;
    uint8_t b = (HOLD_OVERLAY_COLOR) & 0xFF;
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(r, g, b));
    strip.show();
    return;
  }

  if (indicatorActive && millis() < indicatorUntilMs) {
    strip.setBrightness(brightness);
    uint8_t r = (indicatorColor >> 16) & 0xFF;
    uint8_t g = (indicatorColor >> 8) & 0xFF;
    uint8_t b = (indicatorColor) & 0xFF;
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(r, g, b));
    strip.show();
    return;
  } else {
    indicatorActive = false;
  }

  strip.setBrightness(on ? brightness : 0);

  uint32_t* g = on ? displayBuffer() : nullptr;

  for (int i = 0; i < LED_COUNT; i++) {
    uint32_t c = (g ? g[i] : 0);
    strip.setPixelColor(i, strip.Color(r8(c), g8(c), b8(c)));
  }
  strip.show();
}

static void startIndicator(uint32_t rgb, uint32_t ms = 3000) {
  indicatorColor = rgb;
  indicatorActive = true;
  indicatorUntilMs = millis() + ms;
  applyMatrix();
}

static void doSparklesOverlay() {
  if (currentMode != MODE_DRAW) return;
  if (!sparkles) return;
  if (!isMatrixOn()) return;
  if (indicatorActive) return;
  if (holdOverlayActive) return;

  uint32_t now = millis();
  if (now - lastSparkleMs < 120) return;
  if (random(0, 100) > 10) return;

  int tries = 30;
  int x = 0, y = 0;
  while (tries--) {
    x = random(0, W);
    y = random(0, H);
    int dx = x - lastSx;
    int dy = y - lastSy;
    if ((dx*dx + dy*dy) >= 6) break;
  }

  int idx = xyToIndex(x, y);
  if (idx < 0) return;

  uint32_t original = drawGrid[idx];

  strip.setBrightness(brightness);
  strip.setPixelColor(idx, strip.Color(255, 255, 255));
  strip.show();

  delay(25);

  strip.setPixelColor(idx, strip.Color(r8(original), g8(original), b8(original)));
  strip.show();

  lastSx = x; lastSy = y;
  lastSparkleMs = now;
}


static void updateMotion() {
  if (aod) return;
  if (!mpu_ok) return;

  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  float mag = sqrtf(a.acceleration.x * a.acceleration.x +
                    a.acceleration.y * a.acceleration.y +
                    a.acceleration.z * a.acceleration.z);

  float delta = fabsf(mag - 9.81f);

  if (delta >= motionThreshold) {
    wakeUntilMs = millis() + 10000;
  }
}

static void setupRoutes(); 

static void wifiStart() {
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  delay(120);

  IPAddress ip = WiFi.softAPIP();
  Serial.println(ok ? "WiFi AP: PORNIT" : "WiFi AP: FAIL");
  Serial.print("SSID: "); Serial.println(AP_SSID);
  Serial.print("IP: "); Serial.println(ip);
  Serial.println("URL: http://192.168.4.1/");

  setupRoutes();
  server.begin();
  Serial.println("HTTP: port 80");
}

static void wifiStop() {
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi AP: OPRIT");
}

static void toggleWifi() {
  wifiEnabled = !wifiEnabled;
  markDirty();
  saveState();

  if (wifiEnabled) {
    startIndicator(0x0000FF, 3000);
    wifiStart();
  } else {
    startIndicator(0xFF0000, 3000);
    wifiStop();
  }
}

static bool readTouchPressed() {
  int v = digitalRead(TOUCH_PIN);
  bool pressed = TOUCH_ACTIVE_HIGH ? (v == HIGH) : (v == LOW);
  return pressed;
}

static void toggleAOD() {
  aod = !aod;
  if (!aod) wakeUntilMs = millis() + 10000;
  markDirty();
  applyMatrix();
}

static void updateTouch() {
  bool pressed = readTouchPressed();
  uint32_t now = millis();

  static uint32_t lastChange = 0;
  static bool stable = false;

  if (pressed != stable) {
    if (now - lastChange > 25) {
      stable = pressed;
      lastChange = now;

      if (stable) {
        touchPressMs = now;
        touchLongFired = false;
        holdOverlayActive = true;  
        applyMatrix();
      } else {
        holdOverlayActive = false; 
        uint32_t held = now - touchPressMs;
        if (!touchLongFired && held < 5000) {
          toggleAOD();
          saveState();
        }
      }
    }
  } else {
    if (stable && !touchLongFired) {
      holdOverlayActive = true;

      if (now - touchPressMs >= 5000) {
        touchLongFired = true;
        holdOverlayActive = false; 
        toggleWifi();
        applyMatrix();
      }
    }
  }
}

static const uint8_t dicePatterns[6][25] = {
  {0,0,0,0,0,  0,0,0,0,0,  0,0,1,0,0,  0,0,0,0,0,  0,0,0,0,0},
  {1,0,0,0,0,  0,0,0,0,0,  0,0,0,0,0,  0,0,0,0,0,  0,0,0,0,1},
  {1,0,0,0,0,  0,0,0,0,0,  0,0,1,0,0,  0,0,0,0,0,  0,0,0,0,1},
  {1,0,0,0,1,  0,0,0,0,0,  0,0,0,0,0,  0,0,0,0,0,  1,0,0,0,1},
  {1,0,0,0,1,  0,0,0,0,0,  0,0,1,0,0,  0,0,0,0,0,  1,0,0,0,1},
  {1,0,0,0,1,  0,0,0,0,0,  1,0,0,0,1,  0,0,0,0,0,  1,0,0,0,1}
};

static void rollDiceOnce() {
  int d = random(0, 6);
  clearGrid(previewGrid);
  for (int i = 0; i < LED_COUNT; i++) {
    if (dicePatterns[d][i]) previewGrid[i] = 0xFFFFFF;
  }
  applyMatrix();
}

static uint32_t wheel(uint8_t p) {
  p = 255 - p;
  if (p < 85) return strip.Color(255 - p * 3, 0, p * 3);
  if (p < 170) { p -= 85; return strip.Color(0, p * 3, 255 - p * 3); }
  p -= 170;
  return strip.Color(p * 3, 255 - p * 3, 0);
}

static void updateRainbow() {
  if (currentMode != MODE_RAINBOW) return;
  if (!isMatrixOn()) return;
  if (indicatorActive) return;
  if (holdOverlayActive) return;
  if (activePreset != 0) return;

  uint32_t now = millis();
  if (now - lastModeMs < 120) return;
  lastModeMs = now;

  static uint8_t off = 0;
  for (int y = 0; y < H; y++) {
    uint32_t c = wheel(off + y * 40);
    for (int x = 0; x < W; x++) {
      int idx = xyToIndex(x, y);
      if (idx >= 0) previewGrid[idx] = c;
    }
  }
  off += 4;
  applyMatrix();
}

static void updateLightning() {
  if (currentMode != MODE_LIGHTNING) return;
  if (!isMatrixOn()) return;
  if (indicatorActive) return;
  if (holdOverlayActive) return;
  if (activePreset != 0) return;

  uint32_t now = millis();
  if (now - lastModeMs < 60) return;
  lastModeMs = now;

  static bool striking = false;
  static int lx = 0, ly = 0;
  static uint8_t hold = 0;

  if (!striking) {
    if (random(0, 100) > 10) return;
    striking = true;
    lx = random(0, W);
    ly = 0;
    hold = 0;
    clearGrid(previewGrid);
  }

  if (ly < H) {
    int idx = xyToIndex(lx, ly);
    if (idx >= 0) previewGrid[idx] = 0xE8F5FF;
    if (random(0, 100) < 35) {
      int nx = lx + (random(0, 2) ? 1 : -1);
      if (nx >= 0 && nx < W) {
        int nidx = xyToIndex(nx, ly);
        if (nidx >= 0) previewGrid[nidx] = 0x4DA3FF;
      }
    }
    ly++;
    lx += random(-1, 2);
    if (lx < 0) lx = 0;
    if (lx >= W) lx = W - 1;
    applyMatrix();
    return;
  }

  if (hold < 4) {
    hold++;
    for (int i = 0; i < LED_COUNT; i++) {
      if (random(0, 100) < 35) previewGrid[i] = 0xFFFFFF;
    }
    applyMatrix();
    return;
  }

  striking = false;
  clearGrid(previewGrid);
  applyMatrix();
}


static void putPtsShifted(uint32_t* target, const int* pts, uint16_t nPts, int dx, int dy, uint32_t col){
  for(uint16_t i=0;i<nPts;i+=2){
    int x = pts[i] + dx;
    int y = pts[i+1] + dy;
    int idx = xyToIndex(x,y);
    if(idx>=0) target[idx]=col;
  }
}

static void setMaskRotated(uint32_t* target, const uint8_t mask[25], uint8_t rot90, uint32_t col){
  for(int y=0;y<5;y++){
    for(int x=0;x<5;x++){
      int sx=x, sy=y;
      int rx=x, ry=y;
      if(rot90==0){ rx=sx; ry=sy; }
      else if(rot90==1){ rx=4-sy; ry=sx; }
      else if(rot90==2){ rx=4-sx; ry=4-sy; }
      else { rx=sy; ry=4-sx; }

      if(mask[sy*5+sx]){
        int idx = xyToIndex(rx,ry);
        if(idx>=0) target[idx]=col;
      }
    }
  }
}

static void presetFrameTo(uint8_t id, uint8_t frame, uint32_t* target) {
  clearGrid(target);

  switch (id) {
    case 1: {
      static const uint8_t heartMask[25] = {
        0,1,0,1,0,
        1,1,1,1,1,
        1,1,1,1,1,
        0,1,1,1,0,
        0,0,1,0,0
      };

      uint8_t f = frame % 6;
      uint8_t width;
      bool mirror = false;

      if (f == 0) width = 5;
      else if (f == 1) width = 3;
      else if (f == 2) width = 1;
      else if (f == 3) width = 3;
      else if (f == 4) width = 5;
      else { width = 3; mirror = true; }

      uint32_t col = (width == 1) ? 0xFFD1DC : (width == 3 ? 0xFF5A7A : 0xFF0033);

      int minX = (5 - width) / 2;
      int maxX = minX + width - 1;

      for(int y=0;y<5;y++){
        for(int x=0;x<5;x++){
          int sx = mirror ? 4-x : x;
          int sy = y;
          if(heartMask[sy*5+sx]){
            if(x >= minX && x <= maxX){
              int idx = xyToIndex(x,y);
              if(idx >= 0) target[idx] = col;
            }
          }
        }
      }
    } break;

    case 2: { 
      const uint32_t Y = 0xFFD000;
      const uint32_t K = 0x000000;

      for(int i=0;i<LED_COUNT;i++) target[i]=Y;

      target[xyToIndex(1,1)] = K;
      target[xyToIndex(3,1)] = K;

      uint8_t f = frame % 3;

      if(f==0){
        target[xyToIndex(1,3)] = K;
        target[xyToIndex(2,4)] = K;
        target[xyToIndex(3,3)] = K;
      } else if(f==1){
        target[xyToIndex(1,4)] = K;
        target[xyToIndex(2,4)] = K;
        target[xyToIndex(3,4)] = K;
        target[xyToIndex(0,3)] = K;
        target[xyToIndex(4,3)] = K;
      } else {
        target[xyToIndex(1,3)] = K;
        target[xyToIndex(2,4)] = K;
        target[xyToIndex(3,3)] = K;
        target[xyToIndex(0,2)] = 0xFF6FA0;
        target[xyToIndex(4,2)] = 0xFF6FA0;
      }
    } break;

    case 3: {
      uint8_t f = frame % 3;
      bool inv = (f == 1);
      bool blink = (f == 2);
      for(int y=0;y<H;y++){
        for(int x=0;x<W;x++){
          int idx=xyToIndex(x,y);
          if(blink){
            target[idx] = ((x+y)&1) ? 0xFFFFFF : 0x101010;
          } else {
            bool b = ((x+y)&1);
            if(inv) b = !b;
            target[idx] = b ? 0x222222 : 0xFFFFFF;
          }
        }
      }
    } break;

    case 4: { 
      const uint32_t C1=0xFF3300, C2=0xFF6600, C3=0xFF9900, C4=0xFFCC00;
      uint8_t f = frame % 4;
      for(int y=0;y<H;y++){
        for(int x=0;x<W;x++){
          int idx=xyToIndex(x,y);
          if(y==4) target[idx]=C1;
          else if(y==3) target[idx]= (f&1)?C2:C1;
          else if(y==2) target[idx]= (f&1)?C3:C2;
          else if(y==1) target[idx]= (f&1)?C4:C3;
          else target[idx]=0;
        }
      }
      if(f==0){ target[xyToIndex(2,0)] = C4; }
      if(f==1){ target[xyToIndex(1,0)] = C4; }
      if(f==2){ target[xyToIndex(3,0)] = C4; }
      if(f==3){ target[xyToIndex(2,0)] = C4; target[xyToIndex(2,1)] = C4; }
    } break;

    case 5: { 
      uint32_t C=0x00FF66;
      if((frame%2)==0){
        for(int i=0;i<5;i++){ target[xyToIndex(2,i)]=C; target[xyToIndex(i,2)]=C; }
      } else {
        for(int i=0;i<5;i++){ target[xyToIndex(i,i)]=C; target[xyToIndex(4-i,i)]=C; }
      }
    } break;

    case 6: { 
      uint32_t C = (frame%2==0)?0x00B4FF:0x003A55;
      for(int i=0;i<5;i++){ target[xyToIndex(i,i)]=C; target[xyToIndex(4-i,i)]=C; }
    } break;

    case 7: {
      uint8_t f = frame % 4;
      uint8_t len = (f==0)?2:(f==1)?3:(f==2)?4:3;
      uint32_t C = 0xFFFFFF;

      auto put=[&](int x,int y){ int i=xyToIndex(x,y); if(i>=0) target[i]=C; };
      int cx=2, cy=2;

      if (arrowDir==0) { 
        for(int x=0;x<=len;x++) put(x,cy);
        int t=len;
        put(t,cy);
        if(t-1>=0){ put(t-1,cy-1); put(t-1,cy+1); }
        if(len>=3 && t-2>=0){ put(t-2,cy-2); put(t-2,cy+2); }
      } else if (arrowDir==1) { 
        for(int x=4;x>=4-len;x--) put(x,cy);
        int t=4-len;
        put(t,cy);
        if(t+1<=4){ put(t+1,cy-1); put(t+1,cy+1); }
        if(len>=3 && t+2<=4){ put(t+2,cy-2); put(t+2,cy+2); }
      } else if (arrowDir==2) { 
        for(int y=4;y>=4-len;y--) put(cx,y);
        int t=4-len;
        put(cx,t);
        if(t+1<=4){ put(cx-1,t+1); put(cx+1,t+1); }
        if(len>=3 && t+2<=4){ put(cx-2,t+2); put(cx+2,t+2); }
      } else { 
        for(int y=0;y<=len;y++) put(cx,y);
        int t=len;
        put(cx,t);
        if(t-1>=0){ put(cx-1,t-1); put(cx+1,t-1); }
        if(len>=3 && t-2>=0){ put(cx-2,t-2); put(cx+2,t-2); }
      }
    } break;
    case 8: { 
      uint32_t C=0xFF00FF;
      for(int x=0;x<5;x++){ target[xyToIndex(x,0)]=C; target[xyToIndex(x,4)]=C; }
      for(int y=0;y<5;y++){ target[xyToIndex(0,y)]=C; target[xyToIndex(4,y)]=C; }
      uint8_t f = frame % 4;
      int cx = (f==0)?0:(f==1)?4:(f==2)?4:0;
      int cy = (f==0)?0:(f==1)?0:(f==2)?4:4;
      target[xyToIndex(cx,cy)] = 0xFFFFFF;
    } break;

    default: break;
  }
}


static uint8_t presetFrameCount(uint8_t id){
  switch(id){
    case 1: return 6; 
    case 2: return 3;
    case 3: return 3; 
    case 4: return 4; 
    case 5: return 2; 
    case 6: return 2; 
    case 7: return 4; 
    case 8: return 4; 
    default: return 1;
  }
}

static void startPresetAnimation(uint8_t id){
  activePreset = id;
  presetFrame = 0;
  presetPaused = false;
  presetNextMs = 0;
}

static void stopPresetAnimation(){
  activePreset = 0;
  presetFrame = 0;
  presetPaused = false;
  presetNextMs = 0;
}

static void updatePresetAnimation(){
  if (activePreset == 0) return;
  if (currentMode == MODE_DRAW) { stopPresetAnimation(); return; }
  if (!isMatrixOn()) return;
  if (indicatorActive) return;
  if (holdOverlayActive) return;

  uint32_t now = millis();
  if (presetNextMs != 0 && now < presetNextMs) return;

  if (presetPaused){
    presetPaused = false;
    presetFrame = 0;
  }

  presetFrameTo(activePreset, presetFrame, previewGrid);
  applyMatrix();

  presetFrame++;
  uint8_t n = presetFrameCount(activePreset);
  if (presetFrame >= n){
    if (activePreset == 1) {
      presetPaused = false;
      presetFrame = 0;
      presetNextMs = now + 300;
    } else {
      presetPaused = true;
      presetNextMs = now + PRESET_PAUSE_MS;
    }
  } else {
    presetNextMs = (activePreset==1) ? (now + 300) : (now + 140);
  }
}

static void applyPreset(uint8_t id) {
  if (currentMode == MODE_DRAW) {
    presetFrameTo(id, 0, drawGrid);
    applyMatrix();
    markDirty();
  } else {
    if (animationsEnabled) startPresetAnimation(id);
    else { presetFrameTo(id, 0, previewGrid); applyMatrix(); }
  }
}
static void setMode(DisplayMode m) {
  DisplayMode prev = currentMode;

  uint32_t fromBuf[LED_COUNT];
  uint32_t* cur = displayBuffer();
  for(int i=0;i<LED_COUNT;i++) fromBuf[i]=cur[i];

  currentMode = m;

  if (m != MODE_DICE) diceAuto = false;
  lastModeMs = 0;
  stopPresetAnimation();

  if (!aod) wakeUntilMs = millis() + 10000;

  if (m == MODE_DRAW && prev != MODE_DRAW) {
    clearGrid(drawGrid);
    markDirty();
    saveState();
  }

  if (m != MODE_DRAW) clearGrid(previewGrid);

  uint32_t* to = displayBuffer();
  transitionTo(fromBuf, to, 10, 16);
  applyMatrix();
}
static const char PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="ro">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no"/>
  <title>Adrian's ESP Matrix</title>
  <style>
    :root{
      --cell:52px;
      --gap:10px;
      --radius:14px;
      --border: rgba(255,255,255,.18);
      --shadow: rgba(0,0,0,.55);
      --text:#fff;
      --muted: rgba(255,255,255,.7);
      --accent:#025043;
      --accentSoft: rgba(2,80,67,.35);
      --accentLine: rgba(2,80,67,.95);
    }
    body{
      margin:0;
      background: radial-gradient(ellipse at top, #111 0%, #000 65%);
      color: var(--text);
      font-family: system-ui, -apple-system, Segoe UI, Roboto, Arial;
      display:flex;
      flex-direction:column;
      align-items:center;
      min-height:100vh;
      padding:18px 12px 28px;
    }
    h1{
      margin:10px 0 14px;
      font-size:28px;
      letter-spacing:.4px;
      text-shadow: 0 10px 30px rgba(255,255,255,.06);
    }
    .grid{
      display:grid;
      grid-template-columns: repeat(5, var(--cell));
      grid-template-rows: repeat(5, var(--cell));
      gap: var(--gap);
      padding:14px;
      border-radius: 18px;
      background: rgba(255,255,255,.03);
      box-shadow: 0 18px 40px var(--shadow);
      touch-action: none;
      user-select:none;
    }
    .cell{
      width: var(--cell);
      height: var(--cell);
      border-radius: var(--radius);
      background:#111;
      border: 1px solid var(--border);
      box-shadow: inset 0 0 0 1px rgba(0,0,0,.2);
      transition: background 120ms linear;
    }
    .cell.eraser-hover{
      outline: 3px solid var(--accentLine);
      outline-offset: 2px;
    }

    .panel{
      width:min(360px, 92vw);
      margin-top:16px;
      padding:14px 14px 12px;
      border-radius:16px;
      background: rgba(255,255,255,.03);
      border: 1px solid rgba(255,255,255,.08);
      box-shadow: 0 16px 36px rgba(0,0,0,.45);
    }

    .row{ display:flex; align-items:center; justify-content:space-between; gap:10px; margin:10px 0; }
    .row label{ color: var(--muted); font-size:14px; }

    .tools{ display:flex; gap:10px; align-items:center; justify-content:center; margin-bottom:10px; flex-wrap:wrap; }

    .btn{
      padding:10px 14px;
      border-radius:12px;
      border:1px solid rgba(255,255,255,.18);
      background: rgba(255,255,255,.04);
      color:#fff;
      font-weight:700;
      cursor:pointer;
      transition: transform 140ms ease, background 140ms ease;
    }
    .btn:active{ transform: scale(.98); }
    .btn:hover{ background: rgba(255,255,255,.08); }
    .btn.active{
      background: var(--accentSoft);
      border-color: var(--accentLine);
      box-shadow: 0 0 0 2px rgba(2,80,67,.18) inset;
    }
    .btn:disabled{
      opacity:.45;
      cursor:not-allowed;
      filter: grayscale(1);
    }

    .picker{
      width:56px; height:42px;
      border:none;
      padding:0;
      background:transparent;
      border-radius:14px;
      overflow:hidden;
      outline: 1px solid rgba(255,255,255,.18);
    }
    input[type="color"]{
      -webkit-appearance: none;
      appearance: none;
      width:56px; height:42px;
      border: none;
      padding: 0;
      border-radius:14px;
      background: transparent;
    }
    input[type="color"]::-webkit-color-swatch-wrapper{
      padding: 0;
      border-radius:14px;
    }
    input[type="color"]::-webkit-color-swatch{
      border: none;
      border-radius:14px;
    }

    input[type="checkbox"]{ accent-color: var(--accent); }
    input[type="range"]{ width: 190px; accent-color: var(--accent); }

    .small{ font-size:12px; color: var(--muted); margin-top:6px; text-align:center; }

    .sectionTitle{
      font-size:12px;
      color: rgba(255,255,255,.55);
      letter-spacing:.12em;
      text-transform:uppercase;
      margin: 6px 0 10px;
      text-align:center;
    }

    select, option{ color:#fff; background:#111; }

    .modeRadio{ display:inline-flex; align-items:center; gap:0; }
    .modeRadio input{ position:absolute; opacity:0; pointer-events:none; }
</style>
</head>
<body>
  <h1>Adrian's ESP Matrix</h1>

  <div id="grid" class="grid"></div>

  <div class="panel">
    <div class="sectionTitle">Moduri</div>
    <div class="tools" role="radiogroup" aria-label="Moduri">
  <label class="btn modeRadio"><input type="radio" name="mode" id="mDraw" value="0">Draw</label>
  <label class="btn modeRadio"><input type="radio" name="mode" id="mDice" value="1">Dice</label>
  <label class="btn modeRadio"><input type="radio" name="mode" id="mRainbow" value="2">Rainbow </label>
  <label class="btn modeRadio"><input type="radio" name="mode" id="mLightning" value="3">Bolt</label>
</div>


    <div id="drawTools">
      <div class="sectionTitle">Drawing Tools</div>
      <div class="tools">
        <input id="color" class="picker" type="color" value="#ff0000">
        <button id="eraser" class="btn">Eraser</button>
      </div>
    </div>

    <div id="diceSection" style="display:none">
      <div class="sectionTitle">Dice</div>
      <div class="row">
        <button id="rollDice" class="btn">Drop Dice</button>
        <label><input id="diceAuto" type="checkbox"> Auto (3 sec)</label>
      </div>
    </div>


    <div class="tools">
      <button id="p1" class="btn">Heart</button>
      <button id="p2" class="btn">Smiley</button>
      <button id="p3" class="btn">Chess Board</button>
      <button id="p4" class="btn">Fire</button>
      <button id="p5" class="btn">Plus</button>
      <button id="p6" class="btn">X</button>
      <button id="p7" class="btn">Arrow</button>
      <button id="p8" class="btn">Squere</button>
    </div>

<div id="arrowMenu" class="row" style="display:none">
  <label>Orientation</label>
  <select id="adir" style="flex:0 0 150px; padding:10px 12px; border-radius:12px; border:1px solid rgba(255,255,255,.18); background: rgba(255,255,255,.04); color:#fff; -webkit-text-fill-color:#fff;">
    <option value="0">Right</option>
    <option value="1">left</option>
    <option value="2">Up</option>
    <option value="3">Down</option>
  </select>
</div>


    <div class="sectionTitle">Settings</div>
    <div class="row">
      <label>Brightness</label>
      <input id="br" type="range" min="0" max="255" value="50">
    </div>

    <div class="row">
      <label><input id="sp" type="checkbox"> Sparkles</label>
      <label><input id="aod" type="checkbox"> AOD</label>
    </div>

<div class="row">
  <label><input id="anim" type="checkbox"> Animate</label>
  <span></span>
</div>

    <div class="row">
      <label>Motion sensitivity</label>
      <input id="sens" type="range" min="0.2" max="6.0" step="0.1" value="1.8">
    </div>

    <div class="small" id="status">Synchronizing...</div>
  </div>

<script>
  const W=5,H=5;
  const gridEl = document.getElementById('grid');
  const colorEl = document.getElementById('color');
  const eraserBtn = document.getElementById('eraser');
  const brEl = document.getElementById('br');
  const spEl = document.getElementById('sp');
  const aodEl = document.getElementById('aod');
  const sensEl = document.getElementById('sens');
  const arrowMenu = document.getElementById('arrowMenu');
  const adirEl = document.getElementById('adir');
  const animEl = document.getElementById('anim');
  const statusEl = document.getElementById('status');

  const mDraw = document.getElementById('mDraw');
  const mDice = document.getElementById('mDice');
  const mRainbow = document.getElementById('mRainbow');
  const mLightning = document.getElementById('mLightning');

  const rollBtn = document.getElementById('rollDice');
  const diceAutoEl = document.getElementById('diceAuto');

  const diceSection = document.getElementById('diceSection');
  const drawTools = document.getElementById('drawTools');

  const pBtns = {
    1: document.getElementById('p1'),
    2: document.getElementById('p2'),
    3: document.getElementById('p3'),
    4: document.getElementById('p4'),
    5: document.getElementById('p5'),
    6: document.getElementById('p6'),
    7: document.getElementById('p7'),
    8: document.getElementById('p8'),
  };

  let cells=[];
  let paint=false;
  let eraser=false;
  let colors=new Array(W*H).fill("#000000");
  let suppressAODWrite = false;

  const MODE_DRAW=0, MODE_DICE=1, MODE_RAINBOW=2, MODE_LIGHTNING=3;
  let mode = MODE_DRAW;

  function idx(x,y){ return y*W+x; }

  function setCellByIndex(i,hex){
    colors[i]=hex;
    if(cells[i]) cells[i].style.background=hex;
  }

  function sendPixel(x,y,hex){
    fetch(`/set?x=${x}&y=${y}&c=${encodeURIComponent(hex)}`).catch(()=>{});
  }

  function applyAtEvent(ev){
    if (mode !== MODE_DRAW) return;
    const t = ev.target;
    if(!t.classList.contains('cell')) return;
    const x = parseInt(t.dataset.x,10);
    const y = parseInt(t.dataset.y,10);
    const hex = eraser ? "#000000" : colorEl.value;
    setCellByIndex(idx(x,y), hex);
    sendPixel(x,y,hex);
  }

  function rebuildGrid(){
    gridEl.innerHTML="";
    cells=[];
    for(let y=0;y<H;y++){
      for(let x=0;x<W;x++){
        const d=document.createElement('div');
        d.className="cell";
        d.dataset.x=x; d.dataset.y=y;
        d.style.background = colors[idx(x,y)];
        d.addEventListener('pointerdown',(e)=>{ paint=true; d.setPointerCapture(e.pointerId); applyAtEvent(e); });
        d.addEventListener('pointerenter',(e)=>{ if(paint) applyAtEvent(e); if(eraser) d.classList.add('eraser-hover'); });
        d.addEventListener('pointerleave',()=>{ d.classList.remove('eraser-hover'); });
        d.addEventListener('pointerup',()=>{ paint=false; });
        cells.push(d);
        gridEl.appendChild(d);
      }
    }
    document.body.addEventListener('pointerup',()=>{ paint=false; });
  }

  function setEraser(on){
    eraser=on;
    eraserBtn.classList.toggle('active', eraser);
    cells.forEach(c=>c.classList.remove('eraser-hover'));
  }

  function setModeUI(m){
    mode = m;
    [mDraw,mDice,mRainbow,mLightning].forEach(b=>b.parentElement && b.parentElement.classList.remove('active'));
    if (m===MODE_DRAW){ mDraw.checked=true; mDraw.parentElement.classList.add('active'); }
    if (m===MODE_DICE){ mDice.checked=true; mDice.parentElement.classList.add('active'); }
    if (m===MODE_RAINBOW){ mRainbow.checked=true; mRainbow.parentElement.classList.add('active'); }
    if (m===MODE_LIGHTNING){ mLightning.checked=true; mLightning.parentElement.classList.add('active'); }

    drawTools.style.display = (m===MODE_DRAW) ? "block" : "none";
    diceSection.style.display = (m===MODE_DICE) ? "block" : "none";

    diceAutoEl.disabled = (m!==MODE_DICE);
    rollBtn.disabled = (m!==MODE_DICE) || diceAutoEl.checked;
  }

  async function setModeRemote(m){
    await fetch(`/cfg?mode=${m}`).catch(()=>{});
  }

  mDraw.addEventListener('change', async ()=>{ if(mDraw.checked){ await setModeRemote(MODE_DRAW); setModeUI(MODE_DRAW);} });
  mDice.addEventListener('change', async ()=>{ if(mDice.checked){ await setModeRemote(MODE_DICE); setModeUI(MODE_DICE);} });
  mRainbow.addEventListener('change', async ()=>{ if(mRainbow.checked){ await setModeRemote(MODE_RAINBOW); setModeUI(MODE_RAINBOW);} });
  mLightning.addEventListener('change', async ()=>{ if(mLightning.checked){ await setModeRemote(MODE_LIGHTNING); setModeUI(MODE_LIGHTNING);} });

  eraserBtn.addEventListener('click',()=> setEraser(!eraser));

  brEl.addEventListener('input',()=> fetch(`/cfg?br=${brEl.value}`).catch(()=>{}));
  spEl.addEventListener('change',()=> fetch(`/cfg?sp=${spEl.checked?1:0}`).catch(()=>{}));

  aodEl.addEventListener('change',()=>{
    if (suppressAODWrite) return;
    fetch(`/cfg?aod=${aodEl.checked?1:0}`).catch(()=>{});
  });

  sensEl.addEventListener('input',()=> fetch(`/cfg?mth=${encodeURIComponent(sensEl.value)}`).catch(()=>{}));

  animEl.addEventListener('change',()=> fetch(`/cfg?anim=${animEl.checked?1:0}`).catch(()=>{}));

  adirEl && adirEl.addEventListener('change',()=> fetch(`/cfg?adir=${adirEl.value}`).catch(()=>{}));

  rollBtn.addEventListener('click',()=> fetch('/dice').catch(()=>{}));

  diceAutoEl.addEventListener('change',()=>{
    rollBtn.disabled = diceAutoEl.checked || (mode!==MODE_DICE);
    fetch(`/cfg?dauto=${diceAutoEl.checked?1:0}`).catch(()=>{});
  });

  function preset(id){
    if (arrowMenu) arrowMenu.style.display = (id===7) ? 'flex' : 'none';
    fetch(`/preset?id=${id}`).catch(()=>{});
  }
  Object.keys(pBtns).forEach(k=>{
    const id=parseInt(k,10);
    pBtns[id].addEventListener('click',()=>preset(id));
  });

  async function fetchState(){
    const r = await fetch('/state');
    return await r.json();
  }

  function applyGridToUI(arr){
    for(let i=0;i<25;i++){
      setCellByIndex(i, arr[i] || "#000000");
    }
  }

  async function fullSync(){
    try{
      const j = await fetchState();
      applyGridToUI(j.grid);

      brEl.value = j.br;
      spEl.checked = !!j.sp;

      suppressAODWrite = true;
      aodEl.checked = !!j.aod;
      suppressAODWrite = false;

      sensEl.value = j.mth;

      animEl.checked = !!j.anim;

      if (typeof j.adir !== 'undefined' && adirEl) adirEl.value = j.adir;

      if (typeof j.adir !== 'undefined' && adirEl) adirEl.value = j.adir;

      setModeUI(j.mode|0);
      diceAutoEl.checked = !!j.dauto;
      rollBtn.disabled = diceAutoEl.checked || (mode!==MODE_DICE);

      rebuildGrid();
      setEraser(false);

      statusEl.textContent = "Connected ✓";
    }catch(e){
      statusEl.textContent = "Sync Error (refresh)";
    }
  }

  async function lightSync(){
    try{
      const j = await fetchState();
      suppressAODWrite = true;
      aodEl.checked = !!j.aod;
      suppressAODWrite = false;

      spEl.checked = !!j.sp;
      brEl.value = j.br;
      sensEl.value = j.mth;

      animEl.checked = !!j.anim;

      if (typeof j.adir !== 'undefined' && adirEl) adirEl.value = j.adir;

      setModeUI(j.mode|0);
      diceAutoEl.checked = !!j.dauto;
      rollBtn.disabled = diceAutoEl.checked || (mode!==MODE_DICE);

      applyGridToUI(j.grid);
      statusEl.textContent = "Connected ✓";
    }catch(e){
      statusEl.textContent = "Offline";
    }
  }

  fullSync();
  setInterval(lightSync, 250);
</script>
</body>
</html>
)HTML";

static void handleRoot() {
  server.send(200, "text/html; charset=utf-8", FPSTR(PAGE));
}
static void appendGridJsonXY(String& json, const uint32_t* g){
  json += "[";
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      int idx = xyToIndex(x, y);  
      uint32_t c = (idx >= 0) ? g[idx] : 0;
      char buf[8];
      snprintf(buf, sizeof(buf), "#%02X%02X%02X", r8(c), g8(c), b8(c));
      json += "\""; json += buf; json += "\"";
      if (!(x == W-1 && y == H-1)) json += ",";
    }
  }
  json += "]";
}
static void handleState() {
  String json;
  json.reserve(2300);
  json += "{";
  json += "\"br\":"; json += String(brightness);
  json += ",\"sp\":"; json += (sparkles ? "true" : "false");
  json += ",\"aod\":"; json += (aod ? "true" : "false");
  json += ",\"mth\":"; json += String(motionThreshold, 1);
  json += ",\"wifi\":"; json += (wifiEnabled ? "true" : "false");
  json += ",\"anim\":"; json += (animationsEnabled ? "true" : "false");
  json += ",\"mode\":"; json += String((int)currentMode);
  json += ",\"dauto\":"; json += (diceAuto ? "true" : "false");
  json += ",\"grid\":";
  appendGridJsonXY(json, displayBuffer());
  json += ",\"draw\":";
  appendGridJsonXY(json, drawGrid);
  json += "}";

  server.send(200, "application/json; charset=utf-8", json);
}

static void handleSetPixel() {
  if (!server.hasArg("x") || !server.hasArg("y") || !server.hasArg("c")) {
    server.send(400, "text/plain; charset=utf-8", "missing args");
    return;
  }
  int x = server.arg("x").toInt();
  int y = server.arg("y").toInt();
  String cstr = server.arg("c");

  uint32_t c = parseHexColor(cstr);
  int idx = xyToIndex(x, y);
  if (idx < 0) {
    server.send(400, "text/plain; charset=utf-8", "bad xy");
    return;
  }

  if (currentMode != MODE_DRAW) setMode(MODE_DRAW);

  drawGrid[idx] = c;
  applyMatrix();
  markDirty();

  server.send(200, "text/plain; charset=utf-8", "ok");
}

static void handleCfg() {
  bool changed = false;
  bool saveNow = false;

  if (server.hasArg("br")) {
    int v = server.arg("br").toInt();
    if (v < 0) v = 0; if (v > 255) v = 255;
    brightness = (uint8_t)v;
    changed = true;
  }
  if (server.hasArg("sp")) {
    sparkles = (server.arg("sp").toInt() != 0);
    changed = true;
  }
  if (server.hasArg("aod")) {
    aod = (server.arg("aod").toInt() != 0);
    if (!aod) wakeUntilMs = millis() + 10000;
    changed = true;
  }

if (server.hasArg("adir")) {
  int v = server.arg("adir").toInt();
  if (v < 0) v = 0;
  if (v > 3) v = 3;
  arrowDir = (uint8_t)v;
  changed = true;
  saveNow = true;
}

  if (server.hasArg("anim")) {
    animationsEnabled = (server.arg("anim").toInt() != 0);
    changed = true;
    saveNow = true;
  }

  if (server.hasArg("mth")) {
    float v = server.arg("mth").toFloat();
    if (v < 0.2f) v = 0.2f;
    if (v > 6.0f) v = 6.0f;
    motionThreshold = v;
    changed = true;
  }

  if (server.hasArg("mode")) {
    int m = server.arg("mode").toInt();
    if (m < 0) m = 0;
    if (m > 3) m = 3;
    setMode((DisplayMode)m);
    changed = true;
    saveNow = true;
  }

  if (server.hasArg("dauto")) {
    diceAuto = (server.arg("dauto").toInt() != 0);
    if (currentMode != MODE_DICE) diceAuto = false;
    changed = true;
    saveNow = true;
  }

  if (changed) {
    applyMatrix();
    markDirty();
  }

  server.send(200, "text/plain; charset=utf-8", "ok");
  if (saveNow) saveState();
}

static void handleDice() {
  if (currentMode != MODE_DICE) setMode(MODE_DICE);
  diceAuto = false;
  stopPresetAnimation();
  rollDiceOnce();
  server.send(200, "text/plain; charset=utf-8", "ok");
}

static void handlePreset() {
  if (!server.hasArg("id")) {
    server.send(400, "text/plain; charset=utf-8", "missing id");
    return;
  }
  int id = server.arg("id").toInt();
  if (id < 1 || id > 8) {
    server.send(400, "text/plain; charset=utf-8", "bad id");
    return;
  }
  applyPreset((uint8_t)id);
  if (currentMode == MODE_DRAW) saveState();
  server.send(200, "text/plain; charset=utf-8", "ok");
}

static void handleNotFound() {
  server.send(404, "text/plain; charset=utf-8", "Not found");
}

static void setupRoutes() {
  server.on("/", handleRoot);
  server.on("/state", handleState);
  server.on("/set", handleSetPixel);
  server.on("/cfg", handleCfg);
  server.on("/dice", handleDice);
  server.on("/preset", handlePreset);
  server.onNotFound(handleNotFound);
}

void setup() {
  Serial.begin(115200);
  delay(250);
  randomSeed(esp_random());

  strip.begin();
  strip.clear();
  strip.show();

  pinMode(TOUCH_PIN, INPUT);

  loadState();
  applyMatrix();

  Wire.begin(I2C_SDA, I2C_SCL);
  mpu_ok = mpu.begin();
  if (mpu_ok) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("MPU6050: OK (I2C SDA=8, SCL=7)");
  } else {
    Serial.println("MPU6050:I2C ERR (SDA=8, SCL=7). AOD OFF.");
  }

if (!wifiEnabled) {
  wifiEnabled = true;
  saveState();
}
wifiStart();

  Serial.println("Touch: tap = toggle AOD, hold 5s = toggle WiFi");
}
void loop() {
  server.handleClient();

  updateTouch();
  updateMotion();

  static bool lastOn = true;
  bool nowOn = isMatrixOn();
  if (nowOn != lastOn) {
    applyMatrix();
    lastOn = nowOn;
  }

  if (indicatorActive && millis() >= indicatorUntilMs) {
    indicatorActive = false;
    applyMatrix();
  }

  if (currentMode == MODE_DICE && diceAuto && activePreset == 0) {
    uint32_t now = millis();
    if (now - lastDiceMs >= 3000) {
      lastDiceMs = now;
      rollDiceOnce();
    }
  }

  if (animationsEnabled) {
    updatePresetAnimation();
    updateRainbow();
    updateLightning();
  }

  doSparklesOverlay();

  if (dirty && (millis() - lastDirtyMs) > 2000) {
    saveState();
  }
}
