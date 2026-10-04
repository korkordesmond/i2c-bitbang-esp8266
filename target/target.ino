// Bit-banged I2C target for ESP8266.
// Listens at TARGET_ADDRESS using two pin-change interrupts (one on SDA, one
// on SCL). The last byte received in a transaction is shown on 8 LEDs through
// a 74HC595 shift register.

const uint8_t TARGET_ADDRESS = 0x50;    // 7-bit target address
const int SDA_PIN = D2;   // GPIO4
const int SCL_PIN = D1;   // GPIO5

// 74HC595 shift register
const int SER_PIN   = D7;   // data
const int RCLK_PIN  = D6;   // latch
const int SRCLK_PIN = D5;   // shift clock

// Older ESP8266 cores call this ICACHE_RAM_ATTR. Interrupt handlers must live in IRAM.
#ifndef IRAM_ATTR
#define IRAM_ATTR ICACHE_RAM_ATTR
#endif

#define SDA_MASK (1 << SDA_PIN)   // SDA's bit in the GPIO registers

// ---- Shared between the ISRs and loop() ----
volatile byte lastReceivedByte = 0;
volatile bool newDataFlag = false;

// ---- Bit-bang target state ----
// ST_IDLE   - no transaction in progress
// ST_ADDR   - receiving the address byte
// ST_DATA   - address matched, receiving data bytes
// ST_IGNORE - address didn't match (or was a read), ignore until the next START
enum State : uint8_t { ST_IDLE, ST_ADDR, ST_DATA, ST_IGNORE };
volatile State state = ST_IDLE;
volatile uint8_t shiftReg = 0;    // bits of the current byte received so far
volatile uint8_t bitCount = 0;    // how many bits of the current byte we have
volatile bool ackPhase = false;   // currently in the 9th (ACK) clock
volatile bool ackThis = false;    // are we ACKing this byte?
volatile bool gotBytes = false;   // at least one data byte in this transaction

// Open-drain emulation: the output latch is kept LOW, so we only toggle the
// output enable. GPES enables the output (pulls the line low), GPEC disables it.
inline void IRAM_ATTR sdaLow()     { GPES = SDA_MASK; }   // enable output -> pulls low
inline void IRAM_ATTR sdaRelease() { GPEC = SDA_MASK; }   // disable output -> pull-up lifts it

void showOnLeds(byte value) {
  shiftOut(SER_PIN, SRCLK_PIN, MSBFIRST, value);
  digitalWrite(RCLK_PIN, HIGH);   // latch the shifted bits onto the outputs
  digitalWrite(RCLK_PIN, LOW);
}

// Called on a STOP or repeated START: the equivalent of receiveEvent().
// It only raises a flag; the LEDs and Serial are too slow to touch inside an ISR.
inline void IRAM_ATTR endOfTransaction() {
  if (state == ST_DATA && gotBytes) newDataFlag = true;
  gotBytes = false;
}

// SDA changed. It only matters while SCL is high: falling = START, rising = STOP.
void IRAM_ATTR sdaISR() {
  if (!GPIP(SCL_PIN)) return;        // SCL low: normal data change, ignore

  if (GPIP(SDA_PIN)) {
    // SDA rising while SCL high = STOP
    endOfTransaction();
    state = ST_IDLE;
    sdaRelease();
  } else {
    // SDA falling while SCL high = START (or repeated START)
    endOfTransaction();
    state    = ST_ADDR;
    bitCount = 0;
    shiftReg = 0;
    ackPhase = false;
    sdaRelease();
  }
}

// SCL changed. Rising edge: sample a bit. Falling edge: drive or release SDA.
void IRAM_ATTR sclISR() {
  if (state == ST_IDLE || state == ST_IGNORE) return;

  if (GPIP(SCL_PIN)) {
    // ---- Rising edge: sample data bits ----
    if (!ackPhase) {
      shiftReg = (shiftReg << 1) | (GPIP(SDA_PIN) ? 1 : 0);
      bitCount++;
    }
  } else {
    // ---- Falling edge: drive/release SDA ----
    if (ackPhase) {
      // End of 9th clock: release SDA and prepare for next byte
      sdaRelease();
      ackPhase = false;
      bitCount = 0;

      if (state == ST_ADDR) {
        state = ackThis ? ST_DATA : ST_IGNORE;
      }
      shiftReg = 0;
    }
    else if (bitCount == 8) {
      // End of 8th clock: decide ACK / NACK. An ACK means pulling SDA low
      // for the 9th clock.
      if (state == ST_ADDR) {
        // match address, and only accept write (R/W = 0)
        ackThis = ((shiftReg >> 1) == TARGET_ADDRESS) && !(shiftReg & 0x01);
      } else {  // ST_DATA
        lastReceivedByte = shiftReg;   // keep the last byte received
        gotBytes = true;
        ackThis = true;
      }
      if (ackThis) sdaLow();
      ackPhase = true;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);                         // give the serial monitor time to connect
  Serial.println("Slave starting...");

  pinMode(SER_PIN, OUTPUT);
  pinMode(RCLK_PIN, OUTPUT);
  pinMode(SRCLK_PIN, OUTPUT);
  digitalWrite(RCLK_PIN, LOW);
  digitalWrite(SRCLK_PIN, LOW);
  showOnLeds(0x00);                    // start with all LEDs off

  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);
  GPOC = SDA_MASK;        // output latch low (only used when output enabled)
  sdaRelease();

  attachInterrupt(digitalPinToInterrupt(SDA_PIN), sdaISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(SCL_PIN), sclISR, CHANGE);

  Serial.print("Listening as bit-banged I2C slave at address 0x");
  Serial.println(TARGET_ADDRESS, HEX);
}

void loop() {
  if (newDataFlag) {
    newDataFlag = false;               // clear first, so a byte arriving now is not lost
    noInterrupts();                    // copy with interrupts off so an ISR can't change it mid-read
    byte value = lastReceivedByte;
    interrupts();

    showOnLeds(value);                 // display the received byte on the 8 LEDs

    Serial.print("Received byte: 0b");
    for (int i = 7; i >= 0; i--) {
      Serial.print((value >> i) & 0x01);
    }
    Serial.print(" (0x");
    Serial.print(value, HEX);
    Serial.println(")");
  }
}