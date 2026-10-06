#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <ESP_I2S.h>

constexpr int PIN_I2S_BCLK  = 4;   
constexpr int PIN_I2S_LRCLK = 5;    
constexpr int PIN_SD_CD     = 6;    
constexpr int PIN_OLED_SCL  = 7;
constexpr int PIN_OLED_SDA  = 8;
constexpr int PIN_SD_DAT2   = 9;   
constexpr int PIN_SD_CS     = 10;
constexpr int PIN_SD_MOSI   = 11;
constexpr int PIN_SD_CLK    = 12;
constexpr int PIN_SD_MISO   = 13;
constexpr int PIN_MIC_CLK   = 14;
constexpr int PIN_MIC_DATA  = 15;
constexpr int PIN_I2S_DOUT  = 16;  
constexpr int PIN_MIC_CTRL  = 17;   
constexpr int PIN_SD_CTRL   = 18;  
constexpr int PIN_LED       = 21;  
constexpr int PIN_AMP_CTRL  = 38;   
constexpr int PIN_BTN1      = 39;
constexpr int PIN_BTN2      = 40;
constexpr int PIN_BTN3      = 41;
constexpr int PIN_BTN4      = 42;
constexpr int PIN_BATT      = 1;    
constexpr int PIN_SD_DAT1   = 2;    

// ===================== KONFIGURASI =====================
constexpr uint32_t STUDY_SECONDS   = 60;       
constexpr uint32_t HUNGER_TICK_MS  = 60000;    
constexpr uint32_t SAVE_EVERY_MS   = 600000;   
constexpr uint32_t DIM_AFTER_MS    = 60000;    
constexpr int      CONTRAST_NORMAL = 200;
constexpr int      CONTRAST_DIM    = 10;
constexpr uint32_t MIC_RATE        = 16000;
constexpr uint32_t SPK_RATE        = 16000;
constexpr int16_t  SPK_AMPL        = 5000;     

// ===================== GLOBAL =====================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
I2SClass i2sMic;
I2SClass i2sSpk;
Preferences prefs;

// ===================== STATE =====================
enum State { ST_HOME, ST_STUDY, ST_QUIZ, ST_RESULT, ST_TEST };
State    state = ST_HOME;
uint16_t growth = 0;          
float    fullness = 60.0f;    // 0..100
uint32_t lastHungerTick = 0;
uint32_t lastSave = 0;

uint32_t studyEnd = 0;
int      qIdx = 0;
bool     resultOk = false;
uint32_t resultUntil = 0;

bool micTesting = false;
int  micLvl = 0;
char testMsg[24] = "SD: -";

float batV = 0;
int   batPct = 0;
uint32_t lastBat = 0;

uint32_t lastActivity = 0;
bool dimmed = false;
uint32_t lastDraw = 0;

// ===================== SOAL CONTOH =====================
struct Quiz { const char* q1; const char* q2; const char* opt[3]; uint8_t ans; };
const Quiz QUIZ[] = {
  {"Hasil dari",           "7 x 8 = ?",    {"54", "56", "58"},                 1},
  {"Planet paling dekat",  "dengan Matahari?", {"Venus", "Mars", "Merkurius"}, 2},
  {"Rumus kimia air?",     "",             {"CO2", "H2O", "NaCl"},             1},
  {"15 + 27 = ?",          "",             {"42", "41", "43"},                 0},
  {"Mamalia bertelur?",    "",             {"Platipus", "Kucing", "Paus"},     0},
};
constexpr int QUIZ_N = sizeof(QUIZ) / sizeof(QUIZ[0]);

// ===================== LOAD SWITCH =====================
inline void railOn(int pin)  { digitalWrite(pin, LOW);  }
inline void railOff(int pin) { digitalWrite(pin, HIGH); }

// ===================== TOMBOL =====================
const int BTN_PINS[4] = {PIN_BTN1, PIN_BTN2, PIN_BTN3, PIN_BTN4};
bool     btnDown[4]  = {false, false, false, false};
bool     btnEvent[4] = {false, false, false, false};
uint32_t btnTime[4]  = {0, 0, 0, 0};

void updateButtons() {
  uint32_t now = millis();
  for (int i = 0; i < 4; i++) {
    bool pr = (digitalRead(BTN_PINS[i]) == LOW);
    if (pr != btnDown[i] && now - btnTime[i] > 30) {
      btnTime[i] = now;
      btnDown[i] = pr;
      if (pr) { btnEvent[i] = true; lastActivity = now; }
    }
  }
}
bool btnPressed(int i) {
  if (btnEvent[i]) { btnEvent[i] = false; return true; }
  return false;
}
void clearButtons() { for (int i = 0; i < 4; i++) btnEvent[i] = false; }

