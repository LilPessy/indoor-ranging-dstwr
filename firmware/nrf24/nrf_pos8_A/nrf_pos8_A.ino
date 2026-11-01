#include <SPI.h>
#include <RF24.h>
#include <math.h>

#define M_CAL       5000
#define N_MIS       2000
#define D_CAL_M     1.0
#define BUTTON_PIN  4
#define PROGRESS    1000

#define CE_PIN      9
#define CSN_PIN     10
#define SPI_HZ      8000000UL
#define CHANNEL     80
#define DATA_RATE   RF24_2MBPS
#define PA_LEVEL    RF24_PA_LOW

#define PERIOD_MS       10
#define FINAL_DELAY_US  0
#define TX_WINDOW_US    2000
#define RESP_WINDOW_US  5000

const double C_M_PER_NS = 0.299792458;

const float PLAN_DIST[] = { 2.0 };

const char* const POS_NAME[] = { "x = -35 cm", "x = -25 cm", "x = -15 cm", "x = -5 cm",
                                 "x = +5 cm",  "x = +15 cm", "x = +25 cm", "x = +35 cm" };
#define MAX_PERSE_PCT  1.0
const uint8_t ND   = sizeof(PLAN_DIST) / sizeof(PLAN_DIST[0]);
const uint8_t NPOS = sizeof(POS_NAME) / sizeof(POS_NAME[0]);
double resD[16], resUA[16], resLost[16];
uint32_t phSent0 = 0, phRecv0 = 0;
bool inPlan(uint16_t k) { return k >= 1 && k <= ND * NPOS; }

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
uint32_t gptHz;

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
  gptHz = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_PCLKD);
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
uint8_t sendPacket(uint8_t *data, uint32_t &t) {
  radio.stopListening();
  clearIrq();
  if (armCapture()) return 1;
  radio.startWrite(data, sizeof(Packet), false);
  uint8_t r = waitCapture(TX_WINDOW_US, t);
  clearIrq();
  return r;
}
void halt(const __FlashStringHelper *msg) {
  Serial.println(msg);
  while (true) delay(1000);
}

struct Stat {
  uint32_t n; double mean, m2;
  void reset() { n = 0; mean = 0; m2 = 0; }
  void add(double x) { n++; double d = x - mean; mean += d / n; m2 += d * (x - mean); }
  double var() const { return n > 1 ? m2 / (n - 1) : 0; }
  double sd()  const { return sqrt(var()); }
};

double tQuant975(double nu) {
  static const double T[30] = { 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
                                2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
                                2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042 };
  if (nu < 1) nu = 1;
  if (nu <= 30) return T[(int)floor(nu) - 1];
  const double z = 1.959963985;
  double z3 = z*z*z, z5 = z3*z*z, z7 = z5*z*z;
  return z + (z3 + z) / (4*nu) + (5*z5 + 16*z3 + 3*z) / (96*nu*nu)
           + (3*z7 + 19*z5 + 17*z3 - 15*z) / (384*nu*nu*nu);
}

enum SessState : uint8_t { ST_RUN, ST_WAIT };
SessState state = ST_RUN;
uint16_t phase = 0;
uint32_t target = M_CAL;
Stat     cal, mis;

const uint8_t HIST = 16;
uint32_t histRa[HIST], histDa[HIST];
uint16_t histId[HIST];
uint8_t  histRpd[HIST];
bool     histValid[HIST];
uint16_t nextId = 0;
uint32_t sent = 0, received = 0;

void clearHistory() { for (uint8_t i = 0; i < HIST; i++) histValid[i] = false; }

void printCalibration() {
  Serial.println(F("# ================= CALIBRAZIONE COMPLETATA ================="));
  Serial.print(F("# M = ")); Serial.print(cal.n);
  Serial.print(F("   media = ")); Serial.print(cal.mean, 2);
  Serial.print(F(" ns   s = ")); Serial.print(cal.sd(), 2); Serial.println(F(" ns"));

  Serial.print(F("#CAL,")); Serial.print(cal.n); Serial.print(',');
  Serial.print(cal.mean, 3); Serial.print(','); Serial.println(cal.sd(), 3);
}

