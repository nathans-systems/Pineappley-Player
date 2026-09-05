#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <U8g2lib.h>
#include "Audio.h"
#include <vector>

#define OLED_SDA      1
#define OLED_SCL      2

#define SD_CS         3
#define SPI_MOSI      4
#define SPI_SCK       5
#define SPI_MISO      6

#define I2S_DOUT      7
#define I2S_BCLK      8
#define I2S_LRC       9

#define BATTERY_PIN   11

#define ENCODER_OUTA  12
#define ENCODER_OUTB  13
#define ENCODER_SW    10

enum UIState { SCREEN_MENU, SCREEN_PLAYER, SCREEN_SETTINGS };
UIState currentScreen = SCREEN_MENU;

enum PlayerButton { BTN_PREV, BTN_PLAY_PAUSE, BTN_NEXT, BTN_SHUFFLE, BTN_SEEK };
PlayerButton selectedPlayerBtn = BTN_PLAY_PAUSE;

enum SettingOption { SETTING_VOL_LIMIT, SETTING_BASS, SETTING_TREBLE, SETTING_BALANCE };
SettingOption selectedSetting = SETTING_VOL_LIMIT;
bool isSettingEditMode = false;

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Audio audio;

std::vector<String> songList;
int currentTrackIndex = 0;
int playingTrackIndex = -1;
int menuScrollOffset = 0;
bool isPlaying = false;
bool isShuffle = false;

float maxVolumeLimit = 4.0f;
int bassBoost = 0;
int trebleBoost = 0;
int balanceLR = 0;

int currentVolume = 50;
bool showVolumeOverlay = false;

bool isSeeking = false;
uint32_t seekTargetSec = 0;

int batteryPercentage = 100;
float filteredVoltageMv = 0.0;
unsigned long lastBatteryCheck = 0;

void updateAudioVolume() {
  float targetVol = (currentVolume / 100.0f) * maxVolumeLimit;
  audio.setVolume(targetVol);
}

void applyEqualizerSettings() {
  audio.setTone(bassBoost, 0, trebleBoost);
  audio.setBalance(balanceLR);
}

int calculateLiPoPercentage(uint32_t mVolts) {
  const uint32_t MIN_VOLTAGE = 3200;
  const uint32_t MAX_VOLTAGE = 4200;

  if (mVolts >= MAX_VOLTAGE) return 100;
  if (mVolts <= MIN_VOLTAGE) return 0;

  return (int)(((float)(mVolts - MIN_VOLTAGE) / (MAX_VOLTAGE - MIN_VOLTAGE)) * 100.0f);
}

void readBatteryLevel() {
  uint32_t rawSum = 0;
  for (int i = 0; i < 32; i++) {
    rawSum += analogReadMilliVolts(BATTERY_PIN);
  }
  float currentPinMv = (float)rawSum / 32.0f;
  float currentBatMv = currentPinMv * 2.0f;

  if (filteredVoltageMv == 0.0) {
    filteredVoltageMv = currentBatMv;
  } else {
    filteredVoltageMv = (filteredVoltageMv * 0.85f) + (currentBatMv * 0.15f);
  }

  batteryPercentage = calculateLiPoPercentage((uint32_t)filteredVoltageMv);
}

void drawBatteryIndicator() {
  u8g2.setFont(u8g2_font_micro_tr);
  
  String pctStr = String(batteryPercentage) + "%";
  int textWidth = u8g2.getStrWidth(pctStr.c_str());
  int textX = 114 - textWidth; 
  u8g2.drawStr(textX, 8, pctStr.c_str());

  u8g2.drawFrame(115, 2, 11, 6);
  u8g2.drawVLine(126, 3, 4);

  int fillWidth = map(batteryPercentage, 0, 100, 0, 7);
  if (fillWidth > 0) {
    u8g2.drawBox(117, 4, fillWidth, 2);
  }
}

volatile int encoderStepDelta = 0; 
portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR encoderISR() {
  static uint8_t oldState = 0;
  static const int8_t knobStates[] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

  oldState <<= 2;
  if (digitalRead(ENCODER_OUTA)) oldState |= 0x02;
  if (digitalRead(ENCODER_OUTB)) oldState |= 0x01;
  oldState &= 0x0F;

  int8_t step = knobStates[oldState];
  if (step != 0) {
    portENTER_CRITICAL_ISR(&encoderMux);
    encoderStepDelta += step;
    portEXIT_CRITICAL_ISR(&encoderMux);
  }
}