// ===================== BATERAI =====================
void updateBattery() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BATT);
  batV = (sum / 16.0f) * 2.0f / 1000.0f;           // divider 1:2
  float p = (batV - 3.30f) / (4.15f - 3.30f) * 100.0f;
  batPct = constrain((int)p, 0, 100);
}

// ===================== AUDIO =====================
bool spkStart() {
  railOn(PIN_AMP_CTRL);
  delay(10);
  i2sSpk.setPins(PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT);
  if (!i2sSpk.begin(I2S_MODE_STD, SPK_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    railOff(PIN_AMP_CTRL);
    return false;
  }
  return true;
}
void spkStop() {
  i2sSpk.end();
  railOff(PIN_AMP_CTRL);
}

struct Note { uint16_t freq; uint16_t ms; };   
// freq 0 = senyap

void writeTone(uint16_t freq, uint16_t ms) {
  static int16_t buf[128 * 2];
  static float phase = 0;
  uint32_t frames = (uint32_t)SPK_RATE * ms / 1000;
  float step = TWO_PI * freq / SPK_RATE;
  while (frames) {
    uint32_t n = frames > 128 ? 128 : frames;
    for (uint32_t i = 0; i < n; i++) {
      int16_t s = freq ? (int16_t)(sinf(phase) * SPK_AMPL) : 0;
      phase += step;
      if (phase > TWO_PI) phase -= TWO_PI;
      buf[2 * i] = s;
      buf[2 * i + 1] = s;                      
      // amp mode mono L+R
    }
    i2sSpk.write((uint8_t*)buf, n * 4);
    frames -= n;
  }
}

void playNotes(const Note* notes, int n) {
  if (micTesting) return;                      
  if (!spkStart()) { Serial.println("Speaker gagal dibuka"); return; }
  for (int i = 0; i < n; i++) writeTone(notes[i].freq, notes[i].ms);
  writeTone(0, 30);
  spkStop();
}

const Note SND_BOOT[] = {{660, 80}, {880, 120}};
const Note SND_BLIP[] = {{988, 70}};
const Note SND_DONE[] = {{880, 120}, {0, 60}, {880, 120}, {0, 60}, {1175, 200}};
const Note SND_WIN[]  = {{523, 110}, {659, 110}, {784, 220}};
const Note SND_LOSE[] = {{330, 180}, {262, 280}};
const Note SND_TEST[] = {{440, 200}, {880, 200}};

// ===================== MIC (PDM) =====================
bool micStart() {
  railOn(PIN_MIC_CTRL);
  delay(30);
  i2sMic.setPinsPdmRx(PIN_MIC_CLK, PIN_MIC_DATA);
  if (!i2sMic.begin(I2S_MODE_PDM_RX, MIC_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    railOff(PIN_MIC_CTRL);
    return false;
  }
  return true;
}
void micStop() {
  i2sMic.end();
  railOff(PIN_MIC_CTRL);
}
int micReadLevel() {                           
// 0..100
  static int16_t buf[256];
  size_t n = i2sMic.readBytes((char*)buf, sizeof(buf));
  size_t cnt = n / 2;
  if (!cnt) return 0;
  long sum = 0;
  for (size_t i = 0; i < cnt; i++) sum += buf[i];
  long mean = sum / (long)cnt;
  long acc = 0;
  for (size_t i = 0; i < cnt; i++) acc += abs(buf[i] - mean);
  long avg = acc / (long)cnt;
  return constrain((int)map(avg, 0, 4000, 0, 100), 0, 100);
}

// ===================== SD CARD =====================
bool sdProbe() {
  railOn(PIN_SD_CTRL);
  delay(100);
  SPI.begin(PIN_SD_CLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  bool ok = SD.begin(PIN_SD_CS, SPI, 10000000);
  if (ok) {
    uint64_t mb = SD.cardSize() / (1024ULL * 1024ULL);
    snprintf(testMsg, sizeof(testMsg), "SD OK %lluMB", (unsigned long long)mb);
    SD.end();
  } else {
    snprintf(testMsg, sizeof(testMsg), "SD tidak terbaca");
  }
  SPI.end();
  pinMode(PIN_SD_CLK, INPUT);
  pinMode(PIN_SD_MOSI, INPUT);
  pinMode(PIN_SD_MISO, INPUT);
  pinMode(PIN_SD_CS, INPUT);
  railOff(PIN_SD_CTRL);
  return ok;
}

// ===================== PET =====================
int petLevel() { return min(10, 1 + growth / 3); }

void loadPet() {
  prefs.begin("fish", false);
  growth = prefs.getUShort("growth", 0);
  fullness = prefs.getFloat("full", 60.0f);
}
void savePet() {
  prefs.putUShort("growth", growth);
  prefs.putFloat("full", fullness);
  lastSave = millis();
}
void updateHunger(uint32_t now) {
  if (now - lastHungerTick >= HUNGER_TICK_MS) {
    lastHungerTick = now;
    fullness = max(0.0f, fullness - 1.0f);
  }
  if (now - lastSave >= SAVE_EVERY_MS) savePet();
}

// ===================== GAMBAR =====================
void drawFish(int cx, int cy, int rx, bool faceRight) {
  int ry = rx / 2 + 1;
  int dir = faceRight ? -1 : 1;
  int tx = faceRight ? cx - rx : cx + rx;
  int tw = rx / 2 + 3;
  u8g2.drawFilledEllipse(cx, cy, rx, ry);
  u8g2.drawTriangle(tx, cy, tx + dir * tw, cy - ry / 2 - 2, tx + dir * tw, cy + ry / 2 + 2);
  int ex = faceRight ? cx + rx / 2 : cx - rx / 2;
  u8g2.setDrawColor(0);
  u8g2.drawDisc(ex, cy - ry / 3, rx > 16 ? 2 : 1);
  u8g2.setDrawColor(1);
}

void drawHome(uint32_t now) {
  char b[24];
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  snprintf(b, sizeof(b), "Lv%d", petLevel());
  u8g2.drawStr(0, 9, b);
  snprintf(b, sizeof(b), "%d%%", batPct);
  u8g2.drawStr(128 - u8g2.getStrWidth(b), 9, b);
  if (fullness < 25 && ((now / 500) % 2)) u8g2.drawStr(46, 9, "Lapar!");

  int rx = 8 + petLevel() * 2;
  int tw = rx / 2 + 3;
  int amp = max(2, 64 - rx - tw - 4);
  float t = now / 1500.0f;
  int cx = 64 + (int)(amp * sinf(t));
  int cy = 30 + (int)(2 * sinf(now / 400.0f));
  drawFish(cx, cy, rx, cosf(t) > 0);

  u8g2.drawFrame(14, 50, 100, 6);
  u8g2.drawBox(15, 51, (int)(constrain(fullness, 0.0f, 100.0f) * 0.98f), 4);
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(0, 63, "1:Belajar 2:Sapa 4:Tes");
  u8g2.sendBuffer();
}

void drawStudy(uint32_t now) {
  int32_t remain = (int32_t)(studyEnd - now);
  if (remain < 0) remain = 0;
  int sec = remain / 1000;
  char b[16];
  snprintf(b, sizeof(b), "%02d:%02d", sec / 60, sec % 60);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(30, 10, "Fokus belajar...");
  u8g2.setFont(u8g2_font_logisoso24_tr);
  int w = u8g2.getStrWidth(b);
  u8g2.drawStr((128 - w) / 2, 46, b);
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(0, 63, "4:Batal");
  u8g2.sendBuffer();
}

void drawQuiz() {
  const Quiz& q = QUIZ[qIdx];
  char b[32];
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, q.q1);
  u8g2.drawStr(0, 21, q.q2);
  for (int i = 0; i < 3; i++) {
    snprintf(b, sizeof(b), "%d) %s", i + 1, q.opt[i]);
    u8g2.drawStr(0, 38 + i * 11, b);
  }
  u8g2.sendBuffer();
}

void drawResult() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB14_tr);
  if (resultOk) {
    u8g2.drawStr(28, 28, "BENAR!");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(18, 48, "Ikan kenyang & tumbuh");
  } else {
    u8g2.drawStr(34, 28, "SALAH");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(18, 48, "Belajar lagi ya...");
  }
  u8g2.sendBuffer();
}

