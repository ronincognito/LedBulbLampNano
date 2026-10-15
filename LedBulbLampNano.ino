#include <math.h>
#include <EEPROM.h>
//
//
// Despite the file name, I installed the code on an Arduino Nano clone, using a LGT8F328P - use lgt8fx board package, with a 32 Mhz internal clock.
// So make sure to select that board in the board manager of the Arduino IDE.
//
//
// Pin numbers for connected lights and buttons
const int touchPinDim = 4;
const int touchPinMode = 2;
// Number of led lights we're controlling
const uint8_t NUM_LEDS = 2;
const int pwmPins[NUM_LEDS] = {6, 9};

// Max brightness settings
const int dutySteps[5]   = {32, 56, 88, 128, 255};
int stepIndex = 2; // index to selected brightness in dutySteps
int maxBrightness;

// Lighting update functions for each mode
int lightMode = 1;  // index to selected lighting mode
void stackedFade();
void waveFlow();
void flipFlop();
void candle();      // forward declarations of lighting mode functions
void staticLights();
void heartBeat();
void (*lightModes[])() = {
    nullptr,      // index 0 unused, so mode numbers stay 1-based
    stackedFade,
    waveFlow,
    flipFlop,
    staticLights,
    heartBeat,
    candle
};
const uint8_t NUM_MODES = sizeof(lightModes) / sizeof(lightModes[0]) - 1; // nr of update modes

// other vars
bool start = true;
float light[NUM_LEDS] = {1, 1}; // initial lighting values (0..1): full configured brightness until the first mode starts

// Timing parameters
const unsigned long longPressTime = 1000; // ms
unsigned long delayTime;
int speedTime = 0; // used to set animation speed

// AUTO mode
bool autoMode = false;
unsigned long autoModeTimer = 0;
const unsigned long autoModeInterval = 30000; // 30 seconds
// feedback of auto mode via flashing
bool autoFlashActive = false;
unsigned long autoFlashStart = 0;

// Button handling
struct Button {
  int pin;
  bool lastState;
  bool longPressTriggered;
  unsigned long pressStartTime;
};
Button btnDim  = {touchPinDim, LOW, false, 0};
Button btnMode = {touchPinMode, LOW, false, 0};

// brightness continous dimming
bool dimmingActive = false;
int currentBrightness;   // actual PWM max value (not step-based)
int dimDirection = 1;    // +1 up, -1 down
unsigned long dimTimer = 0;
const unsigned long dimInterval = 20;   // speed of dimming
bool dimPause = false;
unsigned long dimPauseStart = 0;
const unsigned long dimPauseTime = 500;
unsigned long dimReleaseStart = 0;        // when the dim button first read LOW while dimming
const unsigned long dimReleaseTime = 30;  // it must stay LOW this long to stop dimming (debounce)
const int minBrightness = dutySteps[0];
const int maxBrightnessLimit = dutySteps[4]; // highest dutySteps

// Non-volatile storage of mode and brightness
// A setting is only written after it has been unchanged for nvmSaveDelay,
// and only if it differs from what is already stored (EEPROM.update).
const int EEPROM_ADDR_MODE       = 0;   // 1 byte: 0 = AUTO, 1..NUM_MODES = fixed lightMode
const int EEPROM_ADDR_BRIGHTNESS = 1;   // 1 byte: stepIndex (0..4)
const uint8_t MODE_AUTO = 0;            // stored mode value meaning "AUTO"
const unsigned long nvmSaveDelay = 10000; // ms of stability before saving
uint8_t savedMode;                       // what is currently in NVM
uint8_t savedStep;
uint8_t pendingMode;                     // latest value seen, waiting for the timeout
uint8_t pendingStep;
unsigned long modeChangeTime = 0;        // when pendingMode last changed
unsigned long stepChangeTime = 0;        // when pendingStep last changed

void setup() {
  Serial.begin(115200);
  pinMode(touchPinDim, INPUT); // pins connected to touch inputs - momentary button HIGH when active
  pinMode(touchPinMode, INPUT); 
  for (int i = 0; i < NUM_LEDS; i++) pinMode(pwmPins[i], OUTPUT);
  loadSettings(); // override defaults with stored values, if valid
  currentBrightness = dutySteps[stepIndex];
  maxBrightness = currentBrightness;
  initNvmTracking();
  updateLights(); // show the initial brightness for all leds
}