bool lastRawBtnState = HIGH;
bool stableBtnState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY = 40;

unsigned long btnPressStartTime = 0;
int clickCount = 0;
unsigned long lastClickReleaseTime = 0;
bool longPressTriggered = false;

int marqueePosMenu = 0;
int marqueePosHeader = 0;
int marqueePosPlayer = 0;
unsigned long lastMarqueeUpdate = 0;

uint8_t barHeights[14] = {0};

const unsigned char epd_bitmap_pineapple_64x64 [] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0xc0, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xe0, 0x07, 0x3e, 0x00, 0x00, 
  0x00, 0x00, 0xff, 0xf1, 0x8f, 0xff, 0x00, 0x00, 0x00, 0x80, 0xff, 0x7b, 0xde, 0xff, 0x01, 0x00, 
  0x00, 0x00, 0x8f, 0x3f, 0xfc, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x1f, 0xf8, 0x70, 0x00, 0x00, 
  0x00, 0x00, 0x1c, 0x1c, 0x38, 0x38, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x38, 0x1c, 0x3c, 0x00, 0x00, 
  0x00, 0x00, 0x38, 0x70, 0x0e, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x70, 0xf0, 0x0f, 0x0e, 0x00, 0x00, 
  0x00, 0x00, 0xf0, 0xe0, 0x07, 0x0f, 0x00, 0x00, 0x00, 0x00, 0xff, 0xe0, 0x07, 0xff, 0x00, 0x00, 
  0x00, 0x80, 0xff, 0xc7, 0xe3, 0xff, 0x01, 0x00, 0x00, 0xc0, 0xe3, 0xcf, 0xf3, 0xc7, 0x03, 0x00, 
  0x00, 0xe0, 0x03, 0xfe, 0x7f, 0xc0, 0x07, 0x00, 0x00, 0xc0, 0x07, 0xf8, 0x1f, 0xe0, 0x03, 0x00, 
  0x00, 0x80, 0x0f, 0xf0, 0x0f, 0xf0, 0x01, 0x00, 0x00, 0x00, 0x1e, 0xe0, 0x07, 0x78, 0x00, 0x00, 
  0x00, 0x00, 0x3c, 0xc0, 0x03, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x78, 0xc0, 0x03, 0x1e, 0x00, 0x00, 
  0x00, 0x00, 0xf0, 0xfc, 0x3f, 0x0f, 0x00, 0x00, 0x00, 0x00, 0xe0, 0xff, 0xff, 0x07, 0x00, 0x00, 
  0x00, 0x00, 0xc0, 0x1f, 0xf8, 0x03, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x03, 0xc0, 0x03, 0x00, 0x00, 
  0x00, 0x00, 0xe0, 0x03, 0xc0, 0x07, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x07, 0xe0, 0x0f, 0x00, 0x00, 
  0x00, 0x00, 0x38, 0x0f, 0xf0, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x1e, 0x78, 0x3c, 0x00, 0x00, 
  0x00, 0x00, 0x1c, 0x3c, 0x3c, 0x38, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x78, 0x1e, 0x70, 0x00, 0x00, 
  0x00, 0x00, 0x0e, 0xf0, 0x0f, 0x70, 0x00, 0x00, 0x00, 0x00, 0x07, 0xe0, 0x07, 0xe0, 0x00, 0x00, 
  0x00, 0x00, 0x07, 0xc0, 0x03, 0xe0, 0x00, 0x00, 0x00, 0x80, 0x03, 0xe0, 0x07, 0xc0, 0x01, 0x00, 
  0x00, 0x80, 0x03, 0xf0, 0x0f, 0xc0, 0x01, 0x00, 0x00, 0x80, 0x07, 0x78, 0x1e, 0xe0, 0x01, 0x00, 
  0x00, 0x80, 0x0f, 0x3c, 0x3c, 0xf0, 0x01, 0x00, 0x00, 0xc0, 0x1f, 0x1e, 0x78, 0xf8, 0x03, 0x00, 
  0x00, 0xc0, 0x39, 0x0f, 0xf0, 0x9c, 0x03, 0x00, 0x00, 0xc0, 0xf1, 0x07, 0xe0, 0x8f, 0x03, 0x00, 
  0x00, 0xc0, 0xe1, 0x03, 0xc0, 0x87, 0x03, 0x00, 0x00, 0xc0, 0xe1, 0x03, 0xc0, 0x87, 0x03, 0x00, 
  0x00, 0xc0, 0xf1, 0x03, 0xc0, 0x8f, 0x03, 0x00, 0x00, 0xc0, 0xf9, 0x07, 0xe0, 0x9f, 0x03, 0x00, 
  0x00, 0xc0, 0x3d, 0x0e, 0xf0, 0xbc, 0x03, 0x00, 0x00, 0xc0, 0x1f, 0x1c, 0x78, 0xf8, 0x03, 0x00, 
  0x00, 0xc0, 0x0f, 0x38, 0x3c, 0xf0, 0x03, 0x00, 0x00, 0x80, 0x07, 0x70, 0x1e, 0xe0, 0x01, 0x00, 
  0x00, 0x80, 0x03, 0xe0, 0x0f, 0xc0, 0x01, 0x00, 0x00, 0x80, 0x03, 0xc0, 0x03, 0xc0, 0x01, 0x00, 
  0x00, 0x80, 0x03, 0xc0, 0x03, 0xc0, 0x01, 0x00, 0x00, 0x00, 0x07, 0xe0, 0x07, 0xe0, 0x00, 0x00, 
  0x00, 0x00, 0x07, 0xf0, 0x0e, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x78, 0x1c, 0x70, 0x00, 0x00, 
  0x00, 0x00, 0x0e, 0x3c, 0x38, 0x70, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x1e, 0x70, 0x38, 0x00, 0x00, 
  0x00, 0x00, 0x3c, 0x07, 0xe0, 0x3c, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x03, 0xc0, 0x1f, 0x00, 0x00, 
  0x00, 0x00, 0xf0, 0x07, 0xe0, 0x0f, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xff, 0xff, 0x03, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x0f, 0x00, 0x00, 0x00
};