void drawTest() {
  char b[32];
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  snprintf(b, sizeof(b), "TES  Bat %.2fV %d%%", batV, batPct);
  u8g2.drawStr(0, 9, b);
  u8g2.drawStr(0, 21, testMsg);
  snprintf(b, sizeof(b), "Mic %s", micTesting ? "ON" : "off");
  u8g2.drawStr(0, 33, b);
  u8g2.drawFrame(0, 38, 128, 8);
  u8g2.drawBox(1, 39, (int)(micLvl * 126 / 100), 6);
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(0, 63, "1:Bip 2:Mic 3:SD 4:Back");
  u8g2.sendBuffer();
}

// ===================== PINDAH STATE =====================
void goHome() {
  if (micTesting) { micStop(); micTesting = false; }
  state = ST_HOME;
  clearButtons();
}
void startStudy() {
  studyEnd = millis() + STUDY_SECONDS * 1000UL;
  state = ST_STUDY;
  clearButtons();
}
void startQuiz() {
  qIdx = esp_random() % QUIZ_N;
  state = ST_QUIZ;
  clearButtons();
}

// ===================== HANDLER PER STATE =====================
void handleHome(uint32_t now) {
  if (btnPressed(0)) { startStudy(); return; }
  if (btnPressed(1)) { playNotes(SND_BLIP, 1); }
  if (btnPressed(3)) { state = ST_TEST; clearButtons(); return; }
  if (now - lastDraw >= 100) { lastDraw = now; drawHome(now); }
}

