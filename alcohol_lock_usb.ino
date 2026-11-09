// ============================================================================
// Alcohol Detection Ignition Lock - ESP32 + MQ-3
// USB Telemetry Edition with Sobriety Assessment, Drunk Counter & Lockout
// 120 s warm-up, tunable margins (MARGIN), strike counter (STRIKES / UNLOCK)
//
// Circuit Wiring:
//   MQ-3 AO  ---> [10k] ---> GPIO 34 (ADC1_CH6) ---> [10k] ---> GND
//                 (Voltage divider: 5V max becomes ~2.5V, safe for ESP32 ADC)
//   Green LED ---> [220-330R] ---> GPIO 26 ---> GND (Ignition Allowed / Sober)
//   Red LED   ---> [220-330R] ---> GPIO 27 ---> GND (Ignition Blocked / Drunk)
//   Relay IN  ---> GPIO 14 (Ignition Starter Interlock)
//   ESP32 USB ---> PC / Laptop USB Port (115200 Baud)
//
// Serial commands: CAL, MARGIN <on> <off>, STRIKES <1-20>, UNLOCK, TEST, PING
// ============================================================================

#include <Preferences.h>

// ---------- Hardware Pins ----------
const int PIN_MQ3   = 34;   // MQ-3 analog input via 10k/10k voltage divider
const int PIN_GREEN = 26;   // Green status LED (Sober / Ignition Allowed)
const int PIN_RED   = 27;   // Red warning LED (Drunk / Interlock Blocked)
const int PIN_RELAY = 14;   // Ignition interlock relay

// Set true if your relay triggers when the pin is LOW (active-low module)
const bool RELAY_ACTIVE_LOW = false;

// ---------- Interlock Calibration & Timing Settings ----------
const unsigned long WARMUP_MS        = 120000; // sensor preheat & stabilization
const unsigned long HOLD_MS          = 10000;  // lockout hold after alcohol clears
const int           ADC_MIN          = 20;     // Sensor disconnect or short to GND
const int           ADC_MAX          = 4090;   // Sensor rail short or open divider
const int           BASELINE_SAMPLES = 80;     // Clean-air baseline sample count
const unsigned long FAULT_RECOVER_MS = 2000;   // Debounce time to recover from fault
const unsigned long TELEMETRY_MS     = 250;    // JSON over USB every 250 ms

// Trip / release margins in ADC counts above the clean-air baseline.
// Tuned to the test source (sanitiser / alcohol swab); NOT calibrated against a reference breathalyser.
// Change at run time with:  MARGIN <on> <off>   (resets to these defaults on reboot)
int marginOn  = 110;   // Trip threshold: rise above baseline => DRUNK
int marginOff = 65;    // Release threshold (hysteresis); also needs the HOLD_MS hold

// ---------- Strike counter (saved to flash) ----------
Preferences prefs;
int drunkCount = 0;    // number of separate drunk detections
int maxStrikes = 3;    // hard lockout once drunkCount reaches this

// ---------- State Machine ----------
enum State { WARMUP, SOBER, ALCOHOL, FAULT, LOCKOUT };
State state = WARMUP;

int baseline = 0;
bool baselineValid = false;
int smoothed = 0;
int rawReading = 0;

unsigned long bootTime = 0;
unsigned long lastSample = 0;
unsigned long lastHighTime = 0;
unsigned long faultRecoverStart = 0;
unsigned long lastTelemetry = 0;

String inputBuffer = "";

void saveStrikes() {
  prefs.putInt("count", drunkCount);
  prefs.putInt("max", maxStrikes);
}