void AudioTask(void *pvParameters) {
  for (;;) {
    audio.loop();
    vTaskDelay(1 / portTICK_PERIOD_MS); 
  }
}

void showSplashScreen() {
  u8g2.clearBuffer();
  u8g2.drawXBMP(32, 0, 64, 64, epd_bitmap_pineapple_64x64);
  u8g2.sendBuffer();
  delay(50); 
}

String cleanFileName(String path) {
  int idx = path.lastIndexOf('/');
  if (idx != -1) path = path.substring(idx + 1);
  return path;
}

String formatTime(uint32_t seconds) {
  uint32_t m = seconds / 60;
  uint32_t s = seconds % 60;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02u:%02u", m, s);
  return String(buf);
}

void scanSDCard() {
  File root = SD.open("/");
  if (!root) return;

  songList.clear();
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String fileName = String(file.name());
      if (fileName.endsWith(".mp3") || fileName.endsWith(".MP3")) {
        if (!fileName.startsWith("/")) fileName = "/" + fileName;
        songList.push_back(fileName);
      }
    }
    file = root.openNextFile();
  }
}

void playTrack(int index) {
  if (songList.empty()) return;
  currentTrackIndex = index;
  playingTrackIndex = index; 
  String path = songList[currentTrackIndex];
  
  audio.connecttoFS(SD, path.c_str());
  isPlaying = true;
  marqueePosPlayer = 0;
  isSeeking = false;
  currentScreen = SCREEN_PLAYER;
}

void togglePlayPause() {
  if (songList.empty()) return;
  
  if (isPlaying) {
    audio.pauseResume();
    isPlaying = false;
  } else {
    if (audio.getAudioCurrentTime() == 0) {
      playTrack(currentTrackIndex);
    } else {
      audio.pauseResume();
      isPlaying = true;
    }
  }
}

int getNextTrackIndex() {
  if (songList.empty()) return 0;
  if (isShuffle && songList.size() > 1) {
    int next;
    do {
      next = random(0, songList.size());
    } while (next == playingTrackIndex);
    return next;
  }
  return (playingTrackIndex + 1) % songList.size();
}

