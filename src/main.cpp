#include <WiFiManager.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Fonts/Picopixel.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <NimBLEDevice.h>

// Forward declaration(s)
String longToHex(long val);
// Forward declarations for web debug assets
String frameJson();
extern const char DEBUG_PAGE[] PROGMEM;

// ==== Constants for Modes ====
enum ModeType { MODE_TEXT = 0, MODE_LIGHTING = 1 };
enum TextSubMode { TEXT_SCROLL_STATIC = 0, TEXT_SCROLL_RAINBOW = 1 };
enum LightingSubMode {
  LIGHTING_RAINBOW_CYCLE = 0,
  LIGHTING_SPARKLE = 1,
  LIGHTING_WAVE = 2,
  LIGHTING_FIRE = 3,
  LIGHTING_EQUALIZER = 4,
  LIGHTING_EYE = 5
};

// ==== LED Matrix Config ====
#define MATRIX_PIN 5
#define MATRIX_WIDTH 18
#define MATRIX_HEIGHT 5

// New strip definitions for D18 and D19
#define STRIP_LEFT_PIN 18  // D18
#define STRIP_RIGHT_PIN 19 // D19
#define STRIP_LENGTH 12

// Matrix Initialization
Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
  MATRIX_WIDTH, MATRIX_HEIGHT, MATRIX_PIN,
  NEO_MATRIX_BOTTOM + NEO_MATRIX_RIGHT +
  NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG,
  NEO_GRB + NEO_KHZ800
);

// New Strip Initializations
Adafruit_NeoPixel stripLeft = Adafruit_NeoPixel(STRIP_LENGTH, STRIP_LEFT_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripRight = Adafruit_NeoPixel(STRIP_LENGTH, STRIP_RIGHT_PIN, NEO_GRB + NEO_KHZ800);


// ==== Globals ====
WebServer server(80);
Preferences preferences;

// ==== BLE (GATT) ==== 
// Custom Service and Characteristics UUIDs (documented for Flutter app)
// Service: LED Control
static const char* BLE_SVC_LEDCTRL = "12345678-1234-5678-1234-56789abcdef0";
// Characteristics
static const char* BLE_CH_MODE       = "12345678-1234-5678-1234-56789abcdef1"; // uint8: 0=text,1=lighting
static const char* BLE_CH_SUBMODE    = "12345678-1234-5678-1234-56789abcdef2"; // uint8
static const char* BLE_CH_BRIGHT     = "12345678-1234-5678-1234-56789abcdef3"; // uint8 1..255
static const char* BLE_CH_TEXTSPD    = "12345678-1234-5678-1234-56789abcdef4"; // uint16 (ms)
static const char* BLE_CH_EFFSPD     = "12345678-1234-5678-1234-56789abcdef5"; // uint16 (ms)
static const char* BLE_CH_COLOR      = "12345678-1234-5678-1234-56789abcdef6"; // 3 bytes RGB
static const char* BLE_CH_TEXT       = "12345678-1234-5678-1234-56789abcdef7"; // UTF-8 text (<=180 bytes recommended)

NimBLEServer* bleServer = nullptr;
NimBLEService* bleService = nullptr;
NimBLECharacteristic *chMode = nullptr, *chSub = nullptr, *chBright = nullptr,
                     *chTxtSpd = nullptr, *chEffSpd = nullptr, *chColor = nullptr,
                     *chText = nullptr;

String message = "HELLO WORLD!  ";
int textScrollSpeed = 70; // Text scroll speed (non-blocking delay)
int effectSpeed = 70;     // Lighting effect delay
int brightness = 80;
uint16_t textColor = matrix.Color(255, 100, 0);
String colorHex = "#FF6400";
ModeType modeType = MODE_TEXT;  // Text vs Lighting
int subMode = 0;                // Which text or lighting sub-mode

// Non-blocking frame pacing
unsigned long lastFrameAt = 0;

// BLE write callback helper
class CharWriteCB : public NimBLECharacteristicCallbacks {
public:
  enum Target { T_MODE, T_SUBMODE, T_BRIGHT, T_TEXTSPD, T_EFFSPD, T_COLOR, T_TEXT };
  explicit CharWriteCB(Target t) : target(t) {}
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.empty()) return;
    switch (target) {
      case T_MODE: {
        uint8_t m = (uint8_t)v[0];
        if (m > 1) m = 0;
        if ((ModeType)m != modeType) {
          modeType = (ModeType)m;
          subMode = 0;
          preferences.putInt("type", (int)modeType);
          preferences.putInt("sub", subMode);
        }
        break;
      }
      case T_SUBMODE: {
        uint8_t s = (uint8_t)v[0];
        subMode = s;
        preferences.putInt("sub", subMode);
        break;
      }
      case T_BRIGHT: {
        uint8_t b = (uint8_t)v[0];
        if (b == 0) b = 1;
        brightness = b;
        matrix.setBrightness(brightness);
        stripLeft.setBrightness(brightness);
        stripRight.setBrightness(brightness);
        preferences.putInt("bright", brightness);
        break;
      }
      case T_TEXTSPD: {
        if (v.size() >= 2) {
          uint16_t spd = (uint8_t)v[0] | ((uint8_t)v[1] << 8);
          spd = constrain(spd, 10, 1500);
          textScrollSpeed = spd;
          preferences.putInt("txtspd", textScrollSpeed);
        }
        break;
      }
      case T_EFFSPD: {
        if (v.size() >= 2) {
          uint16_t spd = (uint8_t)v[0] | ((uint8_t)v[1] << 8);
          spd = constrain(spd, 10, 1500);
          effectSpeed = spd;
          preferences.putInt("effspd", effectSpeed);
        }
        break;
      }
      case T_COLOR: {
        // Expect 3 bytes: R,G,B
        if (v.size() >= 3) {
          uint8_t r = (uint8_t)v[0];
          uint8_t g = (uint8_t)v[1];
          uint8_t b = (uint8_t)v[2];
          textColor = matrix.Color(r, g, b);
          long colorVal = ((long)r << 16) | ((long)g << 8) | b;
          colorHex = longToHex(colorVal);
          preferences.putLong("color", colorVal);
        }
        break;
      }
      case T_TEXT: {
        // set message with two spaces at end for scroll gap
        String s = String(v.c_str());
        s.trim();
        if (s.length() > 180) s = s.substring(0, 180);
        message = s + "  ";
        preferences.putString("msg", message);
        break;
      }
    }
  }