void loop() {
  //check for button presses
  handleButton(btnDim,  onShortPressDim,  onLongPressDim);
  if (!dimmingActive) {
    handleButton(btnMode, onShortPressMode, onLongPressMode);
  }

  // AUTO mode handling
  if (autoMode && (millis() - autoModeTimer >= autoModeInterval)) {
    autoModeTimer = millis();
    lightMode = (lightMode % NUM_MODES) + 1;
    start = true;
    Serial.println("AUTO -> next mode");
  }
  handleDimming();
  handleAutoFlash();    
  handleNvmSave();

  // update the lights every so many millisecs, but first check for autoflash 
  if (!autoFlashActive) {
    if (((millis() - delayTime) > speedTime) || start) {
      lightModes[lightMode]();
      delayTime = millis();
      updateLights();
    }
  }
}

// ---------- Non-volatile storage ----------

void loadSettings() {
  uint8_t m = EEPROM.read(EEPROM_ADDR_MODE);
  uint8_t b = EEPROM.read(EEPROM_ADDR_BRIGHTNESS);

  // Blank (0xFF) or corrupt values fail these checks, so defaults are kept
  if (m == MODE_AUTO) {
    autoMode = true;          // resume AUTO, always starting with the first mode
    autoModeTimer = millis();
    lightMode = 1;
  } else if (m <= NUM_MODES) {
    lightMode = m;
  }
  if (b < 5) {
    stepIndex = b;
  }
  Serial.print("Loaded from NVM: mode ");
  if (autoMode) Serial.print("AUTO");
  else Serial.print(lightMode);
  Serial.print(", brightness step ");
  Serial.println(stepIndex);
}

// Value that represents the current mode in NVM: AUTO is a mode of its own,
// so the 30 s cycling through the modes never causes a write.
uint8_t currentModeCode() {
  return autoMode ? MODE_AUTO : lightMode;
}

void initNvmTracking() {
  savedMode = EEPROM.read(EEPROM_ADDR_MODE);
  savedStep = EEPROM.read(EEPROM_ADDR_BRIGHTNESS);
  pendingMode = currentModeCode();
  pendingStep = stepIndex;
  modeChangeTime = stepChangeTime = millis();
}

void handleNvmSave() {
  unsigned long now = millis();

  uint8_t mode = currentModeCode();
  if (mode != pendingMode) {
    pendingMode = mode;
    modeChangeTime = now;           // restart the timeout
  } else if (pendingMode != savedMode && now - modeChangeTime >= nvmSaveDelay) {
    EEPROM.update(EEPROM_ADDR_MODE, pendingMode);
    savedMode = pendingMode;
    Serial.print("Saved mode to NVM: ");
    Serial.println(savedMode);
  }

  if (stepIndex != pendingStep) {
    pendingStep = stepIndex;
    stepChangeTime = now;           // restart the timeout
  } else if (pendingStep != savedStep && now - stepChangeTime >= nvmSaveDelay) {
    EEPROM.update(EEPROM_ADDR_BRIGHTNESS, pendingStep);
    savedStep = pendingStep;
    Serial.print("Saved brightness step to NVM: ");
    Serial.println(savedStep);
  }
}

// ---------- Lighting modes ----------

void stackedFade() {
  static int currentLED = 0;
  static int direction = 1;       // 1 = fade in, -1 = fade out
  static float brightness[NUM_LEDS] = {0,0};
  static unsigned long lastUpdate = 0;
  const float fadeStep = 0.02;    // step for each update
  const unsigned long stepDelay = 30; // ms between brightness steps

  if (start) {
    start = false;
    printLightMode();
    speedTime = stepDelay;
    currentLED = 0;
    direction = 1;
    for (int i=0; i<NUM_LEDS; i++) brightness[i]=0;
    lastUpdate = millis();
  }

  unsigned long now = millis();
  if (now - lastUpdate < stepDelay) return;
  lastUpdate = now;

  // Update current LED
  brightness[currentLED] += fadeStep * direction;
  if (brightness[currentLED] >= 1.0) {
    brightness[currentLED] = 1.0;
    direction = -1;  // start fading out after full brightness
  }
  if (brightness[currentLED] <= 0.0) {
    brightness[currentLED] = 0.0;
    direction = 1;   // start fading in again
    currentLED += 1; // move to next LED
    if (currentLED >= NUM_LEDS) currentLED = 0; // loop back
  }

  // Apply to all LEDs
  for (int i = 0; i < NUM_LEDS; i++) {
    light[i] = brightness[i];
  }
}