void drawMenuUI() {
  u8g2.setFont(u8g2_font_6x10_tf);

  String headerText = "PLAYING: ";
  if (isPlaying && playingTrackIndex >= 0 && playingTrackIndex < (int)songList.size()) {
    headerText += cleanFileName(songList[playingTrackIndex]);
  } else {
    headerText += "[ Stop ]";
  }

  if (headerText.length() > 15) {
    String doubled = headerText + "    " + headerText;
    int subStart = (marqueePosHeader / 3) % (headerText.length() + 4);
    u8g2.drawStr(0, 9, doubled.substring(subStart, subStart + 15).c_str());
  } else {
    u8g2.drawStr(0, 9, headerText.c_str());
  }
  u8g2.drawHLine(0, 11, 128);

  if (currentTrackIndex < menuScrollOffset) {
    menuScrollOffset = currentTrackIndex;
  } else if (currentTrackIndex >= menuScrollOffset + 3) {
    menuScrollOffset = currentTrackIndex - 2;
  }

  for (int i = 0; i < 3; i++) {
    int itemIdx = menuScrollOffset + i;
    if (itemIdx >= (int)songList.size()) break;

    int y = 26 + (i * 16);
    String trackName = cleanFileName(songList[itemIdx]);

    if (itemIdx == currentTrackIndex) {
      u8g2.drawBox(0, y - 11, 128, 15);
      u8g2.setDrawColor(0);

      if (trackName.length() > 18) {
        String doubled = trackName + "   " + trackName;
        int subStart = (marqueePosMenu / 3) % (trackName.length() + 3);
        u8g2.drawStr(2, y, doubled.substring(subStart, subStart + 18).c_str());
      } else {
        u8g2.drawStr(2, y, trackName.c_str());
      }
      u8g2.setDrawColor(1);
    } else {
      if (trackName.length() > 18) trackName = trackName.substring(0, 15) + "..";
      u8g2.drawStr(2, y, trackName.c_str());
    }
  }
}