private:
  Target target;
};

// ==== Helper (Color Conversion) ====
// Forward declaration
String longToHex(long val);
// Convert a 0xRRGGBB long to a "#RRGGBB" String
String longToHex(long val) {
  char buffer[8];
  sprintf(buffer, "#%06lX", val);
  return String(buffer);
}

uint16_t Wheel(byte pos) {
  pos = 255 - pos;
  if (pos < 85) return matrix.Color(255 - pos * 3, 0, pos * 3);
  if (pos < 170) { pos -= 85; return matrix.Color(0, pos * 3, 255 - pos * 3); }
  pos -= 170; return matrix.Color(pos * 3, 255 - pos * 3, 0);
}

// ==== Serial Matrix Simulation (ASCII/ANSI color) ====
#define ENABLE_SERIAL_SIM 0
#define ENABLE_SERIAL_ANSI 1  // set to 0 if ANSI colors are not supported by your terminal

// Map (x,y) to underlying NeoPixel linear index for
// NEO_MATRIX_BOTTOM + NEO_MATRIX_RIGHT + NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG
uint16_t xyIndex(int x, int y) {
  // Flip for RIGHT and BOTTOM orientation
  int ux = (MATRIX_WIDTH - 1) - x;   // RIGHT
  int uy = (MATRIX_HEIGHT - 1) - y;  // BOTTOM

  // Rows layout with zigzag
  bool reverse = (uy & 1); // odd rows reversed
  int posInRow = reverse ? (MATRIX_WIDTH - 1 - ux) : ux;
  return (uint16_t)(uy * MATRIX_WIDTH + posInRow);
}