void waveFlow() {
    static float phase[NUM_LEDS] = {0, 1.5};
    if (start) { start = false; printLightMode(); speedTime = 20; }

    float t = millis() / 500.0; // controls speed
    for (int i = 0; i < NUM_LEDS; i++) {
        light[i] = 0.5 + 0.5 * sin(t + phase[i]);
    }
}

void candle() {
    // --- Tuning knobs ---
    const float FLAME_BASE  = 0.35f;
    const float FLAME_RANGE = 0.55f;
    const float DRIFT_SPEED = 0.05f;

    static float   phase[NUM_LEDS]     = { 0.0f, 2.5f, };  // offset so they start different
    static float   target[NUM_LEDS]    = { FLAME_BASE, FLAME_BASE };
    static float   current[NUM_LEDS]   = { FLAME_BASE, FLAME_BASE };
    static uint8_t holdCount[NUM_LEDS] = { 0, 4};  // offset so they don't jump in sync

    if (start) {
        start = false;
        printLightMode();
        speedTime = 50;
    }

    for (int i = 0; i < NUM_LEDS; i++) {
        // --- Slow organic drift ---
        phase[i] += DRIFT_SPEED;
        if (phase[i] > TWO_PI) phase[i] -= TWO_PI;
        float baseBrightness = FLAME_BASE + sin(phase[i]) * (FLAME_RANGE * 0.5f);

        // --- Random target jumps ---
        if (holdCount[i] == 0) {
            float offset = random(-100, 100) / 100.0f * FLAME_RANGE;
            target[i] = constrain(baseBrightness + offset,
                                  FLAME_BASE - FLAME_RANGE,
                                  FLAME_BASE + FLAME_RANGE);
            holdCount[i] = random(2, 6);
        } else {
            holdCount[i]--;
        }

        // --- Smooth current toward target ---
        current[i] += (target[i] - current[i]) / 8.0f;
        light[i] = constrain(current[i], 0.0f, 1.0f);
    }
}

void heartBeat() {
  static unsigned long startTime = 0;

  if (start) {
    start = false;
    printLightMode();
    speedTime = 10; // smooth updates
    startTime = millis();
  }

  unsigned long now = millis();
  float cycleDuration = 4*1200.0; // total heartbeat cycle in ms

  float t = fmod((now - startTime), cycleDuration) / cycleDuration;
  float brightness = 0;

  if (t < 0.10) {
    //  First peak (fast rise)
    float x = t / 0.10; // from 0.0 to 0.10
    brightness = x * x * x; // sharp rise
  } 
  else if (t < 0.15) {
    // drop after first peak
    float x = (t - 0.10) / 0.05; // from 0.10 to 0.15
    brightness = 1.0 - x;
  } 
  else if (t < 0.25) {
    //  Second peak
    float x = (t - 0.15) / 0.1; // from 0.15 to 0.25
    brightness = x * x; // slightly softer
  } 
  else if (t < 0.70) {
    //  Long fade out
    float x = (t - 0.25) / 0.45;
    brightness = (1.0 - x);
    brightness = brightness * brightness* brightness; // smooth tail
  } 
  else {
    // ⏸ Pause
    brightness = 0;
  }

  //  Apply gamma correction
  float corrected = brightness * brightness; // simple gamma ≈ 2

  // Apply to all LEDs
  for (int i = 0; i < NUM_LEDS; i++) {
    light[i] = corrected;
  }
}

void flipFlop() {
  // Both LEDs alternate: one does a sharp rise-and-hold, the other a smooth cosine decay,
  // then they swap roles every cycleDuration.
  static unsigned long startTime = 0;
  static bool mode1;
  float cycleDuration = 4000; // total cycle in ms

  if (start) {
    start = false;
    printLightMode();
    speedTime = 10; // smooth updates
    startTime = millis();
    light[0] = 0;
    light[1] = 1;
    mode1 = true;
  }

  unsigned long now = millis();
  if ((now - startTime) > cycleDuration) {
    mode1 = !mode1; // toggle mode every cycleDuration
    startTime = millis();
  }

  float t = (now - startTime) / cycleDuration;

  float y = (8*t-1);
  float brightUp = (t<0.125) ? (1-y*y): 1;
  float brightDown = (t<0.5) ? (cos(2 * M_PI*t)+1)/2 : 0;

  // update brightness: one LED fades up, the other fades down.
  light[0] = mode1 ?  brightUp : brightDown;
  light[1] = mode1 ?  brightDown : brightUp;
}

void staticLights() {
  // basic template for a very simple lighting mode
  int i;
  if (start) {
    start = false;    
    printLightMode();
    speedTime = 100; // set animation speed for this mode
    for ( i = 0; i < NUM_LEDS; i++) light[i]=1; // init all four light values
  }
  // that is all for this very simple mode
}

