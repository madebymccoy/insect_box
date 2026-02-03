// ═══════════════════════════════════════════════════════
// AMBIENT BOX - MULTI-CORE VERSION
// Core 0: UI (encoder, buttons, pots, LEDs)
// Core 1: Audio (SD reading, mixing, I2S output)
// ═══════════════════════════════════════════════════════

#include <SD.h>
#include <I2S.h>
#include <Rotary.h>

// ═══════════════════════════════════════════════════════
// PIN DEFINITIONS
// ═══════════════════════════════════════════════════════

#define SD_CS    17
#define I2S_BCLK  10  
#define I2S_DATA  9   
#define POT_PITCH   26
#define POT_FILTER  27
#define POT_VOLUME  28
#define ENC_A       3
#define ENC_B       2
#define ENC_SW      4
#define BTN_TRACK   5
#define LED_TRACK1  20
#define LED_TRACK2  21
#define LED_TRACK3  22

// ═══════════════════════════════════════════════════════
// INTER-CORE COMMANDS
// ═══════════════════════════════════════════════════════

#define CMD_LOAD_SOUND  1
#define CMD_SET_PITCH   2

// ═══════════════════════════════════════════════════════
// SOUND LIBRARY
// ═══════════════════════════════════════════════════════

const char* insectSounds[] = {
  "/insect_ants.wav",
  "/insect_cigales.wav",
  "/insect_crickets.wav",
  "/insect_bees.wav",
  "/insect_var.wav"
};
const int numInsectSounds = 5;

const char* natureSounds[] = {
  "/nature_birds.wav",
  "/nature_birds2.wav",
  "/nature_cal.wav",
  "/nature_manybirds.wav",
  "/nature_seaside.wav",
  "/nature_stream.wav",
  "/nature_thunder.wav"
};
const int numNatureSounds = 7;

const char* padSounds[] = {
  "/pad_fluty.wav",
  "/pad_man.wav",
  "/pad_aquab.wav",
  "/pad_myst.wav",
  "/pad_pady.wav"
};
const int numPadSounds = 5;

// ═══════════════════════════════════════════════════════
// TRACK STRUCTURE
// ═══════════════════════════════════════════════════════

struct Track {
  File wavFile;
  uint32_t dataSize;
  uint32_t dataStart;
  int16_t audioBuffer[256];
  
  // Parameters - marked volatile for cross-core access
  volatile float volume;
  volatile float filter;
  volatile int soundIndex;
  volatile int pendingSoundIndex;  // -1 = no pending change
  
  // Value pickup (core 0 only)
  int potFilterLast;
  bool filterCaught;
  
  const char* name;
  const char** soundList;
  int numSounds;
};

Track tracks[3] = {
  {File(), 0, 0, {}, 0.1, 1, 0, -1, 0, false, "Insects", insectSounds, numInsectSounds},
  {File(), 0, 0, {}, 0.1, 0.2, 0, -1, 0, false, "Nature",  natureSounds,  numNatureSounds},
  {File(), 0, 0, {}, 0.1, 0.8, 0, -1, 0, false, "Pads",    padSounds,     numPadSounds}
};

volatile int activeTrack = 0;
volatile float globalPitch = 1.0;
volatile float globalVolume = 0.5;
volatile bool pitchChanged = false;

// ═══════════════════════════════════════════════════════
// GLOBAL OBJECTS
// ═══════════════════════════════════════════════════════

I2S i2s(OUTPUT, I2S_BCLK, I2S_DATA);
Rotary rotary(ENC_A, ENC_B);

struct SVF {
  float low = 0.0;
  float band = 0.0;
  float high = 0.0;
  float resonance = 0.2;
};

SVF filterInsects = {0, 0, 0, 0.2};
SVF filterNature = {0, 0, 0, 0.2};
SVF filterPads = {0, 0, 0, 0.2};

unsigned long lastEncoderTime = 0;
bool encoderPressed = false;
unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 200;