void renderSimulationAnsi() {
#if ENABLE_SERIAL_SIM && ENABLE_SERIAL_ANSI
  static unsigned long lastSim = 0;
  if (millis() - lastSim < 120) return; // ~8 FPS
  lastSim = millis();

  Serial.println();
  Serial.print("Mode:"); Serial.print((modeType == MODE_TEXT) ? "TEXT" : "LIGHT");
  Serial.print(" Sub:"); Serial.print(subMode);
  Serial.print(" Bright:"); Serial.print(brightness);
  Serial.print(" TSpd:"); Serial.print(textScrollSpeed);
  Serial.print(" ESpd:"); Serial.println(effectSpeed);
  Serial.print("Text: "); Serial.println(message);

  for (int y = 0; y < MATRIX_HEIGHT; y++) {
    for (int x = 0; x < MATRIX_WIDTH; x++) {
      uint16_t idx = xyIndex(x, y);
      uint32_t c = matrix.getPixelColor(idx);
      uint8_t r = (c >> 16) & 0xFF;
      uint8_t g = (c >> 8) & 0xFF;
      uint8_t b = (c) & 0xFF;
      // ANSI 24-bit background color: ESC[48;2;R;G;Bm
      Serial.print("\x1b[48;2;");
      Serial.print(r); Serial.print(';'); Serial.print(g); Serial.print(';'); Serial.print(b); Serial.print('m');
      Serial.print("  "); // two spaces to look like a square
    }
    Serial.print("\x1b[0m\r\n"); // reset and newline
  }
#endif
}

void renderSimulationToSerial() {
#if ENABLE_SERIAL_SIM
  #if ENABLE_SERIAL_ANSI
    renderSimulationAnsi();
    return;
  #endif
  static unsigned long lastSim = 0;
  if (millis() - lastSim < 120) return; // ~8 FPS
  lastSim = millis();

  const char* shades = " .:-=+*#%@"; // 10 levels
  const int levels = 10;

  Serial.println();
  Serial.print("Mode:"); Serial.print((modeType == MODE_TEXT) ? "TEXT" : "LIGHT");
  Serial.print(" Sub:"); Serial.print(subMode);
  Serial.print(" Bright:"); Serial.print(brightness);
  Serial.print(" TSpd:"); Serial.print(textScrollSpeed);
  Serial.print(" ESpd:"); Serial.println(effectSpeed);
  Serial.print("Text: "); Serial.println(message);

  for (int y = 0; y < MATRIX_HEIGHT; y++) {
    String line;
    for (int x = 0; x < MATRIX_WIDTH; x++) {
      uint16_t idx = xyIndex(x, y);
      uint32_t c = matrix.getPixelColor(idx);
      uint8_t r = (c >> 16) & 0xFF;
      uint8_t g = (c >> 8) & 0xFF;
      uint8_t b = (c) & 0xFF;
      int sum = r + g + b; // 0..765
      int li = (sum * (levels - 1)) / 765;
      char ch = shades[li];
      // Add two chars to look more square
      line += ch; line += ch;
    }
    Serial.println(line);
  }
#endif
}


// ==== TEXT EFFECTS (NON-BLOCKING) ====

// Non-blocking single-color scroll
void scrollText(String msg, uint16_t color) {
  static int x = MATRIX_WIDTH;
  int textWidth = msg.length() * 5;
  matrix.fillScreen(0);
  matrix.setCursor(x, 4);
  matrix.setTextColor(color);
  matrix.print(msg);
  x--;
  if (x < -textWidth) x = MATRIX_WIDTH;
}

// Non-blocking rainbow scroll
void rainbowScroll(String msg) {
  static int x = MATRIX_WIDTH;
  int textWidth = msg.length() * 5;
  matrix.fillScreen(0);
  for (int i = 0; i < msg.length(); i++) {
    matrix.setCursor(x + i * 5, 4);
    matrix.setTextColor(Wheel((i * 20 + x * 4) & 255));
    matrix.print(msg[i]);
  }
  x--;
  if (x < -textWidth) x = MATRIX_WIDTH;
}


// ==== LIGHT EFFECTS (USING effectSpeed as wait) ====
void rainbowCycle() {
  static uint16_t j = 0;
  for (int y = 0; y < matrix.height(); y++) {
    for (int x = 0; x < matrix.width(); x++) {
      matrix.drawPixel(x, y, Wheel(((x * 256 / matrix.width()) + j) & 255));
    }
  }
  j++;
}

