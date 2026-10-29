// Alcohol detection ignition lock - ESP32 + MQ-3
// No buzzer, no Wi-Fi, no Bluetooth.
// Green LED = OK, Red LED = blocked, relay = ignition enable
//
// MQ-3 AO is connected through a 10k / 10k divider:
//   AO --[10k]--+-- GPIO 34
//               |
//             [10k]
//               |
//              GND
// A 5V sensor output becomes about 2.5V at the pin, which is safe.

// ---------- Pins ----------
const int PIN_MQ3   = 34;
const int PIN_GREEN = 26;
const int PIN_RED   = 27;
const int PIN_RELAY = 14;

// Many relay modules switch ON when the pin is LOW. Set true if yours does.
const bool RELAY_ACTIVE_LOW = false;

// ---------- Settings ----------
const unsigned long WARMUP_MS = 30000;       // MQ-3 needs time to stabilise (longer is better)
const int MARGIN_ON  = 400;                  // ADC counts above baseline => alcohol (tune this)
const int MARGIN_OFF = 250;                  // must fall below this margin to clear (hysteresis)
const unsigned long HOLD_MS = 10000;         // keep blocked this long after reading clears
const int ADC_MIN = 20;                      // below this: sensor disconnected or shorted
const int ADC_MAX = 4090;                    // above this: rail fault
const int BASELINE_SAMPLES = 80;             // samples averaged for the clean-air baseline
const unsigned long FAULT_RECOVER_MS = 2000; // valid readings needed before leaving FAULT
// ---------------------------

enum State { WARMUP, SOBER, ALCOHOL, FAULT };
State state = WARMUP;

int baseline = 0;
bool baselineValid = false;
int smoothed = 0;
unsigned long bootTime = 0;
unsigned long lastSample = 0;
unsigned long lastHighTime = 0;
unsigned long faultRecoverStart = 0;

void setRelay(bool enableIgnition) {
  digitalWrite(PIN_RELAY, (enableIgnition ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

const char* stateName() {
  switch (state) {
    case WARMUP:  return "WARMING UP";
    case SOBER:   return "OK - ignition allowed";
    case ALCOHOL: return "ALCOHOL DETECTED - ignition blocked";
    default:      return "SENSOR FAULT - ignition blocked";
  }
}

void setOutputs() {
  bool ok = (state == SOBER);
  setRelay(ok);                       // fail-safe: only SOBER enables ignition
  digitalWrite(PIN_GREEN, ok ? HIGH : LOW);
  if (state == FAULT || state == WARMUP) {
    digitalWrite(PIN_RED, (millis() / 400) % 2);   // blink red
  } else {
    digitalWrite(PIN_RED, ok ? LOW : HIGH);
  }
}

// Averages clean-air readings. Returns true if the baseline is usable.
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
    Serial.printf("Baseline = %d\n", baseline);
  } else {
    Serial.println("Baseline invalid: check sensor wiring.");
  }
  return baselineValid;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_MQ3, INPUT);
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);
  setRelay(false);                    // start blocked
  analogReadResolution(12);           // 0-4095
  analogSetPinAttenuation(PIN_MQ3, ADC_11db);

  bootTime = millis();
  Serial.println("Warming up sensor... keep it in clean air.");
}

void loop() {
  unsigned long now = millis();
  if (now - lastSample >= 100) {
    lastSample = now;
    int raw = analogRead(PIN_MQ3);

    // simple low-pass smoothing for noise rejection
    smoothed = (smoothed == 0) ? raw : (smoothed * 9 + raw) / 10;

    // fault detection: reading stuck at a rail => wire broken or shorted
    bool faulty = (raw < ADC_MIN || raw > ADC_MAX);

    if (state == WARMUP) {
      if (now - bootTime >= WARMUP_MS) {
        if (takeBaseline()) {
          state = SOBER;
        } else {
          state = FAULT;
          faultRecoverStart = 0;
        }
      }
    } else if (faulty) {
      state = FAULT;
      faultRecoverStart = 0;
    } else if (state == FAULT) {
      if (faultRecoverStart == 0) faultRecoverStart = now;
      if (now - faultRecoverStart >= FAULT_RECOVER_MS) {
        // If the startup baseline was bad, take a fresh one before allowing ignition.
        if (!baselineValid && !takeBaseline()) {
          faultRecoverStart = 0;      // still bad, stay blocked
        } else {
          state = SOBER;
          faultRecoverStart = 0;
          Serial.println("Sensor recovered - ignition allowed.");
        }
      }
    } else {
      int diff = smoothed - baseline;
      if (diff > MARGIN_ON) {
        state = ALCOHOL;
        lastHighTime = now;
      } else if (state == ALCOHOL) {
        if (diff < MARGIN_OFF && now - lastHighTime >= HOLD_MS) {
          state = SOBER;
        }
      }
    }

    setOutputs();

    static unsigned long lastPrint = 0;
    if (now - lastPrint >= 1000) {
      lastPrint = now;
      Serial.printf("raw=%d smooth=%d base=%d state=%s\n", raw, smoothed, baseline, stateName());
    }
  }
}