// ═══════════════════════════════════════════════════════
// WAV PARSING (Core 1 only)
// ═══════════════════════════════════════════════════════

bool parseWavHeader(File &file, uint32_t* outDataSize, uint32_t* outDataStart) {
  uint8_t riff[4];
  file.read(riff, 4);
  if (strncmp((char*)riff, "RIFF", 4) != 0) return false;
  
  uint32_t fileSize;
  file.read((uint8_t*)&fileSize, 4);
  
  uint8_t wave[4];
  file.read(wave, 4);
  if (strncmp((char*)wave, "WAVE", 4) != 0) return false;
  
  while (file.available()) {
    uint8_t chunkId[4];
    file.read(chunkId, 4);
    uint32_t chunkSize;
    file.read((uint8_t*)&chunkSize, 4);
    
    if (strncmp((char*)chunkId, "fmt ", 4) == 0) {
      uint16_t audioFormat, numChannels, bitsPerSample;
      uint32_t sampleRate, byteRate;
      uint16_t blockAlign;
      
      file.read((uint8_t*)&audioFormat, 2);
      file.read((uint8_t*)&numChannels, 2);
      file.read((uint8_t*)&sampleRate, 4);
      file.read((uint8_t*)&byteRate, 4);
      file.read((uint8_t*)&blockAlign, 2);
      file.read((uint8_t*)&bitsPerSample, 2);
      
      if (chunkSize > 16) {
        file.seek(file.position() + (chunkSize - 16));
      }
      
    } else if (strncmp((char*)chunkId, "data", 4) == 0) {
      *outDataSize = chunkSize;
      *outDataStart = file.position();
      return true;
      
    } else {
      file.seek(file.position() + chunkSize);
    }
  }
  
  return false;
}

// ═══════════════════════════════════════════════════════
// LOAD SOUND (Core 1 only)
// ═══════════════════════════════════════════════════════

bool loadSound(int trackIndex, int soundIndex) {
  Track &t = tracks[trackIndex];
  
  if (t.wavFile) {
    t.wavFile.close();
  }
  
  const char* filename = t.soundList[soundIndex];
  
  Serial.print("[Core1] Loading: ");
  Serial.print(t.name);
  Serial.print(" - ");
  Serial.println(filename);
  
  t.wavFile = SD.open(filename);
  if (!t.wavFile) {
    Serial.println("✗ Failed to open!");
    return false;
  }
  
  if (!parseWavHeader(t.wavFile, &t.dataSize, &t.dataStart)) {
    Serial.println("✗ Invalid WAV!");
    t.wavFile.close();
    return false;
  }
  
  t.wavFile.seek(t.dataStart);
  t.soundIndex = soundIndex;
  t.pendingSoundIndex = -1;
  
  Serial.println("✓ Loaded");
  return true;
}

// ═══════════════════════════════════════════════════════
// LED MANAGEMENT (Core 0)
// ═══════════════════════════════════════════════════════

void updateLEDs() {
  digitalWrite(LED_TRACK1, activeTrack == 0 ? HIGH : LOW);
  digitalWrite(LED_TRACK2, activeTrack == 1 ? HIGH : LOW);
  digitalWrite(LED_TRACK3, activeTrack == 2 ? HIGH : LOW);
}

// ═══════════════════════════════════════════════════════
// VALUE PICKUP (Core 0)
// ═══════════════════════════════════════════════════════

void updatePotWithPickup(int potRaw, volatile float &paramValue, int &lastPotValue, bool &caught, float minVal, float maxVal) {
  if (!caught) {
    int currentPotValue = (int)(((paramValue - minVal) / (maxVal - minVal)) * 4095.0);
    if (abs(potRaw - currentPotValue) < 80) {
      caught = true;
      Serial.println("  → Pot caught!");
    }
  }
  
  if (caught) {
    paramValue = minVal + (potRaw / 4095.0) * (maxVal - minVal);
    lastPotValue = potRaw;
  }
}