void drawPlayerUI() {
  u8g2.setFont(u8g2_font_6x10_tf);

  String trackName = (playingTrackIndex >= 0) ? cleanFileName(songList[playingTrackIndex]) : "";
  if (trackName.length() > 15) {
    String doubled = trackName + "    " + trackName;
    int subStart = (marqueePosPlayer / 3) % (trackName.length() + 4);
    u8g2.drawStr(0, 10, doubled.substring(subStart, subStart + 15).c_str());
  } else {
    u8g2.drawStr(0, 10, trackName.c_str());
  }
  u8g2.drawHLine(0, 13, 128);

  uint32_t curSec = isSeeking ? seekTargetSec : audio.getAudioCurrentTime();
  uint32_t totSec = audio.getAudioFileDuration();
  
  u8g2.drawStr(0, 25, formatTime(curSec).c_str());
  u8g2.drawFrame(35, 18, 58, 8);
  
  if (totSec > 0) {
    int fill = map(curSec, 0, totSec, 0, 54);
    u8g2.drawBox(37, 20, fill, 4);
  }
  u8g2.drawStr(96, 25, formatTime(totSec).c_str());

  uint16_t vu = audio.getVUlevel(); 
  uint8_t leftChannel = vu >> 8;
  uint8_t rightChannel = vu & 0xFF;
  uint8_t rawLevel = (leftChannel + rightChannel) / 2;

  uint8_t baseAmplitude = 1;
  if (isPlaying && rawLevel > 0) {
    float gainMultiplier = 3.0f; 
    float scaledLevel = rawLevel * gainMultiplier;
    baseAmplitude = map(constrain((int)scaledLevel, 0, 255), 0, 255, 1, 16);
  }

  for (int b = 0; b < 14; b++) {
    uint8_t targetHeight = 1;

    if (isPlaying && baseAmplitude > 1) {
      float eqWeight = 1.0;
      if (b < 3)       eqWeight = 1.20;
      else if (b < 7)  eqWeight = 1.05;
      else if (b < 11) eqWeight = 0.90;
      else             eqWeight = 0.75;

      int pseudoFreq = (int)(baseAmplitude * eqWeight) + random(-1, 2);
      targetHeight = constrain(pseudoFreq, 1, 16);
    }

    if (targetHeight >= barHeights[b]) {
      barHeights[b] = targetHeight;
    } else {
      if (barHeights[b] > 1) barHeights[b]--;
    }

    u8g2.drawBox(6 + (b * 9), 45 - barHeights[b], 5, barHeights[b]);
  }

  // BTN_PREV
  u8g2.setDrawColor(1);
  if (selectedPlayerBtn == BTN_PREV) {
    u8g2.drawBox(2, 49, 22, 14);
    u8g2.setDrawColor(0);
  }
  u8g2.drawVLine(6, 52, 8);
  u8g2.drawTriangle(12, 52, 12, 59, 8, 55);
  u8g2.drawTriangle(17, 52, 17, 59, 13, 55);

  // BTN_PLAY_PAUSE
  u8g2.setDrawColor(1);
  if (selectedPlayerBtn == BTN_PLAY_PAUSE) {
    u8g2.drawBox(27, 49, 22, 14);
    u8g2.setDrawColor(0);
  }
  if (isPlaying) {
    u8g2.drawBox(33, 52, 3, 8);
    u8g2.drawBox(38, 52, 3, 8);
  } else {
    u8g2.drawTriangle(34, 52, 34, 59, 41, 55);
  }

  // BTN_NEXT
  u8g2.setDrawColor(1);
  if (selectedPlayerBtn == BTN_NEXT) {
    u8g2.drawBox(52, 49, 22, 14);
    u8g2.setDrawColor(0);
  }
  u8g2.drawTriangle(56, 52, 56, 59, 61, 55);
  u8g2.drawTriangle(62, 52, 62, 59, 67, 55);
  u8g2.drawVLine(69, 52, 8);

  // BTN_SHUFFLE
  u8g2.setDrawColor(1);
  if (selectedPlayerBtn == BTN_SHUFFLE) {
    u8g2.drawBox(77, 49, 22, 14);
    u8g2.setDrawColor(0);
  }
  u8g2.drawLine(82, 53, 87, 58);
  u8g2.drawLine(82, 58, 87, 53);
  if (isShuffle) {
    u8g2.drawHLine(82, 60, 12);
  }

  // BTN_SEEK
  u8g2.setDrawColor(1);
  if (selectedPlayerBtn == BTN_SEEK) {
    u8g2.drawBox(102, 49, 24, 14);
    u8g2.setDrawColor(0);
  }
  u8g2.drawCircle(111, 55, 3);
  u8g2.drawLine(113, 57, 117, 61);

  u8g2.setDrawColor(1); 
}

void drawSettingsUI() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "SETTINGS");
  u8g2.drawHLine(0, 11, 128);

  const char* labels[] = {"Max Vol", "Bass", "Treble", "Balance"};
  String values[4];
  
  values[0] = String(maxVolumeLimit, 1) + "x";
  values[1] = (bassBoost > 0 ? "+" : "") + String(bassBoost) + "dB";
  values[2] = (trebleBoost > 0 ? "+" : "") + String(trebleBoost) + "dB";
  
  if (balanceLR == 0) values[3] = "Center";
  else if (balanceLR < 0) values[3] = "L +" + String(-balanceLR);
  else values[3] = "R +" + String(balanceLR);

  for (int i = 0; i < 4; i++) {
    int y = 24 + (i * 10);
    
    if (i == (int)selectedSetting) {
      if (isSettingEditMode) {
        u8g2.drawFrame(0, y - 8, 128, 10);
        u8g2.drawStr(2, y, labels[i]);
        u8g2.drawStr(80, y, values[i].c_str());
      } else {
        u8g2.drawBox(0, y - 8, 128, 10);
        u8g2.setDrawColor(0);
        u8g2.drawStr(2, y, labels[i]);
        u8g2.drawStr(80, y, values[i].c_str());
        u8g2.setDrawColor(1);
      }
    } else {
      u8g2.drawStr(2, y, labels[i]);
      u8g2.drawStr(80, y, values[i].c_str());
    }
  }
}

