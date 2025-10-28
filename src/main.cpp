#include <WiFiManager.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Fonts/Picopixel.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>

// ==== Constants for Modes ====
enum ModeType { MODE_TEXT = 0, MODE_LIGHTING = 1 };
enum TextSubMode { TEXT_SCROLL_STATIC = 0, TEXT_SCROLL_RAINBOW = 1 };
enum LightingSubMode {
  LIGHTING_RAINBOW_CYCLE = 0,
  LIGHTING_SPARKLE = 1,
  LIGHTING_COLOR_WIPE = 2,
  LIGHTING_WAVE = 3,
  LIGHTING_FIRE = 4,
  LIGHTING_EQUALIZER = 5,
  LIGHTING_EYE = 6
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

// ==== Helper (Color Conversion) ====
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

// ==== HTML Page (stored in flash to save RAM) ====
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<style>body{background:#111;color:#eee;font-family:sans-serif;text-align:center;}
button,select,input{padding:10px;margin:8px;width:85%;max-width:320px;border:none;border-radius:5px;}
button{background:#2196F3;color:#fff;}</style></head><body>
<h2>ESP32 LED Matrix Controller</h2>
<form action='/set'>
<label>Mode Type:</label><select name='type' onchange='this.form.submit()'>
<option value='0'{MODE_TEXT_SELECTED}>Text Display</option>
<option value='1'{MODE_LIGHTING_SELECTED}>Lighting Effects</option>
</select><br>
<label>Sub Mode:</label><select name='sub'>{SUBMODE_OPTIONS}</select><br>
<label>Brightness:</label><input type='range' name='bright' min='1' max='255' value='{BRIGHTNESS}'><br>
<label>Text Speed (Delay):</label><input type='range' name='txtspd' min='10' max='150' value='{TEXT_SPEED}'><br>
<label>Effect Speed (Delay):</label><input type='range' name='effspd' min='10' max='150' value='{EFFECT_SPEED}'><br>
{TEXT_CONTROLS}
<button type='submit'>Apply</button></form></body></html>
)rawliteral";

String makePage() {
  String page = FPSTR(HTML_PAGE);
  page.replace("{MODE_TEXT_SELECTED}", (modeType == MODE_TEXT) ? " selected" : "");
  page.replace("{MODE_LIGHTING_SELECTED}", (modeType == MODE_LIGHTING) ? " selected" : "");

  String subOptions;
  if (modeType == MODE_TEXT) {
    const char* textSub[] = {"Static Color Scroll", "Rainbow Color Scroll"};
    for (int i = 0; i < 2; i++) {
      subOptions += "<option value='" + String(i) + "'" + (i == subMode ? " selected" : "") + ">" + textSub[i] + "</option>";
    }
  } else {
    const char* lightSub[] = {"Rainbow Cycle","Sparkle","Color Wipe","Wave","Fire","Equalizer","Eye Mode"};
    for (int i = 0; i < 7; i++) {
      subOptions += "<option value='" + String(i) + "'" + (i == subMode ? " selected" : "") + ">" + lightSub[i] + "</option>";
    }
  }
  page.replace("{SUBMODE_OPTIONS}", subOptions);
  page.replace("{BRIGHTNESS}", String(brightness));
  page.replace("{TEXT_SPEED}", String(textScrollSpeed));
  page.replace("{EFFECT_SPEED}", String(effectSpeed));

  if (modeType == MODE_TEXT) {
    String controls = "<label>Text Message:</label><br><input name='msg' value='" + message + "' placeholder='Message'><br>";
    controls += "<label>Text Color:</label><br><input type='color' name='color' value='" + colorHex + "'><br>";
    page.replace("{TEXT_CONTROLS}", controls);
  } else {
    page.replace("{TEXT_CONTROLS}", "");
  }

  return page;
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

  server.on("/", []() { server.send(200, "text/html", makePage()); });

  server.on("/set", []() {
    if (server.hasArg("msg")) {
      message = server.arg("msg") + "  ";
      preferences.putString("msg", message);
    }
    if (server.hasArg("txtspd")) { 
      textScrollSpeed = constrain(server.arg("txtspd").toInt(), 10, 150);
      preferences.putInt("txtspd", textScrollSpeed); 
    }
    if (server.hasArg("effspd")) { 
      effectSpeed = constrain(server.arg("effspd").toInt(), 10, 150);
      preferences.putInt("effspd", effectSpeed); 
    }

    if (server.hasArg("bright")) {
      brightness = constrain(server.arg("bright").toInt(), 1, 255);
      matrix.setBrightness(brightness);
      // CORRECTED LINES 372 & 375: Pass brightness directly.
      stripLeft.setBrightness(brightness);  // Line 372
      stripRight.setBrightness(brightness); // Line 375
      preferences.putInt("bright", brightness);
    }
    if (server.hasArg("color")) {
      colorHex = server.arg("color");
      long colorVal = strtol(colorHex.substring(1).c_str(), NULL, 16);
      uint8_t r = (colorVal >> 16) & 0xFF;
      uint8_t g = (colorVal >> 8) & 0xFF;
      uint8_t b = (colorVal) & 0xFF;
      textColor = matrix.Color(r, g, b);
      preferences.putLong("color", colorVal);
    }
    if (server.hasArg("type")) {
      ModeType newModeType = (ModeType)server.arg("type").toInt();
      if (newModeType != modeType) {
        modeType = newModeType;
        subMode = 0; // reset sub-mode when switching type
        preferences.putInt("sub", subMode);
      } else {
        modeType = newModeType;
      }
      preferences.putInt("type", modeType);
    }
    if (server.hasArg("sub")) {
      subMode = server.arg("sub").toInt();
      preferences.putInt("sub", subMode);
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.begin();
  Serial.println("Web server ready!");
  Serial.println(WiFi.localIP());
}

// ==== LOOP (OPTIMIZED, non-blocking) ====
void loop() {
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
        case LIGHTING_COLOR_WIPE:    colorWipe(matrix.Color(0,150,255)); break;
        case LIGHTING_WAVE:          waveEffect(); break;
        case LIGHTING_FIRE:          fireEffect(); break;
        case LIGHTING_EQUALIZER:     drawEqualizer(); break;
        case LIGHTING_EYE:           eyeMode(); break;
      }
    }

    matrix.show();
    updateSideStrips();
  }
}