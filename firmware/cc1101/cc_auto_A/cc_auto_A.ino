#include <RadioLib.h>
#include <SPI.h>
#include <math.h>

#define M_CAL       5000
#define N_MIS       5000
#define D_CAL_M     1.0
#define PATH_LOSS_N 2.00
#define BUTTON_PIN  4
#define PROGRESS    1000

const double C_M_PER_NS = 0.299792458;

const float PLAN_DIST[] = { 1.0, 1.5, 2.0, 2.5 };
const char* const POS_NAME[] = { "centro", "destra 30 cm", "sinistra 30 cm" };
const uint8_t ND   = sizeof(PLAN_DIST) / sizeof(PLAN_DIST[0]);
const uint8_t NPOS = sizeof(POS_NAME) / sizeof(POS_NAME[0]);
double resD[16], resUA[16], resDP[16];
bool inPlan(uint16_t k) { return k >= 1 && k <= ND * NPOS; }

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

#define PERIOD_MS       15
#define FINAL_DELAY_US  0
#define TX_WINDOW_US    5000
#define RESP_WINDOW_US  20000

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
uint32_t gptHz;

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
  gptHz = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_PCLKD);
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
const char* modName() {
  switch (MODULATION) {
    case 1: return "GFSK"; case 2: return "OOK"; case 3: return "4-FSK"; case 4: return "MSK";
    default: return "2-FSK";
  }
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

uint8_t sendPacket(uint8_t *data, uint32_t &t) {
  radio.startTransmit(data, sizeof(Packet));
  gdo0Sync();
  uint8_t r = armCapture();
  if (r == 0) r = waitCapture(TX_WINDOW_US, t);
  radio.finishTransmit();
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
Stat     calRssi, misRssi;

const uint8_t HIST = 16;
uint32_t histRa[HIST], histDa[HIST];
uint16_t histId[HIST];
float    histRssi[HIST];
uint8_t  histLqi[HIST];
bool     histValid[HIST];
uint16_t nextId = 0;
uint32_t sent = 0, received = 0;

void clearHistory() { for (uint8_t i = 0; i < HIST; i++) histValid[i] = false; }

void printRssi(double mean, double sd) {
  Serial.print(F("# RSSI medio = ")); Serial.print(mean, 2);
  Serial.print(F(" dBm   s = ")); Serial.print(sd, 2); Serial.println(F(" dB"));
}

void printCalibration() {
  Serial.println(F("# ================= CALIBRAZIONE COMPLETATA ================="));
  Serial.print(F("# M = ")); Serial.print(cal.n);
  Serial.print(F("   media = ")); Serial.print(cal.mean, 2);
  Serial.print(F(" ns   s = ")); Serial.print(cal.sd(), 2); Serial.println(F(" ns"));

  Serial.print(F("#CAL,")); Serial.print(cal.n); Serial.print(',');
  Serial.print(cal.mean, 3); Serial.print(','); Serial.println(cal.sd(), 3);
  printRssi(calRssi.mean, calRssi.sd());
}

void printRssiEstimate();

void printMeasure() {
  double delta = mis.mean - cal.mean;
  double d = D_CAL_M + C_M_PER_NS * delta;

  double seA = mis.sd() / sqrt((double)mis.n);
  double tA  = tQuant975(mis.n - 1);
  double uA  = C_M_PER_NS * tA * seA;
  if (inPlan(phase)) { resD[phase - 1] = d; resUA[phase - 1] = uA; }

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
  printRssi(misRssi.mean, misRssi.sd());
  printRssiEstimate();
}

void printRssiEstimate() {
  double dP = calRssi.mean - misRssi.mean;
  if (inPlan(phase)) resDP[phase - 1] = dP;
  double k  = 10.0 * PATH_LOSS_N;
  double d  = D_CAL_M * pow(10.0, dP / k);

  double seA = misRssi.sd() / sqrt((double)misRssi.n);
  double uA  = tQuant975(misRssi.n - 1) * seA;

  Serial.print(F("# >>> RSSI (n = ")); Serial.print(PATH_LOSS_N, 2);
  Serial.print(F("): dP = ")); Serial.print(dP, 2);
  Serial.print(F(" dB  ->  d = ")); Serial.print(d, 3); Serial.println(F(" m"));
  Serial.print(F("#     IC95 = [")); Serial.print(D_CAL_M * pow(10.0, (dP - uA) / k), 3);
  Serial.print(F(" ; ")); Serial.print(D_CAL_M * pow(10.0, (dP + uA) / k), 3);
  Serial.print(F("] m   (+/- ")); Serial.print(uA, 3); Serial.println(F(" dB)"));

  Serial.print(F("#RSSI,")); Serial.print(phase); Serial.print(',');
  Serial.print(misRssi.n); Serial.print(','); Serial.print(misRssi.mean, 3); Serial.print(',');
  Serial.print(misRssi.sd(), 3); Serial.print(','); Serial.print(calRssi.n); Serial.print(',');
  Serial.print(calRssi.mean, 3); Serial.print(','); Serial.print(calRssi.sd(), 3); Serial.print(',');
  Serial.print(dP, 3); Serial.print(','); Serial.print(PATH_LOSS_N, 3); Serial.print(',');
  Serial.print(d, 4); Serial.print(','); Serial.println(uA, 4);
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

void printDistanceSummary(uint8_t g) {
  double D = PLAN_DIST[g];
  double mean = 0, mn = 1e9, dpm = 0, ua = 0;
  for (uint8_t i = 0; i < NPOS; i++) {
    uint8_t j = g * NPOS + i;
    mean += resD[j]; dpm += resDP[j]; ua += resUA[j];
    if (resD[j] < mn) mn = resD[j];
  }
  mean /= NPOS; dpm /= NPOS; ua /= NPOS;
  double s = 0, sdp = 0;
  for (uint8_t i = 0; i < NPOS; i++) {
    uint8_t j = g * NPOS + i;
    s += (resD[j] - mean) * (resD[j] - mean);
    sdp += (resDP[j] - dpm) * (resDP[j] - dpm);
  }
  s = sqrt(s / (NPOS - 1)); sdp = sqrt(sdp / (NPOS - 1));
  double t = tQuant975(NPOS - 1);
  double U = t * s / sqrt((double)NPOS);
  double k = 10.0 * PATH_LOSS_N;
  double Ur = t * sdp / sqrt((double)NPOS);
  double dR = D_CAL_M * pow(10.0, dpm / k);
  double rLo = D_CAL_M * pow(10.0, (dpm - Ur) / k), rHi = D_CAL_M * pow(10.0, (dpm + Ur) / k);

  Serial.print(F("# ############ DISTANZA ")); Serial.print(D, 2); Serial.print(F(" m - ")); Serial.print(NPOS);
  Serial.println(F(" posizioni ############"));
  Serial.print(F("# ToF  per posizione [m]:"));
  for (uint8_t i = 0; i < NPOS; i++) { Serial.print(' '); Serial.print(resD[g * NPOS + i], 2); }
  Serial.println();
  Serial.print(F("# ToF  media = ")); Serial.print(mean, 3); Serial.print(F(" m   s tra posizioni = ")); Serial.print(s, 3);
  Serial.print(F(" m   (incertezza statistica media per posizione +/- ")); Serial.print(ua, 3); Serial.println(F(" m)"));
  Serial.print(F("# >>> ToF  IC95 sulla media delle posizioni (t = ")); Serial.print(t, 2); Serial.print(F("): [")); Serial.print(mean - U, 3);
  Serial.print(F(" ; ")); Serial.print(mean + U, 3); Serial.print(F("] m   errore = ")); Serial.print(mean - D, 3); Serial.println(F(" m"));
  Serial.print(F("# >>> ToF  stima di MINIMO = ")); Serial.print(mn, 3); Serial.print(F(" m   errore = ")); Serial.print(mn - D, 3); Serial.println(F(" m"));
  Serial.print(F("# RSSI dP per posizione [dB]:"));
  for (uint8_t i = 0; i < NPOS; i++) { Serial.print(' '); Serial.print(resDP[g * NPOS + i], 2); }
  Serial.println();
  Serial.print(F("# >>> RSSI d = ")); Serial.print(dR, 3); Serial.print(F(" m   IC95 = [")); Serial.print(rLo, 3);
  Serial.print(F(" ; ")); Serial.print(rHi, 3); Serial.print(F("] m   (s dP tra posizioni = ")); Serial.print(sdp, 2);
  Serial.print(F(" dB)   errore = ")); Serial.print(dR - D, 3); Serial.println(F(" m"));

  Serial.print(F("#DIST,")); Serial.print(D, 3); Serial.print(','); Serial.print(NPOS); Serial.print(',');
  Serial.print(mean, 4); Serial.print(','); Serial.print(s, 4); Serial.print(','); Serial.print(U, 4); Serial.print(',');
  Serial.print(mn, 4); Serial.print(','); Serial.print(ua, 4); Serial.print(','); Serial.print(dR, 4); Serial.print(',');
  Serial.print(rLo, 4); Serial.print(','); Serial.print(rHi, 4); Serial.print(','); Serial.print(dpm, 3); Serial.print(',');
  Serial.println(sdp, 3);
}

void printPlanTable() {
  Serial.println(F("# ================= PIANO COMPLETATO: RIEPILOGO ================="));
  Serial.println(F("# d vera | ToF media [IC95]        | ToF minimo | RSSI"));
  for (uint8_t g = 0; g < ND; g++) {
    double mean = 0, mn = 1e9, dpm = 0;
    for (uint8_t i = 0; i < NPOS; i++) { uint8_t j = g * NPOS + i; mean += resD[j]; dpm += resDP[j]; if (resD[j] < mn) mn = resD[j]; }
    mean /= NPOS; dpm /= NPOS;
    double s = 0; for (uint8_t i = 0; i < NPOS; i++) { double e = resD[g * NPOS + i] - mean; s += e * e; }
    double U = tQuant975(NPOS - 1) * sqrt(s / (NPOS - 1)) / sqrt((double)NPOS);
    Serial.print(F("# ")); Serial.print(PLAN_DIST[g], 2); Serial.print(F(" m | ")); Serial.print(mean, 2);
    Serial.print(F(" [")); Serial.print(mean - U, 2); Serial.print(F(" ; ")); Serial.print(mean + U, 2);
    Serial.print(F("] | ")); Serial.print(mn, 2); Serial.print(F(" | "));
    Serial.println(D_CAL_M * pow(10.0, dpm / (10.0 * PATH_LOSS_N)), 2);
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

void addSample(double dsNs, double rssi) {
  if (phase == 0) { cal.add(dsNs); calRssi.add(rssi); }
  else            { mis.add(dsNs); misRssi.add(rssi); }
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

  int st = radio.begin(FREQ_MHZ, BITRATE_KBPS, FREQ_DEV_KHZ, RX_BW_KHZ, TX_POWER_DBM, PREAMBLE_BITS);
  if (st != RADIOLIB_ERR_NONE) { Serial.print(F("# begin() fallito: ")); Serial.println(st); while (true) delay(1000); }
  applyModulation();
  captureInit();
  if (!selfTest()) halt(F("# AUTOTEST FALLITO: GDO0 non arriva al timer GPT1 (controlla D2)"));
  if (ND * NPOS > 16 || NPOS < 2) halt(F("# Piano non valido: al massimo 16 misure e almeno 2 posizioni"));

  Serial.print(F("# DS-TWR CC1101 SESSIONE AUTOMATICA - ")); Serial.print(modName());
  Serial.print(' '); Serial.print(BITRATE_KBPS, 0); Serial.print(F(" kbaud - timer "));
  Serial.print(gptHz); Serial.print(F(" Hz - fronte ")); Serial.print(USE_FALL_EDGE ? F("discesa") : F("salita (sync)"));
  Serial.print(F(" - periodo ")); Serial.print(PERIOD_MS); Serial.println(F(" ms"));
  Serial.print(F("# M_CAL = ")); Serial.print(M_CAL); Serial.print(F("   N_MIS = ")); Serial.print(N_MIS);
  Serial.print(F("   D_CAL = ")); Serial.print(D_CAL_M, 2); Serial.println(F(" m"));
  Serial.println(F("fase,n,id,ra_cyc,da_cyc,db_cyc,rb_cyc,tof_ss_ns,tof_ds_ns,rssi_dbm,lqi,resp_ricevuti,poll_inviati"));
  Serial.print(F("# Piano: distanze [m]:")); for (uint8_t g = 0; g < ND; g++) { Serial.print(' '); Serial.print(PLAN_DIST[g], 2); }
  Serial.print(F("  x ")); Serial.print(NPOS); Serial.println(F(" posizioni"));
  Serial.println(F("# >>> CALIBRAZIONE in corso: B a 1 m, centro"));

  cal.reset(); mis.reset(); calRssi.reset(); misRssi.reset(); clearHistory();
}

void loop() {
  if (state == ST_WAIT) {
    if (!startRequested()) return;
    phase++;
    mis.reset(); misRssi.reset();
    target = N_MIS;
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

  Packet poll = { PKT_POLL, id, 0, 0, 0 };
  uint8_t r = sendPacket((uint8_t*)&poll, t1);
  if (r != 0) { Serial.print(F("# POLL errore ")); Serial.print(r); Serial.print(F(" id ")); Serial.println(id); return; }
  sent++;

  r = listen();
  if (r == 0) r = waitCapture(RESP_WINDOW_US, t4);
  if (r != 0) {
    radio.standby();
    Serial.print(F("# RESP persa (")); Serial.print(r); Serial.print(F(") id ")); Serial.println(id);
    return;
  }
  Packet resp;
  int st = radio.readData((uint8_t*)&resp, sizeof(resp));
  if (st != RADIOLIB_ERR_NONE || resp.type != PKT_RESP || resp.id != id) {
    Serial.print(F("# RESP non valida id ")); Serial.print(id); Serial.print(F(" stato ")); Serial.println(st);
    return;
  }
  received++;
  float rssi = radio.getRSSI();
  uint8_t lqi = radio.getLQI();

  delayMicroseconds(FINAL_DELAY_US);
  Packet fin = { PKT_FINAL, id, 0, 0, 0 };
  r = sendPacket((uint8_t*)&fin, t5);

  uint8_t k = id % HIST;
  histValid[k] = false;
  if (r != 0) {
    Serial.print(F("# FINAL errore ")); Serial.print(r); Serial.print(F(" id ")); Serial.println(id);
  } else {
    histRa[k] = t4 - t1; histDa[k] = t5 - t4; histId[k] = id;
    histRssi[k] = rssi; histLqi[k] = lqi; histValid[k] = true;
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
    Serial.print(histRssi[j], 1);        Serial.print(',');
    Serial.print(histLqi[j]);            Serial.print(',');
    Serial.print(received);              Serial.print(',');
    Serial.println(sent);

    addSample(dsNs, histRssi[j]);
  }
}