void drawSeekOverlay() {
  u8g2.drawBox(10, 15, 108, 36);
  u8g2.setDrawColor(0);
  u8g2.drawFrame(11, 16, 106, 34);

  u8g2.setFont(u8g2_font_micro_tr);
  u8g2.drawStr(18, 25, "SEEKING TRACK...");

  u8g2.drawFrame(18, 30, 72, 10);
  uint32_t totSec = audio.getAudioFileDuration();
  if (totSec > 0) {
    int fill = map(seekTargetSec, 0, totSec, 0, 68);
    u8g2.drawBox(20, 32, fill, 6);
  }

  u8g2.setFont(u8g2_font_micro_tr);
  u8g2.drawStr(18, 46, formatTime(seekTargetSec).c_str());
  u8g2.drawStr(65, 46, ("/ " + formatTime(totSec)).c_str());

  u8g2.setDrawColor(1);
}

void drawVolumeOverlay() {
  u8g2.drawBox(10, 15, 108, 36);
  u8g2.setDrawColor(0);
  u8g2.drawFrame(11, 16, 106, 34);

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(18, 28, "VOLUME");

  u8g2.drawFrame(18, 33, 70, 10);
  
  int fillWidth = map(currentVolume, 0, 100, 0, 66);
  u8g2.drawBox(20, 35, fillWidth, 6);

  u8g2.drawStr(92, 42, (String(currentVolume) + "%").c_str());
  
  u8g2.setDrawColor(1); 
}

void renderUI() {
  u8g2.clearBuffer();

  switch (currentScreen) {
    case SCREEN_MENU:     drawMenuUI(); break;
    case SCREEN_PLAYER:   drawPlayerUI(); break;
    case SCREEN_SETTINGS: drawSettingsUI(); break;
  }

  drawBatteryIndicator();

  if (isSeeking) {
    drawSeekOverlay();
  } else if (showVolumeOverlay) {
    drawVolumeOverlay();
  }

  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(9600);
  delay(500);

  pinMode(BATTERY_PIN, INPUT);
  analogReadResolution(12);

  randomSeed(analogRead(0));

  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.setBusClock(400000);
  u8g2.begin();

  showSplashScreen();

  pinMode(ENCODER_OUTA, INPUT_PULLUP);
  pinMode(ENCODER_OUTB, INPUT_PULLUP);
  pinMode(ENCODER_SW, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(ENCODER_OUTA), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_OUTB), encoderISR, CHANGE);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(10, 30, "SD Card Error!");
    u8g2.sendBuffer();
    for (;;);
  }

  scanSDCard();
  if (songList.empty()) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(10, 30, "No MP3s Found!");
    u8g2.sendBuffer();
    for (;;);
  }

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  updateAudioVolume();

  readBatteryLevel();

  xTaskCreatePinnedToCore(
    AudioTask,
    "AudioTask",
    8192,
    NULL,
    2,
    NULL,
    0        
  );
}

