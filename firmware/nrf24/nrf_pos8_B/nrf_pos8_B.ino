#include <SPI.h>
#include <RF24.h>

#define B_SERIAL_LOG    0

#define CE_PIN      9
#define CSN_PIN     10
#define SPI_HZ      8000000UL
#define CHANNEL     80
#define DATA_RATE   RF24_2MBPS
#define PA_LEVEL    RF24_PA_LOW

#define REPLY_DELAY_US  0
#define WAIT_WINDOW_US  10000
#define TX_WINDOW_US    2000

RF24 radio(CE_PIN, CSN_PIN, SPI_HZ);
const uint8_t ADDR[6] = "DSTWR";

enum : uint8_t { PKT_POLL = 1, PKT_RESP = 2, PKT_FINAL = 3 };
struct __attribute__((packed)) Packet {
  uint8_t  type;
  uint16_t id;
  uint16_t prevId;
  uint32_t dbCyc;
  uint32_t rbCyc;
};

static inline bool irqLow() { return (R_PORT1->PCNTR2 & (1u << 5)) == 0; }

void captureInit() {
  R_BSP_MODULE_START(FSP_IP_GPT, 1);
  R_BSP_PinAccessEnable();
  R_BSP_PinCfg(BSP_IO_PORT_01_PIN_05, (3u << 24) | (1u << 16));
  R_BSP_PinAccessDisable();
  R_GPT1->GTCR     = 0;
  R_GPT1->GTUDDTYC = 1;
  R_GPT1->GTIOR    = 0;
  R_GPT1->GTICASR  = (1u << 10) | (1u << 11);
  R_GPT1->GTICBSR  = 0;
  R_GPT1->GTPR     = 0xFFFFFFFFu;
  R_GPT1->GTCNT    = 0;
  R_GPT1->GTST     = 0;
  R_GPT1->GTCR     = 1;
}

uint8_t armCapture() {
  if (irqLow()) return 1;
  R_GPT1->GTST = 0;
  return 0;
}

uint8_t waitCapture(uint32_t windowUs, uint32_t &t) {
  uint32_t t0 = micros();
  while ((R_GPT1->GTST & 1u) == 0) {
    if (micros() - t0 > windowUs) return 2;
  }
  t = R_GPT1->GTCCR[0];
  R_GPT1->GTST = 0;
  return 0;
}

SPISettings spiCfg(SPI_HZ, MSBFIRST, SPI_MODE0);
void clearIrq() {
  SPI.beginTransaction(spiCfg);
  digitalWrite(CSN_PIN, LOW);
  SPI.transfer(0x20 | 0x07);
  SPI.transfer(0x70);
  digitalWrite(CSN_PIN, HIGH);
  SPI.endTransaction();
}

void listen() {
  radio.flush_rx();
  clearIrq();
  armCapture();
  radio.startListening();
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
  if (!radio.begin() || !radio.isChipConnected()) fail(F("# nRF24 non risponde"));
  radio.setChannel(CHANNEL);
  radio.setDataRate(DATA_RATE);
  radio.setPALevel(PA_LEVEL);
  radio.setAutoAck(false);
  radio.setCRCLength(RF24_CRC_16);
  radio.setPayloadSize(sizeof(Packet));
  radio.maskIRQ(false, true, false);
  radio.openWritingPipe(ADDR);
  radio.openReadingPipe(1, ADDR);
  if (radio.getChannel() != CHANNEL || radio.getDataRate() != DATA_RATE) fail(F("# Rilettura registri errata"));

  captureInit();
  clearIrq();
  if (irqLow()) fail(F("# IRQ resta basso"));

  uint32_t c0 = R_GPT1->GTCNT; delayMicroseconds(10);
  if (R_GPT1->GTCNT == c0) fail(F("# GPT1 fermo"));
  Packet test = { 0, 0, 0, 0, 0 };
  uint32_t tt;
  radio.stopListening(); clearIrq(); armCapture();
  radio.startWrite((uint8_t*)&test, sizeof(test), false);
  if (waitCapture(TX_WINDOW_US, tt) != 0) fail(F("# AUTOTEST CATTURA FALLITO"));
  clearIrq();

#if B_SERIAL_LOG
  Serial.println(F("# DS-TWR nRF24 nodo B (cattura hardware) in ascolto"));
#endif
  listen();
}

void loop() {
  uint32_t tRx;
  uint8_t r = waitCapture(WAIT_WINDOW_US, tRx);
  if (r != 0) {
    if (irqLow()) { respSent = false; radio.stopListening(); listen(); }
    return;
  }

  Packet p;
  radio.read(&p, sizeof(p));

  if (p.type == PKT_POLL) {

    Packet resp = { PKT_RESP, p.id, lastId, lastDb, lastRb };
    delayMicroseconds(REPLY_DELAY_US);
    radio.stopListening();
    if (p.prevId <= RF24_PA_MAX) radio.setPALevel((uint8_t)p.prevId);
    clearIrq();
    armCapture();
    radio.startWrite((uint8_t*)&resp, sizeof(resp), false);
    uint32_t tTx;
    r = waitCapture(TX_WINDOW_US, tTx);
    listen();

    respSent = (r == 0);
    curId = p.id; t2 = tRx; t3 = tTx;
#if B_SERIAL_LOG
    Serial.print(F("POLL ")); Serial.print(p.id); Serial.print(F(" | Db ")); Serial.println(t3 - t2);
#endif
  }
  else if (p.type == PKT_FINAL && respSent && p.id == curId) {

    lastId = curId; lastDb = t3 - t2; lastRb = tRx - t3;
    respSent = false;
    armCapture();
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
#if B_SERIAL_LOG
    Serial.print(F("FINAL ")); Serial.print(p.id); Serial.print(F(" | Rb ")); Serial.println(lastRb);
#endif
  }
  else {
    respSent = false;
    armCapture();
  }
}