void printMeasure() {
  double delta = mis.mean - cal.mean;
  double d = D_CAL_M + C_M_PER_NS * delta;

  double seA = mis.sd() / sqrt((double)mis.n);
  double tA  = tQuant975(mis.n - 1);
  double uA  = C_M_PER_NS * tA * seA;
  if (inPlan(phase)) {
    resD[phase - 1] = d; resUA[phase - 1] = uA;
    uint32_t ps = sent - phSent0, pr = received - phRecv0;
    resLost[phase - 1] = ps ? 100.0 * (double)(ps - pr) / ps : 0;
  }

  Serial.print(F("# ================= MISURA ")); Serial.print(phase); Serial.println(F(" COMPLETATA ================="));
  Serial.print(F("# calibrazione: M = ")); Serial.print(cal.n);
  Serial.print(F("   media = ")); Serial.print(cal.mean, 2);
  Serial.print(F(" ns   s = ")); Serial.print(cal.sd(), 2); Serial.println(F(" ns"));
  Serial.print(F("# misura:       N = ")); Serial.print(mis.n);
  Serial.print(F("   media = ")); Serial.print(mis.mean, 2);
  Serial.print(F(" ns   s = ")); Serial.print(mis.sd(), 2); Serial.println(F(" ns"));
  Serial.print(F("# Delta = ")); Serial.print(delta, 2);
  Serial.print(F(" ns   ->   d = ")); Serial.print(d, 3); Serial.println(F(" m"));

  Serial.print(F("# >>> d = ")); Serial.print(d, 3);
  Serial.print(F(" +/- ")); Serial.print(uA, 3);
  Serial.print(F(" m   IC95 = [")); Serial.print(d - uA, 3); Serial.print(F(" ; ")); Serial.print(d + uA, 3);
  Serial.print(F("] m   (SE = ")); Serial.print(seA, 2); Serial.print(F(" ns, t = ")); Serial.print(tA, 3); Serial.println(')');

  Serial.print(F("#RIS,")); Serial.print(phase); Serial.print(',');
  Serial.print(mis.n); Serial.print(','); Serial.print(mis.mean, 3); Serial.print(',');
  Serial.print(mis.sd(), 3); Serial.print(','); Serial.print(cal.n); Serial.print(',');
  Serial.print(cal.mean, 3); Serial.print(','); Serial.print(cal.sd(), 3); Serial.print(',');
  Serial.print(delta, 3); Serial.print(','); Serial.print(d, 4); Serial.print(',');
  Serial.print(seA, 3); Serial.print(','); Serial.print(tA, 4); Serial.print(','); Serial.println(uA, 4);
}

void printPlace(uint16_t k) {
  if (inPlan(k)) {
    Serial.print(F("B a ")); Serial.print(PLAN_DIST[(k - 1) / NPOS], 2);
    Serial.print(F(" m, ")); Serial.print(POS_NAME[(k - 1) % NPOS]);
  } else {
    Serial.print(F("misura libera (fuori piano)"));
  }
}

void enterWait() {
  state = ST_WAIT;
  clearHistory();
  Serial.print(F("# >>> In attesa - MISURA ")); Serial.print(phase + 1); Serial.print(F(": "));
  printPlace(phase + 1);
  Serial.println(F("  -> premi il pulsante (D4)"));
}

void groupStats(uint8_t g, double &mean, double &s, double &mn, double &ua) {
  mean = 0; mn = 1e9; ua = 0;
  for (uint8_t i = 0; i < NPOS; i++) {
    uint8_t j = g * NPOS + i;
    mean += resD[j]; ua += resUA[j];
    if (resD[j] < mn) mn = resD[j];
  }
  mean /= NPOS; ua /= NPOS;
  s = 0;
  for (uint8_t i = 0; i < NPOS; i++) { double e = resD[g * NPOS + i] - mean; s += e * e; }
  s = sqrt(s / (NPOS - 1));
}

double mediana(double *v, uint8_t n) {
  double a[16];
  for (uint8_t i = 0; i < n; i++) a[i] = v[i];
  for (uint8_t i = 1; i < n; i++) { double x = a[i]; int8_t j = i - 1; while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; } a[j + 1] = x; }
  return (n % 2) ? a[n / 2] : 0.5 * (a[n / 2 - 1] + a[n / 2]);
}