void handleButton(Button &btn, void (*onShort)(), void (*onLong)()) {
  bool currentState = digitalRead(btn.pin);

  // While continuous dimming runs, the dim button only decides when to stop:
  // dimming ends once the button has read LOW for dimReleaseTime (debounce).
  // A bounce back to HIGH inside that window keeps dimming going.
  if (btn.pin == touchPinDim && dimmingActive) {
    if (currentState == HIGH) {
      dimReleaseStart = 0;
    } else if (dimReleaseStart == 0) {
      dimReleaseStart = millis();
    } else if (millis() - dimReleaseStart >= dimReleaseTime) {
      dimmingActive = false;
      dimReleaseStart = 0;
      Serial.print("Stop dimming at ");
      Serial.println(maxBrightness);
    }
    btn.lastState = currentState;
    return;
  }

  // check for press
  if (currentState == HIGH && btn.lastState == LOW) {
    btn.pressStartTime = millis();
    btn.longPressTriggered = false;
  }

  // check for release (a long press has already done its job)
  if (currentState == LOW && btn.lastState == HIGH && !btn.longPressTriggered) {
    onShort();
  }

  // check for long press
  if (currentState == HIGH && !btn.longPressTriggered) {
    if (millis() - btn.pressStartTime >= longPressTime) {
      btn.longPressTriggered = true;
      onLong();
    }
  }

  btn.lastState = currentState;
}

void onShortPressDim() {
  stepIndex = (stepIndex + 1) % 5;
  currentBrightness = dutySteps[stepIndex];
  maxBrightness = currentBrightness;
  Serial.print("Short press dim -> brightness: ");
  Serial.println(currentBrightness);
}

void onLongPressDim() {
  dimmingActive = true;
  dimDirection = 1;
  dimPause = false;
  dimReleaseStart = 0;
  dimTimer = millis();
  Serial.println("Long press DIM -> start continuous dimming");
}

void onShortPressMode() {
  Serial.print("Short press MODE: ");
  // If in AUTO → exit AUTO and go to next mode
  if (autoMode) {
    autoMode = false;
    Serial.println("Exit AUTO mode");
  }
  lightMode = (lightMode % NUM_MODES) + 1;
  start = true;
}

void onLongPressMode() {
  autoMode = true;
  autoModeTimer = millis();
  autoFlashActive = true;
  autoFlashStart = millis();
  Serial.println("Enter AUTO mode");
}

void handleAutoFlash() {
  // shows a brief flashing pattern when AUTO becomes active
  if (!autoFlashActive) return;
  unsigned long t = millis() - autoFlashStart;
  if (t < 150 || (t > 300 && t < 450)|| (t > 600 && t < 750)) {
    // ON (several short flashes)
    for (int i = 0; i < NUM_LEDS; i++) {
      light[i] = 1.0;
    }
  } else if (t < 900) {
    // OFF
    for (int i = 0; i < NUM_LEDS; i++) {
      light[i] = 0.0;
    }
  } else {
    autoFlashActive = false;
    start = true; // restart current mode cleanly
  }
  updateLights(); // override normal animation
}

void handleDimming() {
  if (!dimmingActive) return;
  unsigned long now = millis();
  // Handle pause at limits
  if (dimPause) {
    if (now - dimPauseStart >= dimPauseTime) {
      dimPause = false;
      dimDirection *= -1; // reverse direction
    } else {
      return;
    }
  }
  // Step brightness
  if (now - dimTimer >= dimInterval) {
    dimTimer = now;
    currentBrightness += dimDirection;
    if (currentBrightness >= maxBrightnessLimit) {
      currentBrightness = maxBrightnessLimit;
      dimPause = true;
      dimPauseStart = now;
    }
    if (currentBrightness <= minBrightness) {
      currentBrightness = minBrightness;
      dimPause = true;
      dimPauseStart = now;
    }
    maxBrightness = currentBrightness;
  }
}

void updateLights() {
  int pwmValue;
  for (int i = 0; i < NUM_LEDS; i++)  {
    pwmValue = constrain(light[i], 0.0f, 1.0f) * maxBrightness;
    analogWrite(pwmPins[i],  (pwmValue == 0) ? 1 : pwmValue); // min pwm value should be 1 so lights are never fully off
  }
}

void printLightMode() {
  Serial.print("Mode: ");
  Serial.print(lightMode);
  Serial.print("/");
  Serial.println(NUM_MODES);
}
