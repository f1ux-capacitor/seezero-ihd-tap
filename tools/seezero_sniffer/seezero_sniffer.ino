/*
  SeeZero IHD serial sniffer  -  ESP32-C3 SuperMini
  ---------------------------------------------------
  Listens (receive only) on one IHD pin and prints what it hears over USB.
  Recognises NXP-style frames (01 ... 03, 02-escaped, XOR checksum) as used
  by the Geo Trio II Zigbee chip, and prints any plain text it sees too.

  Wiring (test stage):
    ESP G       -> IHD J7 GND (right-hand pin)
    ESP GPIO20  -> the IHD pin under test
    ESP 5V / TX -> NOT connected (ESP is powered from your computer's USB)

  Arduino IDE settings:
    Board: "ESP32C3 Dev Module"   USB CDC On Boot: "Enabled"
  Serial Monitor: 115200 baud.

  On start-up it first MEASURES the pulse widths on the pin (RMT peripheral,
  0.1 us resolution) to work out the exact baud rate, prints what it found,
  then listens at that speed and decodes. If the measurement fails it falls
  back to sweeping a list of standard speeds.

  Keys you can type in the Serial Monitor (then press Enter):
    p  = measure the pulse widths / baud rate again
    n  = also show [TEXT] and failed 01..03 frames (Trio-style decoding)
    s  = run the standard-speed sweep
    b  = step to the next baud rate manually
    r  = toggle raw hex dump of every byte (needs n on)

  Normal output is one line per message:  [time] length B: hex bytes
*/

#include <algorithm>

#if !ARDUINO_USB_CDC_ON_BOOT
#error "Set Tools > USB CDC On Boot > Enabled, otherwise GPIO20 clashes with the USB log."
#endif

const int  RX_PIN  = 20;     // IHD pin under test goes here
const int  LED_PIN = 8;      // SuperMini blue LED, active low
const long BAUDS[] = {115200, 230400, 250000, 460800, 500000, 921600, 1000000,
                      1500000, 2000000, 2500000, 3000000, 3686400, 4000000,
                      57600, 38400, 19200, 9600};
const int  NBAUDS  = sizeof(BAUDS) / sizeof(BAUDS[0]);
const uint32_t SWEEP_MS = 8000;   // listening time per speed during the sweep
int  baudIdx = 0;
long curBaud = 115200;            // speed the UART is listening at now
const uint32_t RMT_HZ = 80000000; // RMT tick = 12.5 ns (finest the C3 can do)
const float    TICKS_PER_US = RMT_HZ / 1e6f;
bool rawDump = false;
bool nxpVerbose = false;          // also print [TEXT] and failed 01..03 frames (key n)

// message capture: bytes are grouped into messages split on idle gaps
// and on the AA AA sync header seen on the SeeZero
uint8_t  msg[512];
int      mlen = 0;
uint32_t lastUs = 0, msgCount = 0;
const uint32_t GAP_US = 300;
volatile uint32_t frameErrs = 0;  // UART framing errors (wrong speed => lots)

// frame capture
uint8_t  frame[600];
int      flen = 0;
bool     inFrame = false;
// plain-text capture (debug output from a chip)
char     text[200];
int      tlen = 0;
// stats
uint32_t bytesTotal = 0, bytesWindow = 0, framesOk = 0, framesBad = 0;
uint32_t lastReport = 0, lastByte = 0;

bool quiet = false;               // true during the sweep: count, don't print

void onUartError(hardwareSerial_error_t err) {
  if (err == UART_FRAME_ERROR) frameErrs++;
}

void openUart(long baud) {
  Serial1.end();
  Serial1.setRxBufferSize(4096);
  Serial1.onReceiveError(onUartError);
  Serial1.begin(baud, SERIAL_8N1, RX_PIN, -1);   // -1: no TX pin, receive only
  Serial1.setRxFIFOFull(64);   // drain the hardware FIFO early: bytes arrive fast at ~3.7 Mbaud
  inFrame = false; flen = 0; tlen = 0; mlen = 0;
  frameErrs = 0;
}

void startUart() {
  openUart(curBaud);
  Serial.printf("\n=== Listening on GPIO%d at %ld baud ===\n", RX_PIN, curBaud);
}