void printDistanceSummary(uint8_t g) {
  double D = PLAN_DIST[g], mean, s, mn, ua;
  groupStats(g, mean, s, mn, ua);
  double t = tQuant975(NPOS - 1);
  double U = t * s / sqrt((double)NPOS);
  double med = mediana(&resD[g * NPOS], NPOS);

  double v[16]; uint8_t nv = 0; double sf = 0, mf = 0;
  for (uint8_t i = 0; i < NPOS; i++) if (resLost[g * NPOS + i] <= MAX_PERSE_PCT) v[nv++] = resD[g * NPOS + i];
  for (uint8_t i = 0; i < nv; i++) mf += v[i];
  if (nv) mf /= nv;
  for (uint8_t i = 0; i < nv; i++) sf += (v[i] - mf) * (v[i] - mf);
  double Uf = (nv > 1) ? tQuant975(nv - 1) * sqrt(sf / (nv - 1)) / sqrt((double)nv) : NAN;
  double medf = nv ? mediana(v, nv) : NAN;

  Serial.print(F("# ############ DISTANZA ")); Serial.print(D, 2); Serial.print(F(" m - ")); Serial.print(NPOS);
  Serial.println(F(" posizioni ############"));
  for (uint8_t i = 0; i < NPOS; i++) {
    Serial.print(F("#   ")); Serial.print(POS_NAME[i]); Serial.print(F(": d = ")); Serial.print(resD[g * NPOS + i], 2);
    Serial.print(F(" m   perse = ")); Serial.print(resLost[g * NPOS + i], 2); Serial.print('%');
    Serial.println(resLost[g * NPOS + i] > MAX_PERSE_PCT ? F("   <- fading, esclusa") : F(""));
  }
  Serial.print(F("# >>> MEDIA (tutte):        ")); Serial.print(mean, 3); Serial.print(F(" m   IC95 = [")); Serial.print(mean - U, 3);
  Serial.print(F(" ; ")); Serial.print(mean + U, 3); Serial.print(F("] m   errore = ")); Serial.print(mean - D, 3); Serial.println(F(" m"));
  Serial.print(F("# >>> MEDIANA (tutte):      ")); Serial.print(med, 3); Serial.print(F(" m   errore = ")); Serial.print(med - D, 3); Serial.println(F(" m"));
  Serial.print(F("# >>> MEDIA (pulite, ")); Serial.print(nv); Serial.print('/'); Serial.print(NPOS); Serial.print(F("): "));
  Serial.print(mf, 3); Serial.print(F(" m   IC95 = [")); Serial.print(mf - Uf, 3); Serial.print(F(" ; ")); Serial.print(mf + Uf, 3);
  Serial.print(F("] m   errore = ")); Serial.print(mf - D, 3); Serial.println(F(" m"));
  Serial.print(F("# >>> MEDIANA (pulite):     ")); Serial.print(medf, 3); Serial.print(F(" m   errore = ")); Serial.print(medf - D, 3); Serial.println(F(" m"));

  Serial.print(F("#DIST,")); Serial.print(D, 3); Serial.print(','); Serial.print(NPOS); Serial.print(',');
  Serial.print(mean, 4); Serial.print(','); Serial.print(U, 4); Serial.print(','); Serial.print(med, 4); Serial.print(',');
  Serial.print(nv); Serial.print(','); Serial.print(mf, 4); Serial.print(','); Serial.print(Uf, 4); Serial.print(',');
  Serial.print(medf, 4); Serial.print(','); Serial.print(s, 4); Serial.print(','); Serial.println(ua, 4);
}

void printPlanTable() {
  Serial.println(F("# ================= PIANO COMPLETATO: RIEPILOGO ================="));
  Serial.println(F("# d vera | ToF media [IC95]        | ToF minimo"));
  for (uint8_t g = 0; g < ND; g++) {
    double mean, s, mn, ua;
    groupStats(g, mean, s, mn, ua);
    double U = tQuant975(NPOS - 1) * s / sqrt((double)NPOS);
    Serial.print(F("# ")); Serial.print(PLAN_DIST[g], 2); Serial.print(F(" m | ")); Serial.print(mean, 2);
    Serial.print(F(" [")); Serial.print(mean - U, 2); Serial.print(F(" ; ")); Serial.print(mean + U, 2);
    Serial.print(F("] | ")); Serial.println(mn, 2);
  }
  Serial.println(F("# Altre pressioni del pulsante = misure libere. Ctrl+C sul terminale per chiudere il file."));
}