// ---------- Relay & Indicator Outputs ----------
void setRelay(bool enableIgnition) {
  // Fail-safe: relay is only active when ignition is explicitly permitted (sober)
  digitalWrite(PIN_RELAY, (enableIgnition ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

const char* stateName() {
  switch (state) {
    case WARMUP:  return "WARMING UP";
    case SOBER:   return "SOBER - ignition allowed";
    case ALCOHOL: return "DRUNK / INTOXICATED - ignition blocked";
    case FAULT:   return "SENSOR FAULT - ignition blocked";
    case LOCKOUT: return "LOCKED OUT - too many drunk detections";
    default:      return "UNKNOWN";
  }
}

const char* stateCode() {
  switch (state) {
    case WARMUP:  return "WARMUP";
    case SOBER:   return "SOBER";
    case ALCOHOL: return "DRUNK";
    case FAULT:   return "FAULT";
    case LOCKOUT: return "LOCKOUT";
    default:      return "FAULT";
  }
}

void setOutputs() {
  bool ok = (state == SOBER);
  setRelay(ok);                       // Fail-safe: only SOBER energizes the relay
  digitalWrite(PIN_GREEN, ok ? HIGH : LOW);

  if (state == LOCKOUT) {
    digitalWrite(PIN_RED, (millis() / 150) % 2);   // fast flash
  } else if (state == FAULT || state == WARMUP) {
    digitalWrite(PIN_RED, (millis() / 400) % 2);   // slow flash
  } else {
    digitalWrite(PIN_RED, ok ? LOW : HIGH);
  }
}

// ---------- Clean-Air Calibration ----------
bool takeBaseline() {
  long sum = 0;
  for (int i = 0; i < BASELINE_SAMPLES; i++) {
    sum += analogRead(PIN_MQ3);
    delay(10);
  }
  baseline = sum / BASELINE_SAMPLES;
  baselineValid = (baseline > ADC_MIN && baseline < ADC_MAX);

  if (baselineValid) {
    smoothed = baseline;
    Serial.printf("{\"info\":\"Clean air baseline calibrated\",\"baseline\":%d}\n", baseline);
  } else {
    Serial.println("{\"warning\":\"Baseline invalid - check sensor wiring\"}");
  }
  return baselineValid;
}

// ---------- USB JSON Telemetry Stream ----------
void sendTelemetryJSON() {
  unsigned long now = millis();
  int diff = smoothed - baseline;
  bool isDrunk = (state == ALCOHOL);
  bool ignitionAllowed = (state == SOBER);

  // Estimated BAC %: linear, uncalibrated. marginOn counts maps to 0.080%.
  float estBac = 0.0f;
  if (diff > 0) {
    estBac = ((float)diff / (float)marginOn) * 0.080f;
  }

  int warmupSec = 0;
  if (state == WARMUP) {
    unsigned long elapsed = now - bootTime;
    if (elapsed < WARMUP_MS) {
      warmupSec = (WARMUP_MS - elapsed + 999) / 1000;
    }
  }

  float holdSec = 0.0;
  if (state == ALCOHOL && diff < marginOff) {
    unsigned long elapsedHold = now - lastHighTime;
    if (elapsedHold < HOLD_MS) {
      holdSec = (float)(HOLD_MS - elapsedHold) / 1000.0f;
    }
  }

  Serial.print("{\"state\":\"");
  Serial.print(stateName());
  Serial.print("\",\"code\":\"");
  Serial.print(stateCode());
  Serial.print("\",\"is_drunk\":");
  Serial.print(isDrunk ? "true" : "false");
  Serial.print(",\"est_bac\":");
  Serial.print(estBac, 3);
  Serial.print(",\"raw\":");
  Serial.print(rawReading);
  Serial.print(",\"reading\":");
  Serial.print(smoothed);
  Serial.print(",\"baseline\":");
  Serial.print(baseline);
  Serial.print(",\"diff\":");
  Serial.print(diff);
  Serial.print(",\"margin_on\":");
  Serial.print(marginOn);
  Serial.print(",\"margin_off\":");
  Serial.print(marginOff);
  Serial.print(",\"ignition_allowed\":");
  Serial.print(ignitionAllowed ? "true" : "false");
  Serial.print(",\"warmup_sec\":");
  Serial.print(warmupSec);
  Serial.print(",\"hold_sec\":");
  Serial.print(holdSec, 1);
  Serial.print(",\"drunk_count\":");
  Serial.print(drunkCount);
  Serial.print(",\"max_strikes\":");
  Serial.print(maxStrikes);
  Serial.print(",\"fault\":");
  Serial.print(state == FAULT ? "true" : "false");
  Serial.println("}");
}

// ---------- Incoming Serial Command Parser ----------
void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "CAL" || cmd == "CALIBRATE") {
    // Only in a clean SOBER state, so it cannot be used to unlock while alcohol is present
    if (state != SOBER) {
      Serial.println("{\"warning\":\"CAL refused: only allowed in SOBER state\"}");
      return;
    }
    Serial.println("{\"action\":\"Recalibrating clean air reference...\"}");
    state = takeBaseline() ? SOBER : FAULT;

  } else if (cmd.startsWith("MARGIN")) {
    int a = 0, b = 0;
    if (sscanf(cmd.c_str(), "MARGIN %d %d", &a, &b) == 2 && a >= 20 && b >= 5 && b < a) {
      marginOn = a;
      marginOff = b;
      Serial.printf("{\"action\":\"Margins updated\",\"margin_on\":%d,\"margin_off\":%d}\n", marginOn, marginOff);
    } else {
      Serial.println("{\"warning\":\"Usage: MARGIN <on> <off> with on >= 20 and 5 <= off < on\"}");
    }

  } else if (cmd.startsWith("STRIKES")) {
    int n = 0;
    if (sscanf(cmd.c_str(), "STRIKES %d", &n) == 1 && n >= 1 && n <= 20) {
      maxStrikes = n;
      saveStrikes();
      Serial.printf("{\"action\":\"Strike limit updated\",\"max_strikes\":%d}\n", maxStrikes);
    } else {
      Serial.println("{\"warning\":\"Usage: STRIKES <1-20>\"}");
    }

  } else if (cmd == "UNLOCK") {
    drunkCount = 0;
    saveStrikes();
    if (state == LOCKOUT) {
      if (millis() - bootTime < WARMUP_MS) {
        state = WARMUP;                      // not warmed up yet (e.g. rebooted while locked)
      } else {
        state = takeBaseline() ? SOBER : FAULT;
      }
    }
    Serial.println("{\"action\":\"Unlocked, drunk count reset\"}");

  } else if (cmd == "TEST") {
    Serial.println("{\"action\":\"Self-test initiated...\"}");
    digitalWrite(PIN_GREEN, HIGH);
    digitalWrite(PIN_RED, HIGH);
    delay(500);
    digitalWrite(PIN_GREEN, LOW);
    digitalWrite(PIN_RED, LOW);

  } else if (cmd == "PING") {
    Serial.println("{\"pong\":true}");

  } else if (cmd.length() > 0) {
    Serial.printf("{\"unknown_command\":\"%s\"}\n", cmd.c_str());
  }
}

void processSerialInput() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputBuffer.length() > 0) {
        handleCommand(inputBuffer);
        inputBuffer = "";
      }
    } else if (inputBuffer.length() < 64) {
      inputBuffer += c;
    }
  }
}