void flushText() {
  if (tlen >= 4 && !quiet && nxpVerbose) { text[tlen] = 0; Serial.printf("[TEXT] %s\n", text); }
  tlen = 0;
}

// Un-escape with a given XOR key and check the checksum.
// Layout after un-escaping: type(2) len(2) checksum(1) data(...)
bool decode(uint8_t key, uint8_t *out, int &olen) {
  olen = 0;
  for (int i = 0; i < flen; i++) {
    uint8_t c = frame[i];
    if (c == 0x02 && i + 1 < flen) c = frame[++i] ^ key;
    out[olen++] = c;
  }
  if (olen < 5) return false;
  uint8_t x = 0;
  for (int i = 0; i < olen; i++) if (i != 4) x ^= out[i];
  return x == out[4];
}

void handleFrame() {
  static uint8_t buf[600];
  int n;
  uint8_t keyUsed = 0;
  if (decode(0x08, buf, n))      keyUsed = 0x08;   // what the Trio II uses
  else if (decode(0x10, buf, n)) keyUsed = 0x10;   // classic ZiGate style

  if (keyUsed) {
    framesOk++;
    if (quiet) return;
    uint16_t type = (buf[0] << 8) | buf[1];
    uint16_t len  = (buf[2] << 8) | buf[3];
    Serial.printf("[FRAME OK] type=0x%04X len=%u escXOR=0x%02X :", type, len, keyUsed);
    for (int i = 5; i < n; i++) Serial.printf(" %02X", buf[i]);
    Serial.println();
    if (type == 0x8002) Serial.println("   ^ 0x8002 = Zigbee data indication (meter data on the Trio II)");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  } else {
    framesBad++;
    if (quiet || !nxpVerbose) return;
    Serial.printf("[FRAME ??] %d bytes, checksum failed :", flen);
    for (int i = 0; i < flen && i < 64; i++) Serial.printf(" %02X", frame[i]);
    Serial.println(flen > 64 ? " ..." : "");
  }
}

uint32_t sweepBytes = 0, sweep01 = 0, sweep03 = 0;

void emitMsg(int n) {                  // print the first n bytes of msg[] as one message
  if (n <= 0) return;
  msgCount++;
  if (!quiet) {
    Serial.printf("[%8.3f s] %3d B:", millis() / 1000.0f, n);
    for (int i = 0; i < n; i++) Serial.printf(" %02X", msg[i]);
    Serial.println();
  }
}

void addToMsg(uint8_t c) {
  uint32_t now = micros();
  if (mlen && now - lastUs > GAP_US) { emitMsg(mlen); mlen = 0; }   // idle gap
  lastUs = now;
  // a new AA AA header inside the buffer starts a new message
  if (c == 0xAA && mlen > 2 && msg[mlen - 1] == 0xAA) {
    emitMsg(mlen - 1);
    msg[0] = 0xAA; mlen = 1;
  }
  if (mlen < (int)sizeof(msg)) msg[mlen++] = c;
  else { emitMsg(mlen); mlen = 0; }
}

void processByte(uint8_t c) {
  bytesTotal++; bytesWindow++; lastByte = millis();
  addToMsg(c);
  if (c == 0x01) sweep01++;
  if (c == 0x03) sweep03++;
  if (rawDump && !quiet && nxpVerbose) Serial.printf("%02X ", c);

  if (c == 0x01) {                       // start of frame
    flushText();
    inFrame = true; flen = 0;
  } else if (inFrame && c == 0x03) {     // end of frame
    inFrame = false;
    handleFrame();
  } else if (inFrame) {
    if (flen < (int)sizeof(frame)) frame[flen++] = c;
    else inFrame = false;                // runaway, drop it
  } else if (c >= 0x20 && c < 0x7F) {    // printable text outside frames
    if (tlen < (int)sizeof(text) - 1) text[tlen++] = c;
  } else if (c == '\n' || c == '\r') {
    flushText();
  }
}