bool startRequested() {
  if (Serial.available()) { while (Serial.available()) Serial.read(); return true; }
  if (digitalRead(BUTTON_PIN) == HIGH) return false;
  delay(30);
  if (digitalRead(BUTTON_PIN) == HIGH) return false;
  while (digitalRead(BUTTON_PIN) == LOW) delay(5);
  delay(30);
  return true;
}

void addSample(double dsNs) {
  if (phase == 0) cal.add(dsNs); else mis.add(dsNs);
  uint32_t n = (phase == 0) ? cal.n : mis.n;
  if (n % PROGRESS == 0 && n < target) {
    Serial.print(F("# avanzamento ")); Serial.print(n); Serial.print('/'); Serial.println(target);
  }
  if (n >= target) {
    if (phase == 0) printCalibration();
    else {
      printMeasure();
      if (inPlan(phase) && phase % NPOS == 0) printDistanceSummary((phase - 1) / NPOS);
      if (phase == ND * NPOS) printPlanTable();
    }
    enterWait();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  while (!Serial);
  delay(500);

  if (!radio.begin() || !radio.isChipConnected()) halt(F("# nRF24 non risponde: controlla alimentazione e SPI"));
  radio.setChannel(CHANNEL);
  radio.setDataRate(DATA_RATE);
  radio.setPALevel(PA_LEVEL);
  radio.setAutoAck(false);
  radio.setCRCLength(RF24_CRC_16);
  radio.setPayloadSize(sizeof(Packet));
  radio.maskIRQ(false, true, false);
  radio.openWritingPipe(ADDR);
  radio.openReadingPipe(1, ADDR);
  if (radio.getChannel() != CHANNEL || radio.getDataRate() != DATA_RATE)
    halt(F("# Rilettura registri errata: abbassa SPI_HZ"));

  captureInit();
  clearIrq();
  if (irqLow()) halt(F("# IRQ resta basso: controlla il filo D2 -> IRQ"));
  uint32_t c0 = R_GPT1->GTCNT; delayMicroseconds(10);
  if (R_GPT1->GTCNT == c0) halt(F("# GPT1 fermo: il timer non conta"));
  Packet test = { 0, 0, 0, 0, 0 };
  uint32_t tt;
  if (sendPacket((uint8_t*)&test, tt) != 0)
    halt(F("# AUTOTEST CATTURA FALLITO: il fronte di IRQ non arriva al GPT1 (pin D2?)"));

  Serial.print(F("# DS-TWR nRF24 SESSIONE AUTOMATICA - canale ")); Serial.print(CHANNEL);
  Serial.print(F(" - 2 Mbps - PA ")); Serial.print((int)PA_LEVEL);
  Serial.print(F(" - timer ")); Serial.print(gptHz); Serial.print(F(" Hz - periodo "));
  Serial.print(PERIOD_MS); Serial.println(F(" ms"));
  Serial.print(F("# M_CAL = ")); Serial.print(M_CAL); Serial.print(F("   N_MIS = ")); Serial.print(N_MIS);
  Serial.print(F("   D_CAL = ")); Serial.print(D_CAL_M, 2); Serial.println(F(" m"));
  Serial.println(F("fase,n,id,ra_cyc,da_cyc,db_cyc,rb_cyc,tof_ss_ns,tof_ds_ns,rpd,resp_ricevuti,poll_inviati"));
  if (ND * NPOS > 16 || NPOS < 2) halt(F("# Piano non valido: al massimo 16 misure e almeno 2 posizioni"));
  Serial.print(F("# Piano: distanze [m]:")); for (uint8_t g = 0; g < ND; g++) { Serial.print(' '); Serial.print(PLAN_DIST[g], 2); }
  Serial.print(F("  x ")); Serial.print(NPOS); Serial.println(F(" posizioni"));
  Serial.println(F("# >>> CALIBRAZIONE in corso: B a 1 m, centro"));

  cal.reset(); mis.reset(); clearHistory();
}

void loop() {
  if (state == ST_WAIT) {
    if (!startRequested()) return;
    phase++;
    mis.reset();
    target = N_MIS;
    phSent0 = sent; phRecv0 = received;
    clearHistory();
    Serial.print(F("# >>> MISURA ")); Serial.print(phase); Serial.print(F(" in corso: ")); printPlace(phase); Serial.println();
    Serial.print(F("#PIANO,")); Serial.print(phase); Serial.print(',');
    if (inPlan(phase)) { Serial.print(PLAN_DIST[(phase - 1) / NPOS], 3); Serial.print(','); Serial.println(POS_NAME[(phase - 1) % NPOS]); }
    else Serial.println(F("0,libera"));
    state = ST_RUN;
  }

  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < PERIOD_MS) return;
  lastPoll = millis();

  uint16_t id = nextId++;
  uint32_t t1, t4, t5;

  Packet poll = { PKT_POLL, id, (uint16_t)PA_LEVEL, 0, 0 };
  uint8_t r = sendPacket((uint8_t*)&poll, t1);
  if (r != 0) { Serial.print(F("# POLL errore ")); Serial.print(r); Serial.print(F(" id ")); Serial.println(id); return; }
  sent++;

  radio.flush_rx();
  if (armCapture()) { Serial.print(F("# IRQ basso prima della RESP id ")); Serial.println(id); clearIrq(); return; }
  radio.startListening();
  r = waitCapture(RESP_WINDOW_US, t4);
  if (r != 0) {
    radio.stopListening(); clearIrq();
    Serial.print(F("# RESP persa id ")); Serial.println(id);
    return;
  }
  uint8_t rpd = radio.testRPD() ? 1 : 0;
  Packet resp;
  radio.read(&resp, sizeof(resp));
  if (resp.type != PKT_RESP || resp.id != id) {
    radio.stopListening(); clearIrq();
    Serial.print(F("# RESP non valida id ")); Serial.println(id);
    return;
  }
  received++;

  delayMicroseconds(FINAL_DELAY_US);
  Packet fin = { PKT_FINAL, id, 0, 0, 0 };
  r = sendPacket((uint8_t*)&fin, t5);

  uint8_t k = id % HIST;
  histValid[k] = false;
  if (r != 0) {
    Serial.print(F("# FINAL errore ")); Serial.print(r); Serial.print(F(" id ")); Serial.println(id);
  } else {
    histRa[k] = t4 - t1; histDa[k] = t5 - t4; histId[k] = id; histRpd[k] = rpd; histValid[k] = true;
  }

  uint8_t j = resp.prevId % HIST;
  if (resp.dbCyc != 0 && resp.rbCyc != 0 && histValid[j] && histId[j] == resp.prevId) {
    int64_t Ra = histRa[j], Da = histDa[j], Db = resp.dbCyc, Rb = resp.rbCyc;
    double nsPerCyc = 1e9 / (double)gptHz;
    double ssNs = (double)(Ra - Db) / 2.0 * nsPerCyc;
    double dsNs = (double)(Ra * Rb - Da * Db) / (double)(Ra + Rb + Da + Db) * nsPerCyc;
    histValid[j] = false;

    uint32_t n = ((phase == 0) ? cal.n : mis.n) + 1;
    Serial.print(phase);                 Serial.print(',');
    Serial.print(n);                     Serial.print(',');
    Serial.print(resp.prevId);           Serial.print(',');
    Serial.print((uint32_t)Ra);          Serial.print(',');
    Serial.print((uint32_t)Da);          Serial.print(',');
    Serial.print((uint32_t)Db);          Serial.print(',');
    Serial.print((uint32_t)Rb);          Serial.print(',');
    Serial.print(ssNs, 1);               Serial.print(',');
    Serial.print(dsNs, 1);               Serial.print(',');
    Serial.print(histRpd[j]);            Serial.print(',');
    Serial.print(received);              Serial.print(',');
    Serial.println(sent);

    addSample(dsNs);
  }
}