// ═══════════════════════════════════════════════════════
// ENCODER PROCESSING (Core 0)
// ═══════════════════════════════════════════════════════

void processEncoder() {
  encoderPressed = (digitalRead(ENC_SW) == LOW);
  
  unsigned char result = rotary.process();
  
  if (result == DIR_CW || result == DIR_CCW) {
    
    if (encoderPressed) {
      // VOLUME MODE
      float step = 0.003;
      
      if (result == DIR_CW) {
        tracks[activeTrack].volume += step;
      } else {
        tracks[activeTrack].volume -= step;
      }
      tracks[activeTrack].volume = constrain(tracks[activeTrack].volume, 0.0, 0.2);
      
      Serial.print("Volume: ");
      Serial.println(tracks[activeTrack].volume, 3);
      
    } else {
      // SOUND SELECT MODE - set pending index for Core 1 to pick up
      Track &t = tracks[activeTrack];
      
      int newIndex = t.soundIndex;
      
      if (result == DIR_CW) {
        newIndex++;
      } else {
        newIndex--;
      }
      
      // Wrap around
      if (newIndex < 0) newIndex = t.numSounds - 1;
      if (newIndex >= t.numSounds) newIndex = 0;
      
      if (newIndex != t.soundIndex) {
        t.pendingSoundIndex = newIndex;  // Signal Core 1 to load
        Serial.print("[Core0] Requesting sound change: ");
        Serial.println(newIndex);
      }
    }
  }
}

// ═══════════════════════════════════════════════════════
// TRACK BUTTON (Core 0)
// ═══════════════════════════════════════════════════════

void checkTrackButton() {
  if (digitalRead(BTN_TRACK) == LOW) {
    if (millis() - lastButtonPress > debounceDelay) {
      lastButtonPress = millis();
      
      activeTrack = (activeTrack + 1) % 3;
      
      // Reset filter pickup for new track
      tracks[activeTrack].filterCaught = false;
      
      updateLEDs();
      
      Serial.print("\n→ Switched to: ");
      Serial.print(tracks[activeTrack].name);
      Serial.print(" (sound ");
      Serial.print(tracks[activeTrack].soundIndex + 1);
      Serial.print("/");
      Serial.print(tracks[activeTrack].numSounds);
      Serial.println(")");
    }
  }
}

// ═══════════════════════════════════════════════════════
// CORE 0 SETUP - UI
// ═══════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n═══════════════════════════════════");
  Serial.println("   AMBIENT BOX - MULTI-CORE");
  Serial.println("═══════════════════════════════════\n");

  // Encoder & buttons
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(BTN_TRACK, INPUT_PULLUP);
  
  // LEDs
  pinMode(LED_TRACK1, OUTPUT);
  pinMode(LED_TRACK2, OUTPUT);
  pinMode(LED_TRACK3, OUTPUT);
  
  analogReadResolution(12);
  updateLEDs();
  
  Serial.println("[Core0] UI initialized");
  Serial.println("\nControls:");
  Serial.println("  ENCODER turn = Select sound");
  Serial.println("  ENCODER press+turn = Volume");
  Serial.println("  POT 1 = Pitch (global)");
  Serial.println("  POT 2 = Filter");
  Serial.println("  POT 3 = Volume");
  Serial.println("  BUTTON = Cycle tracks\n");
}

// ═══════════════════════════════════════════════════════
// CORE 0 LOOP - UI (encoder, pots, buttons)
// ═══════════════════════════════════════════════════════

