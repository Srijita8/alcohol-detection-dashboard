// ============================================================================
// Alcohol Detection Ignition Lock - ESP32 + MQ-3
// USB Telemetry Edition with Sobriety Assessment & Drunk Detection
//
// Circuit Wiring:
//   MQ-3 AO  ---> [10k] ---> GPIO 34 (ADC1_CH6) ---> [10k] ---> GND
//                 (Voltage divider: 5V max becomes ~2.5V, safe for ESP32 ADC)
//   Green LED ---> [220-330R] ---> GPIO 26 ---> GND (Ignition Allowed / Sober)
//   Red LED   ---> [220-330R] ---> GPIO 27 ---> GND (Ignition Blocked / Drunk)
//   Relay IN  ---> GPIO 14 (Ignition Starter Interlock)
//   ESP32 USB ---> PC / Laptop USB Port (115200 Baud)
// ============================================================================

// ---------- Hardware Pins ----------
const int PIN_MQ3   = 34;   // MQ-3 analog input via 10k/10k voltage divider
const int PIN_GREEN = 26;   // Green status LED (Sober / Ignition Allowed)
const int PIN_RED   = 27;   // Red warning LED (Drunk / Interlock Blocked)
const int PIN_RELAY = 14;   // Ignition interlock relay

// Relay configuration:
// Set true if your relay triggers when the pin is LOW (active-low module)
const bool RELAY_ACTIVE_LOW = false;

// ---------- Interlock Calibration & Timing Settings ----------
const unsigned long WARMUP_MS        = 30000; // 30s sensor preheat & stabilization
const int           MARGIN_ON        = 100;   // Trip threshold: +400 counts above baseline => DRUNK (~0.08% BAC)
const int           MARGIN_OFF       = 60;   // Reset threshold: +250 counts above baseline (hysteresis ~0.05% BAC)
const unsigned long HOLD_MS          = 10000; // 10s mandatory lockout hold after alcohol clears
const int           ADC_MIN          = 20;    // Sensor disconnect or short to GND
const int           ADC_MAX          = 4090;  // Sensor rail short or open divider
const int           BASELINE_SAMPLES = 80;    // Clean-air baseline sample count
const unsigned long FAULT_RECOVER_MS = 2000;  // Debounce time to recover from fault
const unsigned long TELEMETRY_MS     = 250;   // Send JSON over USB every 250ms (4 updates/sec)

// ---------- State Machine ----------
enum State { WARMUP, SOBER, ALCOHOL, FAULT };
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

// Buffer for incoming serial commands from Web Serial Dashboard
String inputBuffer = "";

// ---------- Relay & Indicator Outputs ----------
void setRelay(bool enableIgnition) {
  // Fail-safe logic: relay is only active when ignition is explicitly permitted (sober)
  digitalWrite(PIN_RELAY, (enableIgnition ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

const char* stateName() {
  switch (state) {
    case WARMUP:  return "WARMING UP";
    case SOBER:   return "SOBER - ignition allowed";
    case ALCOHOL: return "DRUNK / INTOXICATED - ignition blocked";
    case FAULT:   return "SENSOR FAULT - ignition blocked";
    default:      return "UNKNOWN";
  }
}

const char* stateCode() {
  switch (state) {
    case WARMUP:  return "WARMUP";
    case SOBER:   return "SOBER";
    case ALCOHOL: return "DRUNK";
    case FAULT:   return "FAULT";
    default:      return "FAULT";
  }
}

void setOutputs() {
  bool ok = (state == SOBER);
  setRelay(ok);                       // Fail-safe: only SOBER state energizes ignition relay
  digitalWrite(PIN_GREEN, ok ? HIGH : LOW);

  if (state == FAULT || state == WARMUP) {
    // 400ms flash for warmup and fault alerts
    digitalWrite(PIN_RED, (millis() / 400) % 2);
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

  // Approximate BAC % calculation based on MQ-3 calibration curve:
  // Margin +400 counts calibrated to ~0.080% BAC (standard legal limit)
  float estBac = 0.0f;
  if (diff > 0) {
    estBac = ((float)diff / (float)MARGIN_ON) * 0.080f;
  }
  if (estBac < 0.0f) estBac = 0.0f;

  // Remaining warmup seconds (0 when warmed up)
  int warmupSec = 0;
  if (state == WARMUP) {
    unsigned long elapsed = now - bootTime;
    if (elapsed < WARMUP_MS) {
      warmupSec = (WARMUP_MS - elapsed + 999) / 1000;
    }
  }

  // Remaining lockout hold time in seconds
  float holdSec = 0.0;
  if (state == ALCOHOL && diff < MARGIN_OFF) {
    unsigned long elapsedHold = now - lastHighTime;
    if (elapsedHold < HOLD_MS) {
      holdSec = (float)(HOLD_MS - elapsedHold) / 1000.0f;
    }
  }

  // Emit clean, structured JSON telemetry line over USB
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
  Serial.print(MARGIN_ON);
  Serial.print(",\"margin_off\":");
  Serial.print(MARGIN_OFF);
  Serial.print(",\"ignition_allowed\":");
  Serial.print(ignitionAllowed ? "true" : "false");
  Serial.print(",\"warmup_sec\":");
  Serial.print(warmupSec);
  Serial.print(",\"hold_sec\":");
  Serial.print(holdSec, 1);
  Serial.print(",\"fault\":");
  Serial.print(state == FAULT ? "true" : "false");
  Serial.println("}");
}

// ---------- Incoming Serial Command Parser ----------
void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "CAL" || cmd == "CALIBRATE") {
    Serial.println("{\"action\":\"Recalibrating clean air reference...\"}");
    if (takeBaseline()) {
      state = SOBER;
    } else {
      state = FAULT;
    }
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
    } else {
      if (inputBuffer.length() < 64) {
        inputBuffer += c;
      }
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
  analogSetPinAttenuation(PIN_MQ3, ADC_11db); // ~3.3V full scale

  bootTime = millis();
  Serial.println("\n{\"status\":\"BOOT_COMPLETE\",\"device\":\"ESP32_ALCOHOL_LOCK\",\"baud\":115200}");
  Serial.println("{\"info\":\"Sensor preheating... Keep in clean ambient air.\"}");
}

// ---------- Main Loop ----------
void loop() {
  unsigned long now = millis();

  // 1. Process commands received over USB Web Serial
  processSerialInput();

  // 2. Sample ADC every 100ms
  if (now - lastSample >= 100) {
    lastSample = now;
    rawReading = analogRead(PIN_MQ3);

    // Exponential moving average filter for noise rejection
    smoothed = (smoothed == 0) ? rawReading : (smoothed * 9 + rawReading) / 10;

    // Detect hardware wire disconnect or rail short
    bool faulty = (rawReading < ADC_MIN || rawReading > ADC_MAX);

    // --- State Machine Evaluation ---
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
    } else if (faulty) {
      state = FAULT;
      faultRecoverStart = 0;
    } else {
      int diff = smoothed - baseline;

      // Alcohol trip detection
      if (diff > MARGIN_ON) {
        state = ALCOHOL;
        lastHighTime = now;
      } else if (state == ALCOHOL) {
        // Hysteresis release: reading must stay below MARGIN_OFF for full HOLD_MS duration
        if (diff < MARGIN_OFF && (now - lastHighTime >= HOLD_MS)) {
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

    // Update physical LEDs and ignition relay
    setOutputs();
  }

  // 3. Stream JSON telemetry to USB Serial at regular intervals
  if (now - lastTelemetry >= TELEMETRY_MS) {
    lastTelemetry = now;
    sendTelemetryJSON();
  }
}