void sparkleEffect() {
  matrix.fillScreen(0);
  for (int i = 0; i < 5; i++) {
    int x = random(matrix.width());
    int y = random(matrix.height());
    matrix.drawPixel(x, y, matrix.Color(random(255), random(255), random(255)));
  }
}

void colorWipe(uint32_t color) {
  static int i = 0;
  matrix.drawPixel(i % matrix.width(), i / matrix.width(), color);
  i++;
  if (i >= matrix.width() * matrix.height()) i = 0;
}

void waveEffect() {
  static float t = 0;
  for (int x = 0; x < matrix.width(); x++) {
    for (int y = 0; y < matrix.height(); y++) {
      int b = (sin((x + t) * 0.5) + 1) * 127;
      matrix.drawPixel(x, y, matrix.Color(0, 0, b));
    }
  }
  t += 0.3;
}

void fireEffect() {
  for (int y = 0; y < matrix.height(); y++) {
    for (int x = 0; x < matrix.width(); x++) {
      int r = random(150, 255);
      int g = random(0, r / 3);
      matrix.drawPixel(x, y, matrix.Color(r, g, 0));
    }
  }
}

// === Fluid Equalizer Lighting Effect ===
void drawEqualizer() {
  static int barHeight[MATRIX_WIDTH]; 

  static const uint16_t bandColors[] = {
    matrix.Color(255,   0,   0),
    matrix.Color(255, 128,   0),
    matrix.Color(255, 255,   0),
    matrix.Color(0,   255,   0),
    matrix.Color(0,   255, 255),
    matrix.Color(0,   0,   255),
    matrix.Color(128, 0,   255),
    matrix.Color(255,   0, 255),
    matrix.Color(255, 128,   0),
    matrix.Color(255, 255,   0),
    matrix.Color(0,   255,   0),
    matrix.Color(0,   255, 255),
    matrix.Color(0,   0,   255),
    matrix.Color(128, 0,   255),
    matrix.Color(255,   0,   0),
    matrix.Color(255, 128,   0),
    matrix.Color(0,   255,   0),
    matrix.Color(0,   0,   255)
  };
  const int numBandColors = sizeof(bandColors) / sizeof(bandColors[0]);

  for (int i = 0; i < MATRIX_WIDTH; i++) {
    int change = random(-1, 2);
    barHeight[i] = constrain(barHeight[i] + change, 1, MATRIX_HEIGHT);
  }

  for (int x = 0; x < MATRIX_WIDTH; x++) {
    uint16_t currentColor = bandColors[x % numBandColors];
    for (int y = 0; y < MATRIX_HEIGHT; y++) {
      if (y < barHeight[x]) {
        matrix.drawPixel(x, MATRIX_HEIGHT - 1 - y, currentColor);
      } else {
        matrix.drawPixel(x, MATRIX_HEIGHT - 1 - y, 0);
      }
    }
  }
}