void loop() {
  // Process encoder - this now runs without being blocked by audio!
  processEncoder();
  checkTrackButton();
  
  // Read pots periodically
  static unsigned long lastPotRead = 0;
  if (millis() - lastPotRead > 50) {
    lastPotRead = millis();
    
    // Volume pot
    int potVolRaw = analogRead(POT_VOLUME);  
    globalVolume = (potVolRaw / 4095.0) * 1.0;

    // Pitch pot
    int potPitchRaw = analogRead(POT_PITCH);
    float potNormalized = potPitchRaw / 4095.0;
    float newPitch = 0.3 + (potNormalized * 0.7);
    
    if (abs(newPitch - globalPitch) > 0.01) {
      globalPitch = newPitch;
      pitchChanged = true;  // Signal Core 1 to update sample rate
    }
    
    // Filter pot - per track with pickup
    int potFilterRaw = analogRead(POT_FILTER);
    Track &t = tracks[activeTrack];
    updatePotWithPickup(potFilterRaw, t.filter, t.potFilterLast, t.filterCaught, 0.01, 0.99);
  }
  
  // Status print
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 2000) {
    lastPrint = millis();
    Track &t = tracks[activeTrack];
    Serial.print("[");
    Serial.print(t.name);
    Serial.print(" ");
    Serial.print(t.soundIndex + 1);
    Serial.print("/");
    Serial.print(t.numSounds);
    Serial.print("] Vol:");
    Serial.print((int)(t.volume*500));
    Serial.print("% Pitch:");
    Serial.print(globalPitch, 2);
    Serial.print(" Filter:");
    Serial.println(t.filter, 2);
  }
  
  // Small delay to prevent tight spinning
  delay(1);
}

// ═══════════════════════════════════════════════════════
// CORE 1 SETUP - AUDIO
// ═══════════════════════════════════════════════════════

void setup1() {
  // Wait for Core 0 to initialize serial
  delay(100);
  
  // I2S pins
  pinMode(I2S_BCLK, OUTPUT);
  pinMode(I2S_DATA, OUTPUT);
  digitalWrite(I2S_BCLK, LOW);
  digitalWrite(I2S_DATA, LOW);
  delay(100);

  // SD Card
  if (!SD.begin(SD_CS)) {
    Serial.println("[Core1] ✗ SD failed!");
    while(1) delay(1000);
  }
  Serial.println("[Core1] ✓ SD ready");

  // I2S
  if (!i2s.begin(11025)) {
    Serial.println("[Core1] ✗ I2S failed!");
    while(1) delay(1000);
  }
  Serial.println("[Core1] ✓ I2S ready @ 11025Hz");

  // Load initial sounds
  Serial.println("[Core1] Loading initial sounds...");
  for (int i = 0; i < 3; i++) {
    if (!loadSound(i, 0)) {
      Serial.println("[Core1] ✗ FAILED!");
      while(1) delay(1000);
    }
  }
  
  Serial.println("[Core1] ✓ Audio ready!");
}

// ═══════════════════════════════════════════════════════
// CORE 1 LOOP - AUDIO (SD reading, mixing, I2S output)
// ═══════════════════════════════════════════════════════

