#include <RadioLib.h>
#include <SPI.h>

#define B_SERIAL_LOG    0

#define MODULATION      1
#define BITRATE_SEL     250

#if MODULATION == 3
#define USE_FALL_EDGE   1
#else
#define USE_FALL_EDGE   0
#endif

#define FREQ_MHZ        433.0
#if BITRATE_SEL == 500
  #if MODULATION == 3 || MODULATION == 2
    #error "A 500 kbps usare 2-FSK, GFSK o MSK"
  #endif
  #define BITRATE_KBPS  500.0
  #define RX_BW_KHZ     812.0
#else
  #if MODULATION == 3
    #define BITRATE_KBPS 125.0
  #else
    #define BITRATE_KBPS 250.0
  #endif
  #define RX_BW_KHZ     541.0
#endif
#define FREQ_DEV_KHZ    127.0
#define TX_POWER_DBM    10
#define PREAMBLE_BITS   64

#define REPLY_DELAY_US  0
#define WAIT_WINDOW_US  10000
#define TX_WINDOW_US    5000

const int CS_PIN   = 10;
const int GDO0_PIN = 2;
CC1101 radio = new Module(CS_PIN, GDO0_PIN, RADIOLIB_NC, RADIOLIB_NC);

enum : uint8_t { PKT_POLL = 1, PKT_RESP = 2, PKT_FINAL = 3 };
struct __attribute__((packed)) Packet {
  uint8_t  type;
  uint16_t id;
  uint16_t prevId;
  uint32_t dbCyc;
  uint32_t rbCyc;
};

static inline bool gdo0High() { return (R_PORT1->PCNTR2 & (1u << 5)) != 0; }

void captureInit() {
  R_BSP_MODULE_START(FSP_IP_GPT, 1);
  R_BSP_PinAccessEnable();
  R_BSP_PinCfg(BSP_IO_PORT_01_PIN_05, (3u << 24) | (1u << 16));
  R_BSP_PinAccessDisable();
  R_GPT1->GTCR     = 0;
  R_GPT1->GTUDDTYC = 1;
  R_GPT1->GTIOR    = 0;
  R_GPT1->GTICASR  = (1u << 8)  | (1u << 9);
  R_GPT1->GTICBSR  = (1u << 10) | (1u << 11);
  R_GPT1->GTPR     = 0xFFFFFFFFu;
  R_GPT1->GTCNT    = 0;
  R_GPT1->GTST     = 0;
  R_GPT1->GTCR     = 1;
}

uint8_t armCapture() {
  R_GPT1->GTST = 0;
  if (gdo0High()) return USE_FALL_EDGE ? 0 : 1;
  return 0;
}

uint8_t waitCapture(uint32_t windowUs, uint32_t &t) {
  uint32_t t0 = micros();
  while ((R_GPT1->GTST & 2u) == 0) {
    if (micros() - t0 > windowUs) return 2;
  }
  bool rise = (R_GPT1->GTST & 1u) != 0;
  uint32_t tRise = R_GPT1->GTCCR[0], tFall = R_GPT1->GTCCR[1];
  R_GPT1->GTST = 0;
#if USE_FALL_EDGE
  t = tFall; (void)rise; (void)tRise;
  return 0;
#else
  if (!rise) return 1;
  t = tRise; (void)tFall;
  return 0;
#endif
}

SPISettings cfg(1000000, MSBFIRST, SPI_MODE0);
void writeReg(uint8_t addr, uint8_t val) {
  SPI.beginTransaction(cfg);
  digitalWrite(CS_PIN, LOW);
  uint32_t t0 = millis();
  while (digitalRead(MISO) == HIGH && millis() - t0 < 50);
  SPI.transfer(addr);
  SPI.transfer(val);
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
}
uint8_t readReg(uint8_t addr) {
  SPI.beginTransaction(cfg);
  digitalWrite(CS_PIN, LOW);
  uint32_t t0 = millis();
  while (digitalRead(MISO) == HIGH && millis() - t0 < 50);
  SPI.transfer(addr | 0x80);
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
  return v;
}
void gdo0Sync() { writeReg(0x02, 0x06); }
uint8_t listen() { radio.startReceive(); gdo0Sync(); return armCapture(); }