void eyeMode() {
  static int eyeX = 0, eyeY = 0;
  static bool blink = false;
  static unsigned long lastBlink = 0;
  static unsigned long lastMove = 0;
  static unsigned long blinkStart = 0;

  // Randomly move eyes every 1000-3000 ms 
  if (millis() - lastMove > random(1000, 5000)) {
    eyeX = random(0, MATRIX_WIDTH - 11); 
    eyeY = random(0, MATRIX_HEIGHT - 5); 
    lastMove = millis();
  }

  // Blink every 5000-10000 ms 
  if (!blink && millis() - lastBlink > random(5000, 15000)) {
    blink = true;
    blinkStart = millis();
    lastBlink = millis();
  }

  matrix.fillScreen(0);

  // Draw two eyes
  int eye1_start_x = eyeX;
  int eye2_start_x = eyeX + 6; 
  int eye_start_y = eyeY;

  // Draw Eye 1
  for(int ex_offset = 0; ex_offset < 5; ex_offset++) {
    for(int ey_offset = 0; ey_offset < 5; ey_offset++) {
      int current_pixel_x = eye1_start_x + ex_offset;
      int current_pixel_y = eye_start_y + ey_offset;

      uint16_t pixel_color = 0;

      if (blink) {
        // Blink state: horizontal line for the eyelid
        if (ey_offset == 2 && ex_offset >= 1 && ex_offset <=3) {
          pixel_color = matrix.Color(0, 0, 255); 
        }
      } else {
        // Open eye state: draw a hollow, rounded square with a pupil
        if ( (ex_offset == 0 && (ey_offset == 1 || ey_offset == 2 || ey_offset == 3)) || 
             (ex_offset == 4 && (ey_offset == 1 || ey_offset == 2 || ey_offset == 3)) || 
             (ey_offset == 0 && (ex_offset == 1 || ex_offset == 2 || ex_offset == 3)) || 
             (ey_offset == 4 && (ex_offset == 1 || ex_offset == 2 || ex_offset == 3)) ) {
          pixel_color = matrix.Color(255, 255, 255); 
        }
        // Pupil
        if (ex_offset == 2 && ey_offset == 2) {
          pixel_color = matrix.Color(0, 0, 255); 
        }
      }
      // Draw pixel for Eye 1
      matrix.drawPixel(current_pixel_x, current_pixel_y, pixel_color);
    }
  }

  // Draw Eye 2
  for(int ex_offset = 0; ex_offset < 5; ex_offset++) {
    for(int ey_offset = 0; ey_offset < 5; ey_offset++) {
      int current_pixel_x = eye2_start_x + ex_offset;
      int current_pixel_y = eye_start_y + ey_offset;

      uint16_t pixel_color = 0; 

      if (blink) {
        if (ey_offset == 2 && ex_offset >= 1 && ex_offset <=3) {
          pixel_color = matrix.Color(0, 0, 255); 
        }
      } else {
        if ( (ex_offset == 0 && (ey_offset == 1 || ey_offset == 2 || ey_offset == 3)) || 
             (ex_offset == 4 && (ey_offset == 1 || ey_offset == 2 || ey_offset == 3)) || 
             (ey_offset == 0 && (ex_offset == 1 || ex_offset == 2 || ex_offset == 3)) || 
             (ey_offset == 4 && (ex_offset == 1 || ex_offset == 2 || ex_offset == 3)) ) {
          pixel_color = matrix.Color(255, 255, 255); 
        }
        if (ex_offset == 2 && ey_offset == 2) {
          pixel_color = matrix.Color(0, 0, 255); 
        }
      }
      // Draw pixel for Eye 2
      matrix.drawPixel(current_pixel_x, current_pixel_y, pixel_color);
    }
  }


  if (blink && millis() - blinkStart > 200) { // blink duration remains short
    blink = false;
  }
}

// ==== FUNCTION TO MIRROR MATRIX EDGES ONTO SIDE STRIPS (SWAPPED SIDES) ====
void updateSideStrips() {
  // Note: Adafruit_NeoPixel/NeoMatrix provides getPixelColor(uint16_t index)
  // but not getPixelColor(x,y). Use a linear index for the pixel.
  // Get the color of the top-left pixel (x=0, y=0)
  uint32_t leftColor = matrix.getPixelColor(0);

  // Get the color of the top-right pixel (x=MATRIX_WIDTH - 1, y=0)
  uint32_t rightColor = matrix.getPixelColor(MATRIX_WIDTH - 1);

  // D19 = LEFT STRIP → show LEFT side color
  for (int i = 0; i < STRIP_LENGTH; i++) {
    stripRight.setPixelColor(i, leftColor);
  }

  // D18 = RIGHT STRIP → show RIGHT side color
  for (int i = 0; i < STRIP_LENGTH; i++) {
    stripLeft.setPixelColor(i, rightColor);
  }

  // Push updates to both LED strips
  stripLeft.show();
  stripRight.show();
}