void loop() {
  if (millis() - lastBatteryCheck > 3000) {
    readBatteryLevel();
    lastBatteryCheck = millis();
  }

  if (millis() - lastMarqueeUpdate > 120) {
    marqueePosMenu++;
    marqueePosHeader++;
    marqueePosPlayer++;
    lastMarqueeUpdate = millis();
    renderUI();
  }

  int steps = 0;
  if (encoderStepDelta != 0) {
    portENTER_CRITICAL(&encoderMux);
    steps = encoderStepDelta / 4; 
    if (steps != 0) {
      encoderStepDelta %= 4;      
    }
    portEXIT_CRITICAL(&encoderMux);
  }

  if (steps != 0) {
    bool isCW = (steps > 0);
    int stepMagnitude = abs(steps);

    for (int i = 0; i < stepMagnitude; i++) {
      if (isSeeking) {
        uint32_t total = audio.getAudioFileDuration();
        if (isCW) {
          seekTargetSec = min(total, seekTargetSec + 5);
        } else {
          seekTargetSec = (seekTargetSec > 5) ? seekTargetSec - 5 : 0;
        }
      }
      else if (showVolumeOverlay) {
        if (isCW) currentVolume = min(100, currentVolume + 2);
        else      currentVolume = max(0, currentVolume - 2);
        
        updateAudioVolume();
      } 
      else if (currentScreen == SCREEN_SETTINGS) {
        if (isSettingEditMode) {
          if (selectedSetting == SETTING_VOL_LIMIT) {
            maxVolumeLimit = constrain(maxVolumeLimit + (isCW ? 0.5f : -0.5f), 1.0f, 21.0f);
            updateAudioVolume();
          } else if (selectedSetting == SETTING_BASS) {
            bassBoost = constrain(bassBoost + (isCW ? 1 : -1), -12, 12);
            applyEqualizerSettings();
          } else if (selectedSetting == SETTING_TREBLE) {
            trebleBoost = constrain(trebleBoost + (isCW ? 1 : -1), -12, 12);
            applyEqualizerSettings();
          } else if (selectedSetting == SETTING_BALANCE) {
            balanceLR = constrain(balanceLR + (isCW ? 1 : -1), -16, 16);
            applyEqualizerSettings();
          }
        } else {
          if (isCW) {
            selectedSetting = (SettingOption)((selectedSetting + 1) % 4);
          } else {
            selectedSetting = (SettingOption)((selectedSetting + 3) % 4);
          }
        }
      }
      else if (currentScreen == SCREEN_MENU) {
        if (isCW) {
          currentTrackIndex = (currentTrackIndex + 1) % songList.size();
        } else {
          currentTrackIndex = (currentTrackIndex - 1 + songList.size()) % songList.size();
        }
        marqueePosMenu = 0;
      } 
      else if (currentScreen == SCREEN_PLAYER) {
        if (isCW) {
          selectedPlayerBtn = (PlayerButton)((selectedPlayerBtn + 1) % 5);
        } else {
          selectedPlayerBtn = (PlayerButton)((selectedPlayerBtn + 4) % 5);
        }
      }
    }
  }

  bool rawReading = digitalRead(ENCODER_SW);

  if (rawReading != lastRawBtnState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (rawReading != stableBtnState) {
      stableBtnState = rawReading;

      if (stableBtnState == LOW) {
        btnPressStartTime = millis();
        longPressTriggered = false;
      } else {
        if (!longPressTriggered) {
          clickCount++;
          lastClickReleaseTime = millis();
        }
      }
    }
  }
  lastRawBtnState = rawReading;

  if (stableBtnState == LOW && !longPressTriggered) {
    if (millis() - btnPressStartTime >= 1500) {
      if (!isSeeking) {
        showVolumeOverlay = !showVolumeOverlay;
        longPressTriggered = true;
        clickCount = 0; 
      }
    }
  }

  if (clickCount > 0 && (millis() - lastClickReleaseTime > 300)) {
    if (clickCount == 1) {
      if (isSeeking) {
        uint32_t fileDuration = audio.getAudioFileDuration();
        if (fileDuration > 0) {
          seekTargetSec = constrain(seekTargetSec, (uint32_t)0, fileDuration - 1);
        }
        
        audio.setAudioPlayPosition(seekTargetSec); 
        
        if (!isPlaying) {
          audio.pauseResume();
          isPlaying = true;
        }

        isSeeking = false;
      } else if (showVolumeOverlay) {
        showVolumeOverlay = false;
      } else if (currentScreen == SCREEN_SETTINGS) {
        isSettingEditMode = !isSettingEditMode;
      } else if (currentScreen == SCREEN_MENU) {
        playTrack(currentTrackIndex);
      } else if (currentScreen == SCREEN_PLAYER) {
        if (selectedPlayerBtn == BTN_PLAY_PAUSE) {
          togglePlayPause();
        } else if (selectedPlayerBtn == BTN_PREV) {
          int prev = (playingTrackIndex - 1 + songList.size()) % songList.size();
          playTrack(prev);
        } else if (selectedPlayerBtn == BTN_NEXT) {
          playTrack(getNextTrackIndex());
        } else if (selectedPlayerBtn == BTN_SHUFFLE) {
          isShuffle = !isShuffle; 
        } else if (selectedPlayerBtn == BTN_SEEK) {
          isSeeking = true;
          seekTargetSec = audio.getAudioCurrentTime();
        }
      }
    } 
    else if (clickCount == 2) {
      isSeeking = false;
      showVolumeOverlay = false;
      isSettingEditMode = false;
      currentScreen = SCREEN_MENU;
    } 
    else if (clickCount >= 3) {
      isSeeking = false;
      showVolumeOverlay = false;
      isSettingEditMode = false;
      currentScreen = SCREEN_SETTINGS;
    }

    clickCount = 0;
  }
}

void audio_eof_mp3(const char *info) {
  playTrack(getNextTrackIndex());
}