// Listen at every speed for SWEEP_MS, score it, then lock on to the best.
void sweep() {
  Serial.printf("\n=== Baud sweep on GPIO%d: %d speeds x %lu s, please wait ===\n",
                RX_PIN, NBAUDS, (unsigned long)(SWEEP_MS / 1000));
  Serial.println("    baud    bytes  frameErr  err%   01s   03s  frames OK/bad");
  quiet = true;
  int best = -1;
  float bestScore = -1e9;
  for (int i = 0; i < NBAUDS; i++) {
    openUart(BAUDS[i]);
    framesOk = framesBad = 0; sweep01 = sweep03 = 0;
    uint32_t b0 = bytesTotal, t0 = millis();
    while (millis() - t0 < SWEEP_MS) {
      while (Serial1.available()) processByte(Serial1.read());
      delay(1);
    }
    uint32_t n = bytesTotal - b0;
    uint32_t fe = frameErrs;
    float errPct = n ? 100.0f * fe / n : 0;
    Serial.printf("%8ld %8lu %9lu %5.1f %5lu %5lu   %lu/%lu\n", BAUDS[i],
                  (unsigned long)n, (unsigned long)fe, errPct,
                  (unsigned long)sweep01, (unsigned long)sweep03,
                  (unsigned long)framesOk, (unsigned long)framesBad);
    // score: valid frames win outright; otherwise fewest framing errors per byte
    float score = framesOk * 1000.0f + (n >= 20 ? (100.0f - errPct) : -1000.0f);
    if (score > bestScore) { bestScore = score; best = i; }
  }
  quiet = false;
  framesOk = framesBad = 0;
  baudIdx = best < 0 ? 0 : best;
  curBaud = BAUDS[baudIdx];
  Serial.printf("=== Best guess: %ld baud ===\n", curBaud);
  startUart();
}

// ---------- pulse-width measurement (works out the real baud rate) ----------
uint16_t durs[2000];      // pulse lengths in RMT ticks (0.1 us)
int      ndur = 0;

void addDur(uint32_t d) {
  if (d >= 4 && d < 30000 && ndur < (int)(sizeof(durs) / sizeof(durs[0]))) durs[ndur++] = d;
}