void loop1() {
  // Check for pending sound changes from Core 0
  for (int i = 0; i < 3; i++) {
    if (tracks[i].pendingSoundIndex >= 0) {
      loadSound(i, tracks[i].pendingSoundIndex);
    }
  }
  
  // Check for pitch change
  if (pitchChanged) {
    int newSampleRate = (int)(11025 * globalPitch);
    i2s.begin(newSampleRate);
    pitchChanged = false;
  }
  
  // Read audio from all 3 tracks
  int bytesRead1 = tracks[0].wavFile.read((uint8_t*)tracks[0].audioBuffer, 256 * 2);
  int bytesRead2 = tracks[1].wavFile.read((uint8_t*)tracks[1].audioBuffer, 256 * 2);
  int bytesRead3 = tracks[2].wavFile.read((uint8_t*)tracks[2].audioBuffer, 256 * 2);
  
  if (bytesRead1 > 0 && bytesRead2 > 0 && bytesRead3 > 0) {
    int samples = min(min(bytesRead1, bytesRead2), bytesRead3) / 2;
    
    // Cache volatile values locally for this buffer
    float vol0 = tracks[0].volume;
    float vol1 = tracks[1].volume;
    float vol2 = tracks[2].volume;
    float filt0 = tracks[0].filter;
    float filt1 = tracks[1].filter;
    float filt2 = tracks[2].filter;
    float gVol = globalVolume;
    
    for (int i = 0; i < samples; i++) {
      // Track 1 (Insects)
      float input1 = (float)(tracks[0].audioBuffer[i] * vol0);
      filterInsects.low += filt0 * filterInsects.band;
      filterInsects.high = input1 - filterInsects.low - filterInsects.resonance * filterInsects.band;
      filterInsects.band += filt0 * filterInsects.high;
      int16_t filtered1 = (int16_t)filterInsects.low;
      
      // Track 2 (Nature)
      float input2 = (float)(tracks[1].audioBuffer[i] * vol1);
      filterNature.low += filt1 * filterNature.band;
      filterNature.high = input2 - filterNature.low - filterNature.resonance * filterNature.band;
      filterNature.band += filt1 * filterNature.high;
      int16_t filtered2 = (int16_t)filterNature.low;
      
      // Track 3 (Pads)
      float input3 = (float)(tracks[2].audioBuffer[i] * vol2);
      filterPads.low += filt2 * filterPads.band;
      filterPads.high = input3 - filterPads.low - filterPads.resonance * filterPads.band;
      filterPads.band += filt2 * filterPads.high;
      int16_t filtered3 = (int16_t)filterPads.low;
      
      // Mix
      int32_t mixed = (int32_t)filtered1 + (int32_t)filtered2 + (int32_t)filtered3;
      mixed = (int32_t)(mixed * gVol);
      
      // Clamp
      if (mixed > 32767) mixed = 32767;
      if (mixed < -32768) mixed = -32768;
      
      int16_t sample = (int16_t)mixed;
      
      i2s.write(sample);
      i2s.write(sample);
    }
  }
  
  // Loop files when near end
  for (int i = 0; i < 3; i++) {
    if (tracks[i].wavFile.position() >= tracks[i].dataStart + tracks[i].dataSize - 512) {
      tracks[i].wavFile.seek(tracks[i].dataStart);
    }
  }
}
  float volume;
  float filter;
  int soundIndex;
  
  // Value pickup
  int potFilterLast;
  bool filterCaught;
  
  const char* name;
  const char** soundList;
  int numSounds;
};

Track tracks[3] = {
  {File(), 0, 0, {}, 0.1, 1, 0, 0, false, "Insects", insectSounds, numInsectSounds},
  {File(), 0, 0, {}, 0.1, 0.2, 0, 0, false, "Nature",  natureSounds,  numNatureSounds},
  {File(), 0, 0, {}, 0.1, 0.8, 0, 0, false, "Pads",    padSounds,     numPadSounds}
};

int activeTrack = 0;

float globalPitch = 1.0;
float globalVolume = 0.5;

// ═══════════════════════════════════════════════════════
// GLOBAL OBJECTS
// ═══════════════════════════════════════════════════════

I2S i2s(OUTPUT, I2S_BCLK, I2S_DATA);
Rotary rotary(ENC_A, ENC_B);

struct SVF {
  float low = 0.0;
  float band = 0.0;
  float high = 0.0;
  float resonance = 0.2;
};

SVF filterInsects = {0, 0, 0, 0.2};
SVF filterNature = {0, 0, 0, 0.2};
SVF filterPads = {0, 0, 0, 0.2};

unsigned long lastEncoderTime = 0;
bool encoderPressed = false;
unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 200;

// ═══════════════════════════════════════════════════════
// WAV PARSING
// ═══════════════════════════════════════════════════════