// ==== SETUP ====
void setup() {
  Serial.begin(115200);
  preferences.begin("ledmatrix", false);

  message = preferences.getString("msg", "HELLO WORLD!  ");
  textScrollSpeed = preferences.getInt("txtspd", 70);
  effectSpeed = preferences.getInt("effspd", 70);
  brightness = preferences.getInt("bright", 80);
  
  long savedColor = preferences.getLong("color", 0xFF6400);
  textColor = matrix.Color((savedColor >> 16) & 0xFF, (savedColor >> 8) & 0xFF, savedColor & 0xFF);
  colorHex = longToHex(savedColor);
  
  modeType = (ModeType)preferences.getInt("type", MODE_TEXT);
  subMode = preferences.getInt("sub", 0);

  // Wi-Fi for web debug UI
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("LED_Controller")) {
    ESP.restart();
  }

  matrix.begin();
  stripLeft.begin();
  stripRight.begin();

  matrix.setFont(&Picopixel);
  matrix.setTextWrap(false);
  matrix.setBrightness(brightness);
  stripLeft.setBrightness(brightness);
  stripRight.setBrightness(brightness);
  // Web debug routes
  server.on("/", [](){ server.send(200, "text/html", FPSTR(DEBUG_PAGE)); });
  server.on("/frame", [](){
    String j = frameJson();
    server.send(200, "application/json", j);
  });
  server.begin();
  Serial.print("Web debug UI at http://"); Serial.println(WiFi.localIP());

  // ===== BLE init =====
  NimBLEDevice::init("DaftPunkHelmet");
  bleServer = NimBLEDevice::createServer();
  bleService = bleServer->createService(BLE_SVC_LEDCTRL);

  chMode   = bleService->createCharacteristic(BLE_CH_MODE,    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  chSub    = bleService->createCharacteristic(BLE_CH_SUBMODE, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  chBright = bleService->createCharacteristic(BLE_CH_BRIGHT,  NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  chTxtSpd = bleService->createCharacteristic(BLE_CH_TEXTSPD, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  chEffSpd = bleService->createCharacteristic(BLE_CH_EFFSPD,  NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  chColor  = bleService->createCharacteristic(BLE_CH_COLOR,   NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  chText   = bleService->createCharacteristic(BLE_CH_TEXT,    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);

  chMode->setCallbacks(new CharWriteCB(CharWriteCB::T_MODE));
  chSub->setCallbacks(new CharWriteCB(CharWriteCB::T_SUBMODE));
  chBright->setCallbacks(new CharWriteCB(CharWriteCB::T_BRIGHT));
  chTxtSpd->setCallbacks(new CharWriteCB(CharWriteCB::T_TEXTSPD));
  chEffSpd->setCallbacks(new CharWriteCB(CharWriteCB::T_EFFSPD));
  chColor->setCallbacks(new CharWriteCB(CharWriteCB::T_COLOR));
  chText->setCallbacks(new CharWriteCB(CharWriteCB::T_TEXT));

  // Set initial values
  uint8_t m = (uint8_t)modeType; chMode->setValue(&m, 1);
  uint8_t s = (uint8_t)subMode;  chSub->setValue(&s, 1);
  uint8_t b = (uint8_t)brightness; chBright->setValue(&b, 1);
  uint16_t ts = (uint16_t)textScrollSpeed; chTxtSpd->setValue((uint8_t*)&ts, 2);
  uint16_t es = (uint16_t)effectSpeed;     chEffSpd->setValue((uint8_t*)&es, 2);
  uint8_t rgb[3] = { (uint8_t)((preferences.getLong("color", 0xFF6400) >> 16) & 0xFF), (uint8_t)((preferences.getLong("color", 0xFF6400) >> 8) & 0xFF), (uint8_t)(preferences.getLong("color", 0xFF6400) & 0xFF) };
  chColor->setValue(rgb, 3);
  chText->setValue(message.c_str());

  bleService->start();
  // Increase ATT MTU to allow larger text packets and improve throughput
  NimBLEDevice::setMTU(247);

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SVC_LEDCTRL);
  adv->setScanResponse(true);
  // Make advertising more discoverable (intervals in 0.625ms units)
  adv->setMinInterval(160); // ~100 ms
  adv->setMaxInterval(240); // ~150 ms
  adv->start();
  Serial.println("BLE ready! Advertised as DaftPunkHelmet");
}

// ==== LOOP (OPTIMIZED, non-blocking) ====
void loop() {
  // Handle web debug requests
  server.handleClient();

  unsigned long now = millis();
  int frameDelay = (modeType == MODE_TEXT) ? textScrollSpeed : effectSpeed;
  if (now - lastFrameAt >= (unsigned long)frameDelay) {
    lastFrameAt = now;

    // Draw the matrix effect for this frame
    if (modeType == MODE_TEXT) {
      switch (subMode) {
        case TEXT_SCROLL_STATIC:  scrollText(message, textColor); break;
        case TEXT_SCROLL_RAINBOW: rainbowScroll(message); break;
      }
    } else {
      switch (subMode) {
        case LIGHTING_RAINBOW_CYCLE: rainbowCycle(); break;
        case LIGHTING_SPARKLE:       sparkleEffect(); break;
        case LIGHTING_WAVE:          waveEffect(); break;
        case LIGHTING_FIRE:          fireEffect(); break;
        case LIGHTING_EQUALIZER:     drawEqualizer(); break;
        case LIGHTING_EYE:           eyeMode(); break;
      }
    }

    matrix.show();
    updateSideStrips();
    #if ENABLE_SERIAL_SIM
      renderSimulationToSerial();
    #endif
  }
}

// ==== Web Debug Page (Color grid) ====
const char DEBUG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>ESP32 Matrix Debug</title>
  <style>
    body{background:#111;color:#eee;font-family:system-ui, sans-serif;margin:0;padding:16px}
    #grid{display:grid;gap:2px;margin-top:12px}
    .px{width:18px;height:18px;background:#000;border-radius:2px}
    .hdr{opacity:.8;font-size:14px}
    .legend{margin-top:8px;font-size:12px;opacity:.8}
  </style>
  <script>
    let w=0, h=0;
    async function refresh(){
      try{
        const res = await fetch('/frame');
        const j = await res.json();
        if (!j || !j.px) return;
        if (w!==j.w || h!==j.h){
          w=j.w; h=j.h;
          const grid = document.getElementById('grid');
          grid.style.gridTemplateColumns = `repeat(${w}, 18px)`;
          grid.innerHTML='';
          for(let i=0;i<w*h;i++){
            const d=document.createElement('div'); d.className='px'; grid.appendChild(d);
          }
        }
        const nodes = document.getElementById('grid').children;
        for(let i=0;i<j.px.length && i<nodes.length;i++){
          nodes[i].style.backgroundColor = j.px[i];
        }
        document.getElementById('hdr').textContent = `Mode:${j.mode} Sub:${j.sub} Bright:${j.bright} T:${j.tspd} E:${j.espd}`;
      }catch(e){/* ignore */}
      setTimeout(refresh, 200);
    }
    window.addEventListener('load', refresh);
  </script>
  </head>
  <body>
    <div class="hdr" id="hdr">Loading…</div>
    <div id="grid"></div>
    <div class="legend">This page auto-refreshes every ~200ms. Colors mirror the current matrix frame.</div>
  </body>
</html>
)rawliteral";

String frameJson() {
  String out = "{";
  out += "\"w\":" + String(MATRIX_WIDTH) + ",";
  out += "\"h\":" + String(MATRIX_HEIGHT) + ",";
  out += "\"mode\":" + String((modeType==MODE_TEXT)?0:1) + ",";
  out += "\"sub\":" + String(subMode) + ",";
  out += "\"bright\":" + String(brightness) + ",";
  out += "\"tspd\":" + String(textScrollSpeed) + ",";
  out += "\"espd\":" + String(effectSpeed) + ",";
  out += "\"px\":[";
  char buf[8];
  for (int y=0; y<MATRIX_HEIGHT; y++) {
    for (int x=0; x<MATRIX_WIDTH; x++) {
      uint16_t idx = xyIndex(x,y);
      uint32_t c = matrix.getPixelColor(idx);
      uint8_t r = (c >> 16) & 0xFF;
      uint8_t g = (c >> 8) & 0xFF;
      uint8_t b = (c) & 0xFF;
      sprintf(buf, "#%02X%02X%02X", r, g, b);
      out += "\""; out += buf; out += "\",";
    }
  }
  if (out[out.length()-1] == ',') out.remove(out.length()-1);
  out += "]}";
  return out;
}