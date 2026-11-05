# Radiolocalizzazione indoor con DS-TWR su radio low-cost

Codice della tesi di laurea triennale in Ingegneria Informatica *"Sperimentazione di algoritmi di radiolocalizzazione in ambienti indoor"* (Politecnico di Bari, Fondamenti di Telecomunicazioni).

Il progetto misura la distanza tra due nodi radio con due tecniche:

- **DS-TWR** (*Double-Sided Two-Way Ranging*): stima il tempo di volo (ToF) del segnale scambiando tre pacchetti (POLL, RESP, FINAL) e misurando gli intervalli con un timer hardware;
- **RSSI**: stima la distanza dalla potenza ricevuta con il modello *log-distance* (solo CC1101).

Le prove sono state svolte con due coppie di moduli: **CC1101 a 433 MHz** e **nRF24L01+ PA+LNA a 2,4 GHz**, entrambe pilotate da **Arduino UNO R4 Minima**.

---

## Contenuto del repository

```
firmware/
├── cc1101/
│   ├── cc_auto_A/cc_auto_A.ino     nodo A (iniziatore) - 433 MHz
│   └── cc_auto_B/cc_auto_B.ino     nodo B (risponditore) - 433 MHz
└── nrf24/
    ├── nrf_pos8_A/nrf_pos8_A.ino   nodo A (iniziatore) - 2,4 GHz
    └── nrf_pos8_B/nrf_pos8_B.ino   nodo B (risponditore) - 2,4 GHz
matlab/
└── analisi_distanze.m              post-elaborazione dei dati registrati
```

---

## 1. Hardware necessario

| Componente | Quantità | Note |
|---|---|---|
| Arduino UNO R4 Minima | 2 | uno per nodo |
| EBYTE E07-M1101D-SMA (CC1101, 433 MHz) | 2 | con antenna SMA |
| Level shifter TXS0108E | 2 | adatta i 5 V di Arduino ai 3,3 V del CC1101 |
| nRF24L01+ PA+LNA (2,4 GHz) | 2 | con antenna SMA |
| Modulo di alimentazione MB102 | 2 | fornisce i 3,3 V all'nRF24L01+ |
| Pulsante | 1 | solo sul nodo A |
| Powerbank USB | 1 | alimenta il nodo B |
| Cavo USB-C | 2 | |

## 2. Collegamenti

Il timestamp viene preso dal **timer GPT1** del microcontrollore, in cattura hardware sul **pin D2**: il filo su D2 è quello che conta per la precisione della misura.

### CC1101 (attraverso il TXS0108E)

| CC1101 | Arduino |
|---|---|
| SCK | D13 |
| MOSI (SI) | D11 |
| MISO (SO) | D12 |
| CSN | D10 |
| GDO0 | **D2** |
| VCC / GND | 3,3 V / GND |

### nRF24L01+ PA+LNA

| nRF24L01+ | Arduino |
|---|---|
| SCK | D13 |
| MOSI | D11 |
| MISO | D12 |
| CSN | D10 |
| CE | D9 |
| IRQ | **D2** |
| VCC | 3,3 V dal MB102 |
| GND | GND (in comune tra MB102 e Arduino) |

### Pulsante (solo nodo A)

Tra **D4** e **GND**. Non serve resistenza: si usa il pull-up interno.

## 3. Software

1. Installa l'**Arduino IDE** (2.x).
2. Da *Gestione schede* installa **Arduino UNO R4 Boards**.
3. Da *Gestione librerie* installa:
   - **RadioLib** (Jan Gromeš), per il CC1101;
   - **RF24** (TMRh20), per l'nRF24L01+.
4. Per l'analisi: **MATLAB R2020a** o successivo. Non servono toolbox.

## 4. Configurazione del firmware

I parametri si trovano nelle prime righe di ogni sketch. **I parametri radio devono essere identici su A e B.**

### CC1101

| Parametro | Significato |
|---|---|
| `MODULATION` | 0 = 2-FSK, 1 = GFSK, 2 = OOK, 3 = 4-FSK, 4 = MSK |
| `BITRATE_SEL` | 250 o 500 kbps (a 500 kbps solo 2-FSK, GFSK, MSK) |
| `M_CAL`, `N_MIS` | campioni di calibrazione e di misura (default 5000) |
| `PERIOD_MS` | periodo tra due scambi (default 15 ms) |
| `PATH_LOSS_N` | esponente di path loss per la stima RSSI a bordo |
| `PLAN_DIST`, `POS_NAME` | distanze e posizioni del piano di misura |

### nRF24L01+

| Parametro | Significato |
|---|---|
| `CHANNEL` | canale radio (80 = 2480 MHz) |
| `DATA_RATE` | velocità (default 2 Mbps) |
| `PA_LEVEL` | potenza di trasmissione (B la copia dal POLL) |
| `M_CAL`, `N_MIS` | campioni di calibrazione e di misura (default 5000 / 2000) |
| `PERIOD_MS` | periodo tra due scambi (default 10 ms) |
| `PLAN_DIST`, `POS_NAME` | distanze e posizioni del piano di misura |

Il piano di misura contiene al massimo 16 misure e almeno 2 posizioni per distanza.

## 5. Caricamento

1. Carica lo sketch **B** sulla prima scheda e alimentala con il powerbank.
   Il LED integrato **cambia stato a ogni scambio completato**; se **lampeggia velocemente** c'è un errore all'avvio (radio non trovata o autotest della cattura fallito).
2. Carica lo sketch **A** sulla seconda scheda, collegata al computer.