bool parseWavHeader(File &file, uint32_t* outDataSize, uint32_t* outDataStart) {
  uint8_t riff[4];
  file.read(riff, 4);
  if (strncmp((char*)riff, "RIFF", 4) != 0) return false;
  
  uint32_t fileSize;
  file.read((uint8_t*)&fileSize, 4);
  
  uint8_t wave[4];
  file.read(wave, 4);
  if (strncmp((char*)wave, "WAVE", 4) != 0) return false;
  
  while (file.available()) {
    uint8_t chunkId[4];
    file.read(chunkId, 4);
    uint32_t chunkSize;
    file.read((uint8_t*)&chunkSize, 4);
    
    if (strncmp((char*)chunkId, "fmt ", 4) == 0) {
      uint16_t audioFormat, numChannels, bitsPerSample;
      uint32_t sampleRate, byteRate;
      uint16_t blockAlign;
      
      file.read((uint8_t*)&audioFormat, 2);
      file.read((uint8_t*)&numChannels, 2);
      file.read((uint8_t*)&sampleRate, 4);
      file.read((uint8_t*)&byteRate, 4);
      file.read((uint8_t*)&blockAlign, 2);
      file.read((uint8_t*)&bitsPerSample, 2);
      
      if (chunkSize > 16) {
        file.seek(file.position() + (chunkSize - 16));
      }
      
    } else if (strncmp((char*)chunkId, "data", 4) == 0) {
      *outDataSize = chunkSize;
      *outDataStart = file.position();
      return true;
      
    } else {
      file.seek(file.position() + chunkSize);
    }
  }
  
  return false;
}

// ═══════════════════════════════════════════════════════
// LOAD SOUND
// ═══════════════════════════════════════════════════════

bool loadSound(int trackIndex, int soundIndex) {
  Track &t = tracks[trackIndex];
  
  // Close previous file
  if (t.wavFile) {
    t.wavFile.close();
  }
  
  // Get filename
  const char* filename = t.soundList[soundIndex];
  
  Serial.print("Loading: ");
  Serial.print(t.name);
  Serial.print(" - ");
  Serial.println(filename);
  
  // Open file
  t.wavFile = SD.open(filename);
  if (!t.wavFile) {
    Serial.println("✗ Failed to open!");
    return false;
  }
  
  // Parse header
  if (!parseWavHeader(t.wavFile, &t.dataSize, &t.dataStart)) {
    Serial.println("✗ Invalid WAV!");
    t.wavFile.close();
    return false;
  }
  
  // Seek to audio data
  t.wavFile.seek(t.dataStart);
  
  t.soundIndex = soundIndex;
  
  Serial.println("✓ Loaded");
  return true;
}

// ═══════════════════════════════════════════════════════
// LED MANAGEMENT
// ═══════════════════════════════════════════════════════

void updateLEDs() {
  digitalWrite(LED_TRACK1, activeTrack == 0 ? HIGH : LOW);
  digitalWrite(LED_TRACK2, activeTrack == 1 ? HIGH : LOW);
  digitalWrite(LED_TRACK3, activeTrack == 2 ? HIGH : LOW);
}

// ═══════════════════════════════════════════════════════
// VALUE PICKUP
// ═══════════════════════════════════════════════════════

void updatePotWithPickup(int potRaw, float &paramValue, int &lastPotValue, bool &caught, float minVal, float maxVal) {
  if (!caught) {
    int currentPotValue = (int)(((paramValue - minVal) / (maxVal - minVal)) * 4095.0);
    if (abs(potRaw - currentPotValue) < 80) {
      caught = true;
      Serial.println("  → Pot caught!");
    }
  }
  
  if (caught) {
    paramValue = minVal + (potRaw / 4095.0) * (maxVal - minVal);
    lastPotValue = potRaw;
  }
}

// ═══════════════════════════════════════════════════════
// ENCODER PROCESSING
// ═══════════════════════════════════════════════════════