void setModFormat(uint8_t fmt) {
  uint8_t v = readReg(0x12);
  writeReg(0x12, (v & 0x8F) | (fmt << 4));
}
void applyModulation() {
#if MODULATION == 1
  radio.setDataShaping(RADIOLIB_SHAPING_0_5);
#elif MODULATION == 2
  radio.setOOK(true);
#elif MODULATION == 3
  setModFormat(0b100);
#elif MODULATION == 4
  setModFormat(0b111);
  writeReg(0x15, 0x00);
#endif
}

bool selfTest() {
  writeReg(0x02, 0x2F); delay(2);
  R_GPT1->GTST = 0;
  writeReg(0x02, 0x6F); delay(2);
  bool hi = gdo0High();
  writeReg(0x02, 0x2F); delay(2);
  bool lo = !gdo0High();
  bool caps = (R_GPT1->GTST & 3u) == 3u;
  R_GPT1->GTST = 0;
  return hi && lo && caps;
}

void fail(const __FlashStringHelper *msg) {
#if B_SERIAL_LOG
  Serial.println(msg);
#endif
  while (true) { digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); delay(80); }
}

bool     respSent = false;
uint16_t curId = 0;
uint32_t t2 = 0, t3 = 0;
uint16_t lastId = 0;
uint32_t lastDb = 0, lastRb = 0;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
#if B_SERIAL_LOG
  Serial.begin(115200);
  while (!Serial && millis() < 3000);
#endif
  if (radio.begin(FREQ_MHZ, BITRATE_KBPS, FREQ_DEV_KHZ, RX_BW_KHZ, TX_POWER_DBM, PREAMBLE_BITS) != RADIOLIB_ERR_NONE)
    fail(F("# begin() fallito"));
  applyModulation();
  captureInit();
  if (!selfTest()) fail(F("# AUTOTEST CATTURA FALLITO"));
#if B_SERIAL_LOG
  Serial.println(F("# DS-TWR CC1101 nodo B (cattura hardware) in ascolto"));
#endif
  listen();
}

void loop() {
  uint32_t tRx;
  uint8_t r = waitCapture(WAIT_WINDOW_US, tRx);
  if (r == 2) {
#if B_SERIAL_LOG
    static uint32_t lastBeat = 0;
    if (millis() - lastBeat > 2000) {
      lastBeat = millis();
      Serial.print(F("# in ascolto - GDO0 = ")); Serial.print(gdo0High());
      Serial.print(F("  GTST = 0x")); Serial.print(R_GPT1->GTST, HEX);
      Serial.print(F("  IOCFG0 = 0x")); Serial.println(readReg(0x02), HEX);
    }
#endif
    return;
  }
  if (r != 0) {
#if B_SERIAL_LOG
    Serial.println(F("# discesa senza salita: pacchetto scartato"));
#endif
    respSent = false; listen(); return;
  }

  Packet p;
  int st = radio.readData((uint8_t*)&p, sizeof(p));
  if (st != RADIOLIB_ERR_NONE) {
#if B_SERIAL_LOG
    Serial.print(F("# readData errore ")); Serial.println(st);
#endif
    listen(); return;
  }

  if (p.type == PKT_POLL) {

    Packet resp = { PKT_RESP, p.id, lastId, lastDb, lastRb };
    delayMicroseconds(REPLY_DELAY_US);
    radio.startTransmit((uint8_t*)&resp, sizeof(resp));
    gdo0Sync();
    uint32_t tTx = 0;
    r = armCapture();
    if (r == 0) r = waitCapture(TX_WINDOW_US, tTx);
    radio.finishTransmit();
    listen();

    respSent = (r == 0);
    curId = p.id; t2 = tRx; t3 = tTx;
#if B_SERIAL_LOG
    if (r != 0) { Serial.print(F("# RESP: cattura TX fallita ")); Serial.println(r); }
#endif
#if B_SERIAL_LOG
    Serial.print(F("POLL ")); Serial.print(p.id); Serial.print(F(" | Db ")); Serial.println(t3 - t2);
#endif
  }
  else if (p.type == PKT_FINAL && respSent && p.id == curId) {

    lastId = curId; lastDb = t3 - t2; lastRb = tRx - t3;
    respSent = false;
    listen();
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
#if B_SERIAL_LOG
    Serial.print(F("FINAL ")); Serial.print(p.id); Serial.print(F(" | Rb ")); Serial.println(lastRb);
#endif
  }
  else {
#if B_SERIAL_LOG
    Serial.print(F("# pacchetto inatteso tipo ")); Serial.print(p.type); Serial.print(F(" id ")); Serial.println(p.id);
#endif
    respSent = false;
    listen();
  }
}