All'avvio entrambi i nodi eseguono un **autotest**: verificano che il fronte del segnale (GDO0 o IRQ) arrivi davvero al timer su D2.

## 6. Sessione di misura

La sessione è automatica e si comanda con il solo pulsante del nodo A.

1. **Calibrazione.** Metti i nodi a **1 m** di distanza. La calibrazione parte appena il terminale apre la porta seriale e raccoglie `M_CAL` scambi validi.
2. Al termine il nodo A stampa il riassunto e **si ferma**, dicendo dove mettere B per la misura successiva.
3. **Misura.** Sposta B nella posizione indicata e premi il pulsante (oppure invia un carattere qualsiasi sulla seriale). A fine misura viene stampata la stima della distanza con l'intervallo di confidenza al 95%.
4. Ripeti per tutte le posizioni del piano. Finite le posizioni di una distanza viene stampato il riassunto della distanza; finito il piano, ogni pressione esegue una misura libera, fuori piano.
5. Per ricalibrare basta premere **reset**.

### Registrare i dati

La registrazione non si interrompe mai: tutta la sessione finisce in un unico file. Su macOS o Linux, ad esempio:

```bash
stty -f /dev/cu.usbmodemXXXX 115200       # su Linux: stty -F /dev/ttyACM0 115200
cat /dev/cu.usbmodemXXXX | tee prova.csv
```

Si chiude con **Ctrl+C**. Sostituisci `XXXX` con il nome della porta (`ls /dev/cu.usbmodem*`). Va bene anche qualsiasi terminale seriale che salvi l'output su file.

### Formato del file

Righe di dati (una per scambio):

```
CC1101:  fase,n,id,ra_cyc,da_cyc,db_cyc,rb_cyc,tof_ss_ns,tof_ds_ns,rssi_dbm,lqi,resp_ricevuti,poll_inviati
nRF24:   fase,n,id,ra_cyc,da_cyc,db_cyc,rb_cyc,tof_ss_ns,tof_ds_ns,rpd,resp_ricevuti,poll_inviati
```

- `fase` = 0 per la calibrazione, *k* per la misura *k*;
- `ra/da/db/rb_cyc` = intervalli del DS-TWR in cicli del timer (48 MHz);
- `tof_ds_ns` = tempo di volo DS-TWR del singolo scambio, in ns.

Le righe che iniziano con `#` sono commenti e riassunti; tra queste `#PIANO,k,distanza_m,posizione` indica la posizione di ogni misura ed è l'unica usata da MATLAB.

## 7. Calcolo della distanza

Per ogni scambio:

$$\tau = \frac{R_a R_b - D_a D_b}{R_a + R_b + D_a + D_b}\cdot\frac{1}{f_{clk}}$$

dove $R$ sono i tempi di andata e ritorno e $D$ i tempi di risposta di ciascun nodo. La distanza si ottiene confrontando il ToF medio con quello di calibrazione, che viene trattato come una costante:

$$\hat d = d_0 + c\,(\bar\tau - \bar\tau_{cal}), \qquad d_0 = 1\ \text{m}$$

Per l'RSSI (modello *log-distance*):

$$\hat d = d_0 \cdot 10^{\,(\bar P_{cal} - \bar P)/(10\,n)}$$

## 8. Analisi con MATLAB

1. Organizza i file registrati in cartelle per banda, ad esempio:
   ```
   campagna dati/
   ├── 433/senza ostacoli/gfskNoOb.csv
   ├── 433/con ostacoli/...
   └── 2,4/senza ostacoli/...
   ```
2. In `analisi_distanze.m` imposta:
   - `cartella`: la cartella dei dati (di default `~/Desktop/campagna dati`);
   - `files`: l'elenco delle prove da analizzare, con il percorso relativo a `cartella`;
   - `opt`: livello di confidenza (0,95), numero di blocchi per le medie a blocchi (20), soglia degli outlier (6·MAD), esponente di path loss (2,65 in laboratorio, 2 in camera anecoica).
3. Esegui lo script.

Le prove con `CA` nel nome sono trattate come prove in camera anecoica (n = 2).

### Cosa fa lo script

- scarta gli outlier con un filtro basato sulla mediana (MAD);
- calcola la media di calibrazione del ToF (e dell'RSSI);
- per ogni posizione stima la distanza e l'intervallo di confidenza al 95% con la *t* di Student, stimando la varianza della media con il **metodo delle medie a blocchi** per tenere conto della correlazione tra campioni successivi;
- per ogni distanza combina le posizioni (*t* con P−1 gradi di libertà);
- genera i grafici e le tabelle.

### Risultati

Tutto viene salvato in `cartella/figure/`:

```
figure/
├── <banda>_<prova>/
│   ├── distribuzioni_ToF/     istogrammi del ToF con gaussiane
│   ├── probabilita_ToF/       carte di probabilità normale
│   ├── distribuzioni_RSSI/    (solo CC1101)
│   ├── probabilita_RSSI/      (solo CC1101)
│   ├── stime/                 distanze stimate con IC al 95%
│   ├── risultati_posizioni.csv
│   ├── risultati_distanze.csv
│   └── tabella_distanze.tex   tabella LaTeX (richiede booktabs)
├── risultati_posizioni_tutte.csv
└── risultati_distanze_tutte.csv
```

I grafici sono PDF vettoriali, pronti per essere inclusi in LaTeX.

---

## Autore

Daniele Petti, Politecnico di Bari
Relatori: Prof. L. A. Grieco, Prof. G. Boggia
Correlatore: Dott. R. Semeraro