void processEncoder() {
  encoderPressed = (digitalRead(ENC_SW) == LOW);
  
  unsigned char result = rotary.process();
  
  if (result == DIR_CW || result == DIR_CCW) {
    
    if (encoderPressed) {
      // VOLUME MODE (constant step)
      float step = 0.003;
      
      if (result == DIR_CW) {
        tracks[activeTrack].volume += step;
      } else {
        tracks[activeTrack].volume -= step;
      }
      tracks[activeTrack].volume = constrain(tracks[activeTrack].volume, 0.0, 0.2);
      
      Serial.print("Volume: ");
      Serial.println(tracks[activeTrack].volume, 3);
      
    } else {
      // SOUND SELECT MODE (one step at a time)
      Track &t = tracks[activeTrack];
      
      int newIndex = t.soundIndex;
      
      if (result == DIR_CW) {
        newIndex++;
      } else {
        newIndex--;
      }
      
      // Wrap around
      if (newIndex < 0) newIndex = t.numSounds - 1;
      if (newIndex >= t.numSounds) newIndex = 0;
      
      if (newIndex != t.soundIndex) {
        loadSound(activeTrack, newIndex);
      }
    }
  }
}

// ═══════════════════════════════════════════════════════
// TRACK BUTTON
// ═══════════════════════════════════════════════════════

void checkTrackButton() {
  if (digitalRead(BTN_TRACK) == LOW) {
    if (millis() - lastButtonPress > debounceDelay) {
      lastButtonPress = millis();
      
      activeTrack = (activeTrack + 1) % 3;
      
      updateLEDs();
      
      Serial.print("\n→ Switched to: ");
      Serial.print(tracks[activeTrack].name);
      Serial.print(" (sound ");
      Serial.print(tracks[activeTrack].soundIndex + 1);
      Serial.print("/");
      Serial.print(tracks[activeTrack].numSounds);
      Serial.println(")");
    }
  }
}

// ═══════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════

void setup() {
  pinMode(I2S_BCLK, OUTPUT);
  pinMode(I2S_DATA, OUTPUT);
  digitalWrite(I2S_BCLK, LOW);
  digitalWrite(I2S_DATA, LOW);
  delay(100);
  
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n═══════════════════════════════════");
  Serial.println("   AMBIENT BOX - NAVIGATION");
  Serial.println("═══════════════════════════════════\n");

  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(BTN_TRACK, INPUT_PULLUP);
  
  pinMode(LED_TRACK1, OUTPUT);
  pinMode(LED_TRACK2, OUTPUT);
  pinMode(LED_TRACK3, OUTPUT);
  
  analogReadResolution(12);
  updateLEDs();
  
  Serial.println("✓ GPIO configured");

  if (!SD.begin(SD_CS)) {
    Serial.println("✗ SD failed!");
    while(1) delay(1000);
  }
  Serial.println("✓ SD ready");

  if (!i2s.begin(11025)) {
    Serial.println("✗ I2S failed!");
    while(1) delay(1000);
  }
  Serial.println("✓ I2S ready @ 11025Hz");

  Serial.println("\nLoading initial sounds...");
  for (int i = 0; i < 3; i++) {
    if (!loadSound(i, 0)) {
      Serial.println("✗ FAILED!");
      while(1) delay(1000);
    }
  }
  
  Serial.println("\n✓ READY!");
  Serial.println("\nControls:");
  Serial.println("  ENCODER turn = Select sound");
  Serial.println("  ENCODER press+turn = Volume");
  Serial.println("  POT 1 = Pitch (global)");
  Serial.println("  POT 2 = Filter");
  Serial.println("  BUTTON = Cycle tracks\n");
}

// ═══════════════════════════════════════════════════════
// MAIN LOOP
// ═══════════════════════════════════════════════════════