// Returns the measured baud rate, or 0 if it could not tell.
long measureBaud() {
  Serial.printf("\n=== Measuring pulse widths on GPIO%d for up to 15 s ===\n", RX_PIN);
  Serial1.end();
  delay(20);
  bool ok = rmtInit(RX_PIN, RMT_RX_MODE, RMT_MEM_NUM_BLOCKS_2, RMT_HZ);
  if (!ok) ok = rmtInit(RX_PIN, RMT_RX_MODE, RMT_MEM_NUM_BLOCKS_1, RMT_HZ);
  if (!ok) { Serial.println("RMT init failed - skipping measurement."); return 0; }
  rmtSetRxMaxThreshold(RX_PIN, 30000);          // 375 us of idle ends a capture
  rmtSetRxMinThreshold(RX_PIN, 2);              // ignore glitches under 25 ns

  static rmt_data_t buf[96];
  ndur = 0;
  int captures = 0, shown = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 15000 && ndur < 1800) {
    size_t n = sizeof(buf) / sizeof(buf[0]);
    if (!rmtRead(RX_PIN, buf, &n, 1000)) continue;
    if (n == 0) continue;
    captures++;
    if (shown < 2 && n > 6) {                    // show the first couple of bursts
      shown++;
      Serial.printf("Burst %d (level:us):", shown);
      for (size_t i = 0; i < n && i < 40; i++) {
        Serial.printf(" %c%.2f", buf[i].level0 ? 'H' : 'L', buf[i].duration0 / TICKS_PER_US);
        if (buf[i].duration1) Serial.printf(" %c%.2f", buf[i].level1 ? 'H' : 'L', buf[i].duration1 / TICKS_PER_US);
      }
      Serial.println(n > 40 ? " ..." : "");
    }
    for (size_t i = 0; i < n; i++) { addDur(buf[i].duration0); addDur(buf[i].duration1); }
  }
  rmtDeinit(RX_PIN);
  Serial.printf("Captured %d bursts, %d pulses.\n", captures, ndur);
  if (ndur < 20) { Serial.println("Too few pulses to measure."); return 0; }

  // 1) smallest pulse length that occurs repeatedly = first guess at one bit
  static uint16_t srt[2000];
  memcpy(srt, durs, ndur * sizeof(uint16_t));
  std::sort(srt, srt + ndur);
  int need = ndur / 50;
  if (need < 3) need = 3;
  float T = 0;
  for (int i = 0; i < ndur && T == 0; i++) {
    float c = srt[i]; int cnt = 0;
    for (int j = i; j < ndur && srt[j] <= c * 1.25f; j++) cnt++;
    if (cnt >= need) T = c;
  }
  if (T == 0) { Serial.println("No repeating short pulse found."); return 0; }

  // 2) refine: every pulse should be a whole number of bits long
  for (int pass = 0; pass < 3; pass++) {
    double sumD = 0; long sumN = 0;
    for (int i = 0; i < ndur; i++) {
      int k = lroundf(durs[i] / T);
      if (k >= 1 && k <= 10 && fabsf(durs[i] - k * T) < 0.3f * T) { sumD += durs[i]; sumN += k; }
    }
    if (sumN) T = sumD / sumN;
  }
  int fit = 0, hist[11] = {0};
  for (int i = 0; i < ndur; i++) {
    int k = lroundf(durs[i] / T);
    if (k >= 1 && k <= 10 && fabsf(durs[i] - k * T) < 0.3f * T) { fit++; hist[k]++; }
  }
  long baud = lroundf(RMT_HZ / T);
  float fitPct = 100.0f * fit / ndur;
  Serial.printf("One bit = %.4f us  =>  about %ld baud.  %.0f%% of pulses are whole bits.\n",
                T / TICKS_PER_US, baud, fitPct);
  Serial.print("Pulse lengths in bits:");
  for (int k = 1; k <= 10; k++) Serial.printf("  %dx:%d", k, hist[k]);
  Serial.println();
  if (fitPct < 50) { Serial.println("Doesn't look like clean UART timing."); return 0; }
  // snap to a standard speed if we're within 3% of one
  for (int i = 0; i < NBAUDS; i++)
    if (fabsf((float)baud / BAUDS[i] - 1.0f) < 0.015f) {
      Serial.printf("Nearest standard speed: %ld baud - using that.\n", BAUDS[i]);
      return BAUDS[i];
    }
  Serial.println("Not a standard speed - using the measured value as-is.");
  return baud;
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);          // LED off
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 5000) delay(10);
  delay(2000);                          // time to open the Serial Monitor
  Serial.println("\nSeeZero sniffer ready. Keys: p=measure baud, s=sweep, b=next baud, r=raw hex.");
  long b = measureBaud();
  if (b > 0) { curBaud = b; startUart(); }
  else sweep();
}

void loop() {
  // keyboard commands
  while (Serial.available()) {
    char k = Serial.read();
    if (k == 's') sweep();
    if (k == 'p') { long b = measureBaud(); if (b > 0) curBaud = b; startUart(); }
    if (k == 'b') { baudIdx = (baudIdx + 1) % NBAUDS; curBaud = BAUDS[baudIdx]; startUart(); }
    if (k == 'n') { nxpVerbose = !nxpVerbose; Serial.printf("\nNXP/text output %s\n", nxpVerbose ? "ON" : "OFF"); }
    if (k == 'r') { rawDump = !rawDump; Serial.printf("\nRaw hex %s\n", rawDump ? "ON" : "OFF"); }
  }

  // bytes from the IHD
  while (Serial1.available()) processByte(Serial1.read());
  if (mlen && micros() - lastUs > 20000) { emitMsg(mlen); mlen = 0; }   // flush after 20 ms quiet

  // summary every 10 s
  if (millis() - lastReport >= 10000) {
    lastReport = millis();
    flushText();
    if (bytesWindow == 0)
      Serial.printf("-- 10 s: no data on GPIO%d at %ld baud. Try another pin, or type p to re-measure.\n",
                    RX_PIN, curBaud);
    else
      Serial.printf("-- 10 s @ %ld baud: %lu bytes, %lu messages so far | 01..03 frames OK %lu\n",
                    curBaud, (unsigned long)bytesWindow, (unsigned long)msgCount,
                    (unsigned long)framesOk);
    bytesWindow = 0; frameErrs = 0;
  }
}