void handleStudy(uint32_t now) {
  if (btnPressed(3)) { goHome(); return; }
  if ((int32_t)(studyEnd - now) <= 0) {
    playNotes(SND_DONE, 5);
    startQuiz();
    return;
  }
  if (now - lastDraw >= 250) { lastDraw = now; drawStudy(now); }
}

void handleQuiz(uint32_t now) {
  for (int i = 0; i < 3; i++) {
    if (btnPressed(i)) {
      resultOk = (i == QUIZ[qIdx].ans);
      if (resultOk) {
        growth++;
        fullness = min(100.0f, fullness + 40.0f);
        savePet();
        playNotes(SND_WIN, 3);
      } else {
        playNotes(SND_LOSE, 2);
      }
      resultUntil = millis() + 2500;
      state = ST_RESULT;
      clearButtons();
      return;
    }
  }
  if (now - lastDraw >= 250) { lastDraw = now; drawQuiz(); }
}

void handleResult(uint32_t now) {
  if ((int32_t)(resultUntil - now) <= 0) { goHome(); return; }
  if (now - lastDraw >= 250) { lastDraw = now; drawResult(); }
}

void handleTest(uint32_t now) {
  if (btnPressed(0)) {
    if (micTesting) { micStop(); micTesting = false; }
    playNotes(SND_TEST, 2);
  }
  if (btnPressed(1)) {
    if (!micTesting) {
      micTesting = micStart();
      if (!micTesting) snprintf(testMsg, sizeof(testMsg), "Mic gagal dibuka");
    } else {
      micStop();
      micTesting = false;
      micLvl = 0;
    }
  }
  if (btnPressed(2)) {
    if (micTesting) { micStop(); micTesting = false; micLvl = 0; }
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 30, "Cek SD...");
    u8g2.sendBuffer();
    sdProbe();
  }
  if (btnPressed(3)) { goHome(); return; }

  if (micTesting) micLvl = (micLvl * 6 + micReadLevel() * 4) / 10;   // smoothing
  if (now - lastDraw >= 80) { lastDraw = now; drawTest(); }
}

// ===================== SETUP & LOOP =====================
void setup() {
  digitalWrite(PIN_MIC_CTRL, HIGH);
  digitalWrite(PIN_SD_CTRL, HIGH);
  digitalWrite(PIN_AMP_CTRL, HIGH);
  pinMode(PIN_MIC_CTRL, OUTPUT);
  pinMode(PIN_SD_CTRL, OUTPUT);
  pinMode(PIN_AMP_CTRL, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  for (int i = 0; i < 4; i++) pinMode(BTN_PINS[i], INPUT_PULLUP);
  pinMode(PIN_SD_DAT1, INPUT);
  pinMode(PIN_SD_DAT2, INPUT);
  pinMode(PIN_SD_CD, INPUT);

  Serial.begin(115200);
  analogSetPinAttenuation(PIN_BATT, ADC_11db);

  Wire.setPins(PIN_OLED_SDA, PIN_OLED_SCL);
  u8g2.begin();
  u8g2.setBusClock(400000);
  u8g2.setContrast(CONTRAST_NORMAL);

  loadPet();
  updateBattery();

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB14_tr);
  u8g2.drawStr(4, 30, "FISHCRACK");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(40, 48, "v2  booting");
  u8g2.sendBuffer();
  playNotes(SND_BOOT, 2);
  delay(500);

  lastActivity = lastHungerTick = lastSave = millis();
  Serial.printf("Fishcrackers v2 siap. Level %d, kenyang %.0f, bat %.2fV\n",
                petLevel(), fullness, batV);
}

void loop() {
  updateButtons();
  uint32_t now = millis();

  if (now - lastBat > 5000) { lastBat = now; updateBattery(); }
  updateHunger(now);

  bool wantDim = (now - lastActivity > DIM_AFTER_MS) && (state == ST_HOME || state == ST_STUDY);
  if (wantDim != dimmed) {
    dimmed = wantDim;
    u8g2.setContrast(dimmed ? CONTRAST_DIM : CONTRAST_NORMAL);
  }

  digitalWrite(PIN_LED, state == ST_STUDY ? HIGH : LOW);

  switch (state) {
    case ST_HOME:   handleHome(now);   break;
    case ST_STUDY:  handleStudy(now);  break;
    case ST_QUIZ:   handleQuiz(now);   break;
    case ST_RESULT: handleResult(now); break;
    case ST_TEST:   handleTest(now);   break;
  }
}