void loop() {
  processEncoder();
  checkTrackButton();
  
  // Read pots
    static unsigned long lastPotRead = 0;
    if (millis() - lastPotRead > 50) {
      lastPotRead = millis();
      
      // Volume pot
      int potVolRaw = analogRead(POT_VOLUME);  
      globalVolume = (potVolRaw / 4095.0) * 1;

      // Pitch: global, direct reading (0.5x to 1.0x range)
      int potPitchRaw = analogRead(POT_PITCH);
      float potNormalized = potPitchRaw / 4095.0;
      globalPitch = 0.3 + (potNormalized * 0.7);  // 0.5x à 1.0x
      int newSampleRate = (int)(11025 * globalPitch);
      i2s.begin(newSampleRate);
            
            // Filter: per track, with value pickup
            int potFilterRaw = analogRead(POT_FILTER);
            Track &t = tracks[activeTrack];
            updatePotWithPickup(potFilterRaw, t.filter, t.potFilterLast, t.filterCaught, 0.01, 0.99);
          }
  
  // Status
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 2000) {
    lastPrint = millis();
    Track &t = tracks[activeTrack];
    Serial.print("[");
    Serial.print(t.name);
    Serial.print(" ");
    Serial.print(t.soundIndex + 1);
    Serial.print("/");
    Serial.print(t.numSounds);
    Serial.print("] Vol:");
    Serial.print((int)(t.volume*500));
    Serial.print("% Pitch:");
    Serial.print(globalPitch, 2);
    Serial.print(" Filter:");
    Serial.println(t.filter, 2);
  }
  
  // Read & mix all 3 tracks avec filtrage individuel
  int bytesRead1 = tracks[0].wavFile.read((uint8_t*)tracks[0].audioBuffer, 256 * 2);
  int bytesRead2 = tracks[1].wavFile.read((uint8_t*)tracks[1].audioBuffer, 256 * 2);
  int bytesRead3 = tracks[2].wavFile.read((uint8_t*)tracks[2].audioBuffer, 256 * 2);
  
  if (bytesRead1 > 0 && bytesRead2 > 0 && bytesRead3 > 0) {
    int samples = min(min(bytesRead1, bytesRead2), bytesRead3) / 2;
    
    for (int i = 0; i < samples; i++) {
      // Filtrer chaque track séparément
      
      // Track 1 (Insects)
      float input1 = (float)(tracks[0].audioBuffer[i] * tracks[0].volume);
      filterInsects.low += tracks[0].filter * filterInsects.band;
      filterInsects.high = input1 - filterInsects.low - filterInsects.resonance * filterInsects.band;
      filterInsects.band += tracks[0].filter * filterInsects.high;
      int16_t filtered1 = (int16_t)filterInsects.low;
      
      // Track 2 (Nature)
      float input2 = (float)(tracks[1].audioBuffer[i] * tracks[1].volume);
      filterNature.low += tracks[1].filter * filterNature.band;
      filterNature.high = input2 - filterNature.low - filterNature.resonance * filterNature.band;
      filterNature.band += tracks[1].filter * filterNature.high;
      int16_t filtered2 = (int16_t)filterNature.low;
      
      // Track 3 (Pads)
      float input3 = (float)(tracks[2].audioBuffer[i] * tracks[2].volume);
      filterPads.low += tracks[2].filter * filterPads.band;
      filterPads.high = input3 - filterPads.low - filterPads.resonance * filterPads.band;
      filterPads.band += tracks[2].filter * filterPads.high;
      int16_t filtered3 = (int16_t)filterPads.low;
      
      // Mixer les 3 tracks filtrées
      int32_t mixed = (int32_t)filtered1 + (int32_t)filtered2 + (int32_t)filtered3;

      mixed = (int32_t)(mixed * globalVolume);
      
      // Clamp
      if (mixed > 32767) mixed = 32767;
      if (mixed < -32768) mixed = -32768;
      
      int16_t sample = (int16_t)mixed;
      
      i2s.write(sample);
      i2s.write(sample);
    }
  }
  
  // Loop files
  for (int i = 0; i < 3; i++) {
    if (tracks[i].wavFile.position() >= tracks[i].dataStart + tracks[i].dataSize - 512) {
      tracks[i].wavFile.seek(tracks[i].dataStart);
    }
  }
}
