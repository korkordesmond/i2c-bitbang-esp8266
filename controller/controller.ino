// Bit-banged I2C controller for ESP8266.
// Once a second it sends two bytes to the target: its address + write bit,
// then DATA_BYTE, and prints whether each byte was ACKed.
//
// The bus is driven by a state machine that advances one step per tick
// (TICK_US), so loop() never blocks in a delay.

const int SCL_PIN = D1;
const int SDA_PIN = D2;

const uint8_t TARGET_ADDRESS = 0x50;    // 7-bit target address
const uint8_t DATA_BYTE = 0b00111001;

// One tick = TICK_US microseconds between state-machine steps.
// A data bit takes 4 ticks, so the nominal bus clock is about
// 1,000,000 / (4 * TICK_US) Hz. The real clock is somewhat slower because of
// pin-change and loop overhead.
const unsigned long TICK_US = 4;
const unsigned long RETRY_INTERVAL_US = 1000000UL;   // one transaction per second
const unsigned long STRETCH_TIMEOUT_US = 10000UL;    // longest we wait for a target holding SCL low

// States, and how many ticks each one takes:
// IDLE       - wait for the next transaction
// START      - SDA falls while SCL is high, then SCL falls              (2 ticks)
// SEND_BYTE  - per bit: set SDA, SCL rises, SCL held high, SCL falls     (4 ticks per bit, MSB first)
// READ_ACK   - release SDA, SCL rises, sample SDA (low = ACK), SCL falls (4 ticks)
// STOP       - SDA low, SCL rises, SDA rises while SCL is high           (3 ticks)
enum I2CState { IDLE, START, SEND_BYTE, READ_ACK, STOP };

I2CState state = IDLE;
uint8_t phase = 0;       // step within the current state
uint8_t bitIndex = 0;    // bit being sent in the current byte (0 = MSB)

uint8_t txQueue[2];      // bytes of this transaction: address+write, then data
uint8_t txLen = 0;
uint8_t txIndex = 0;     // byte currently being sent

bool ackReceived = false;
bool acked[2];           // ACK result per byte
uint8_t resultCount = 0;
bool reportPending = false;

bool waiting = false;    // true while waiting for a target to release SCL
unsigned long waitStart = 0;
unsigned long lastTick = 0;
unsigned long lastTransaction = 0;

// Open-drain line control: either drive the line low, or release it and let
// the pull-up raise it. We never drive a line high.
void sdaLow()  { pinMode(SDA_PIN, OUTPUT); digitalWrite(SDA_PIN, LOW); }
void sdaHigh() { pinMode(SDA_PIN, INPUT_PULLUP); }
void sclLow()  { pinMode(SCL_PIN, OUTPUT); digitalWrite(SCL_PIN, LOW); }
void sclHigh() { pinMode(SCL_PIN, INPUT_PULLUP); }

// Give up on the transaction: release both lines and go back to IDLE.
// Used when SCL stays low longer than STRETCH_TIMEOUT_US.
void abortBus(const char* reason) {
  Serial.println(reason);
  sdaHigh();
  sclHigh();
  state = IDLE;
}

// Returns true once SCL has really gone high. A target may hold SCL low to
// stretch the clock, so after releasing it we have to check. While waiting we
// return false and the caller tries again on the next tick.
bool sclIsHigh() {
  if (digitalRead(SCL_PIN)) { waiting = false; return true; }
  if (!waiting) { waiting = true; waitStart = micros(); }
  else if (micros() - waitStart > STRETCH_TIMEOUT_US) { waiting = false; abortBus("SCL stuck low, aborting"); }
  return false;
}

void beginTransaction(uint8_t dataToSend) {
  txQueue[0] = (TARGET_ADDRESS << 1) | 0;         // address + write bit
  txQueue[1] = dataToSend;
  txLen = 2;
  txIndex = 0;
  resultCount = 0;
  phase = 0;
  state = START;
}

// One step of the bus state machine, called once per tick.
void busTick() {
  switch (state) {

    case IDLE:
      break;

    case START:
      if (phase == 0) {                          // SDA falls while SCL is high = START
        sdaLow();
        phase = 1;
      } else {
        sclLow();
        bitIndex = 0;
        phase = 0;
        state = SEND_BYTE;
      }
      break;

    case SEND_BYTE:
      switch (phase) {
        case 0:                                  // SCL is low: put the next bit on SDA (1 = release, 0 = pull low)
          if ((txQueue[txIndex] >> (7 - bitIndex)) & 1) sdaHigh(); else sdaLow();
          phase = 1;
          break;
        case 1:                                  // raise SCL, then wait if the target is stretching it
          sclHigh();
          if (!sclIsHigh()) return;
          phase = 2;
          break;
        case 2:                                  // hold SCL high so the target can sample SDA
          phase = 3;
          break;
        case 3:                                  // lower SCL; after 8 bits, read the ACK
          sclLow();
          phase = 0;
          if (++bitIndex >= 8) state = READ_ACK;
          break;
      }
      break;

    case READ_ACK:
      switch (phase) {
        case 0:                                  // release SDA so the target can pull it low
          sdaHigh();
          phase = 1;
          break;
        case 1:
          sclHigh();
          if (!sclIsHigh()) return;
          phase = 2;
          break;
        case 2:                                  // sample while SCL is high
          ackReceived = (digitalRead(SDA_PIN) == LOW);
          phase = 3;
          break;
        case 3:
          sclLow();
          acked[txIndex] = ackReceived;          // remember it, print after the STOP
          resultCount = txIndex + 1;
          txIndex++;
          phase = 0;
          bitIndex = 0;
          // Send the next byte only if this one was ACKed; a NACK ends the transaction early
          state = (ackReceived && txIndex < txLen) ? SEND_BYTE : STOP;
          break;
      }
      break;

    case STOP:
      switch (phase) {
        case 0:                                  // SDA low while SCL is still low
          sdaLow();
          phase = 1;
          break;
        case 1:
          sclHigh();
          if (!sclIsHigh()) return;
          phase = 2;
          break;
        case 2:                                  // SDA rises while SCL is high = STOP
          sdaHigh();
          phase = 0;
          state = IDLE;
          reportPending = true;
          break;
      }
      break;
  }
}

// One line per byte actually sent (a NACK on the address means only one line).
void printReport() {
  for (uint8_t i = 0; i < resultCount; i++) {
    Serial.print("Byte 0x");
    Serial.print(txQueue[i], HEX);
    Serial.println(acked[i] ? " -> ACK" : " -> NACK");
  }
}

void setup() {
  Serial.begin(115200);
  // Set the output latch LOW once. After this we only switch the pins between
  // output (pull low) and input (release), so the latch never needs to change.
  digitalWrite(SDA_PIN, LOW);
  digitalWrite(SCL_PIN, LOW);
  sdaHigh();
  sclHigh();
  lastTransaction = micros() - RETRY_INTERVAL_US;   // first transaction starts immediately
}

void loop() {
  unsigned long now = micros();

  // Start a new transaction once per RETRY_INTERVAL_US
  if (state == IDLE && (now - lastTransaction) >= RETRY_INTERVAL_US) {
    Serial.println("--- Starting new transaction ---");
    beginTransaction(DATA_BYTE);
    lastTransaction = now;
    lastTick = now;
  }

  // Advance the state machine once per tick
  if (state != IDLE && (now - lastTick) >= TICK_US) {
    lastTick = now;
    busTick();
  }

  // Serial is slow, so only print once the bus is idle and it can't disturb the timing
  if (reportPending && state == IDLE) {
    reportPending = false;
    printReport();
  }
}