// ---------- Arduino Setup ----------
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(PIN_MQ3, INPUT);
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);

  setRelay(false);                            // Fail-safe: start with ignition blocked
  analogReadResolution(12);                   // 12-bit ADC (0 - 4095)
  analogSetPinAttenuation(PIN_MQ3, ADC_6db);  // ~3.3V full scale

  bootTime = millis();

  // Restore strike counter so a reboot cannot clear a lockout
  prefs.begin("interlock", false);
  drunkCount = prefs.getInt("count", 0);
  maxStrikes = prefs.getInt("max", 3);
  if (drunkCount >= maxStrikes) state = LOCKOUT;

  Serial.println("\n{\"status\":\"BOOT_COMPLETE\",\"device\":\"ESP32_ALCOHOL_LOCK\",\"baud\":115200}");
  if (state == LOCKOUT) {
    Serial.println("{\"warning\":\"Locked out from previous drunk detections - send UNLOCK to clear\"}");
  } else {
    Serial.println("{\"info\":\"Sensor preheating... Keep in clean ambient air.\"}");
  }
}

// ---------- Main Loop ----------
void loop() {
  unsigned long now = millis();

  processSerialInput();

  if (now - lastSample >= 100) {
    lastSample = now;
    rawReading = analogRead(PIN_MQ3);

    // Exponential moving average filter
    smoothed = (smoothed == 0) ? rawReading : (smoothed * 9 + rawReading) / 10;

    bool faulty = (rawReading < ADC_MIN || rawReading > ADC_MAX);

    if (state == WARMUP) {
      if (now - bootTime >= WARMUP_MS) {
        if (takeBaseline()) {
          state = SOBER;
          Serial.println("{\"status\":\"WARMUP_COMPLETE\",\"state\":\"SOBER\"}");
        } else {
          state = FAULT;
          faultRecoverStart = 0;
        }
      }
    } else if (state == LOCKOUT) {
      // Stay locked until the UNLOCK command
    } else if (faulty) {
      state = FAULT;
      faultRecoverStart = 0;
    } else {
      int diff = smoothed - baseline;

      if (diff > marginOn) {
        if (state != ALCOHOL) {              // new detection (rising edge), counted once
          drunkCount++;
          saveStrikes();
          Serial.printf("{\"event\":\"DRUNK_DETECTED\",\"count\":%d,\"limit\":%d}\n", drunkCount, maxStrikes);
        }
        lastHighTime = now;
        state = (drunkCount >= maxStrikes) ? LOCKOUT : ALCOHOL;
        if (state == LOCKOUT) {
          Serial.println("{\"status\":\"LOCKOUT\",\"info\":\"Strike limit reached - ignition locked\"}");
        }
      } else if (state == ALCOHOL) {
        // Hysteresis release: below marginOff for the full HOLD_MS
        if (diff < marginOff && (now - lastHighTime >= HOLD_MS)) {
          state = SOBER;
          Serial.println("{\"status\":\"HOLD_EXPIRED\",\"state\":\"SOBER\"}");
        }
      } else if (state == FAULT) {
        // Debounce sensor recovery from fault
        if (faultRecoverStart == 0) {
          faultRecoverStart = now;
        } else if (now - faultRecoverStart >= FAULT_RECOVER_MS) {
          if (baselineValid) {
            state = SOBER;
          } else {
            takeBaseline();
          }
          faultRecoverStart = 0;
        }
      }
    }

    setOutputs();
  }

  if (now - lastTelemetry >= TELEMETRY_MS) {
    lastTelemetry = now;
    sendTelemetryJSON();
  }
}
