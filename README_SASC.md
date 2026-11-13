# MarranoFTP Plus 1.0 per AmigaOS con SAS/C 6.x

Tre programmi dallo stesso core FTP:

| Programma | GUI | Sistema |
|-----------|-----|---------|
| **MarranoFTP** (titolo "MarranoFTP Plus 1.0") | MUI 3.8 + NList | Kickstart 2.04+ |
| **MarranoFTP13GD** | GadTools emulata con GTE.lib | Kickstart 1.2/1.3+ |
| **MarranoFTPI** | solo intuition.library + graphics.library | Kickstart 1.2/1.3+ |

## Struttura: core FTP separato dalla GUI

| File | Contenuto |
|------|-----------|
| `core.h`, `ftp_core.c` | protocollo FTP, socket, coda, configurazione. Nessun codice di interfaccia, solo chiamate compatibili OS 1.3 (niente dos/exec V36, niente iffparse, niente floating point) |
| `gui.h` | il contratto che ogni GUI implementa (`gui_status`, `gui_set_list`, `gui_get_selection`, ...) |
| `gui_mui.c` | GUI MUI 3.8 + NList, usata da **MarranoFTP** (Kickstart 2.04+, AROS, MorphOS) |
| `gui_gt.c`, `gt_bases.c` | GUI GadTools tramite `GTE.lib` (emulazione di gadtools V37 di Massimo Tantignone, pubblico dominio, in `GTE/`), usata da **MarranoFTP13GD** |
| `gui_int.c` | GUI fatta solo con intuition e graphics, usata da **MarranoFTPI** |
| `gui_lines.c/h` | righe di testo delle liste, formattazione file/coda: comuni alle due GUI 1.3 |

Il core chiama la GUI solo attraverso `gui.h`; la GUI chiama il core solo attraverso
le funzioni `core_*` in fondo a `core.h`. Per aggiungere un'altra interfaccia (ReAction,
ClassAct, Triton, una versione a riga di comando, ...) basta un nuovo `gui_xxx.c`
che implementi `gui.h` e contenga `main()`.

`main.c` e `structs.h` non esistono più: il loro contenuto è stato diviso fra
`ftp_core.c`/`core.h` e `gui_mui.c`.

### MarranoFTP13GD (GadTools emulata, OS 1.3)

- Usa solo le funzioni `EF_` di `GTE.lib`: gira in emulazione sia su 1.3 sia su 2.x/3.x,
  `gadtools.library` non viene mai aperta. `LockPubScreen()` è chiamata solo con exec ≥ 36.
- Compilato con `DEFINE=MFTP_SMALL`: buffer ridotti (trasferimento 96 KB, LIST 48 KB,
  coda 1024 elementi, 1024 file per lista) per stare in un Amiga da 1 MB.
- Finestra principale a tutto schermo sul Workbench (si adatta a PAL/NTSC), font topaz 8:
  stato, percorsi locale/remoto modificabili, due liste file, coda, info trasferimento,
  14 pulsanti (Connect, Stop, Download, Upload, Delete, Parent L/R, Refresh L/R,
  Run queue, Step, Clear queue, Sites, Quit). Menu Project: Address book, Settings,
  Save config, About, Quit.
- Liste file: **un click marca/smarca** una riga (`*`), così si possono scegliere più file
  per Download/Upload/Delete; **doppio click** entra nella directory o trasferisce il file.
  Se non c'è niente marcato si usa l'ultima riga cliccata.
- Rubrica (finestra separata): lista host, Nome/Host/Port/User/Password, Active/Passive,
  New/Apply/Delete/Connect/Save cfg/Close; doppio click su un host = connetti.
- Settings: timeout trasferimento e connessione, comportamento coda, file parziali.
- Lo stato connesso/non connesso è nel titolo della finestra (i pulsanti GadTools non
  possono cambiare testo).
- `IntuitionBase`/`GfxBase` sono in `gt_bases.c` come `__near`: `GTE.lib` è compilata con
  dati near e le referenzia relative ad A4 (altrimenti slink Error 510).

### MarranoFTPI (solo intuition + graphics, OS 1.3)

Stessa disposizione e stessi comandi di MarranoFTP13GD, senza nessuna libreria di
emulazione (72 KB contro 115 KB):

- pulsanti: gadget booleani con due `Border` (effetto rilievo) e `IntuiText`, evidenziazione
  `GADGHCOMP`; i "cycle" (Active/Passive, coda, file parziali) sono pulsanti che cambiano testo;
- campi di testo: gadget stringa con cornice incassata (Border a -4,-2);
- liste: disegnate con `Text()` (topaz 8 aperto con `OpenFont()`), barra di scorrimento con
  gadget proporzionale `AUTOKNOB|FREEVERT` aggiornato con `NewModifyProp()` e frecce con
  autoripetizione su `INTUITICKS`; click e doppio click letti da `MOUSEBUTTONS` +
  `DoubleClick()`; la riga cliccata è evidenziata;
- menu costruito a mano con `Menu`/`MenuItem`/`IntuiText` e scorciatoie Amiga-A/S/W/Q;
- colori scelti in base al sistema: su 1.3 testo bianco su blu e evidenziazione arancione,
  su 2.0+ i pen di sistema (nero su grigio, evidenziazione blu);
- solo funzioni V33 (1.2): `OpenWindow`, `AddGList`, `RefreshGList`, `NewModifyProp`,
  `GetScreenData`, `SetMenuStrip`, `ActivateGadget`...

## Requisiti

- SAS/C 6.5x (verificato con 6.58 + patch Y2K)
- NDK 3.x (include in `INCLUDE:`, verificato con NDK 3.2 R4)
- MUI developer include (solo per MarranoFTP) (verificato con MUI 3.8) e le include di NList/NListview
  (`INCLUDE:MUI/NList_mcc.h`, ...; verificato con MCC_NList 0.128, non fanno parte dell'SDK di MUI)
- SDK di Roadshow (netinclude dell'NDK 3.2) o di AmiTCP 4.3, con l'assign `NETINCLUDE:`

## Build verificata

Compilato con il vero SAS/C 6.58 (eseguito in emulazione con vamos):

| NETINCLUDE:          | Errori | Warning | Eseguibile |
|----------------------|--------|---------|------------|
| Roadshow (NDK 3.2)   | 0      | 0       | 79900 byte |
| AmiTCP SDK 4.3       | 0      | 0       | 79616 byte |

`INCLUDE:` era l'unione, in quest'ordine, di NDK 3.2 `Include_H`, MUI 3.8 `C/Include`,
NList 0.128 `Developer/C/include` e `SC:include`.

Entrambi gli eseguibili usano `bsdsocket.library` a runtime, quindi funzionano con
qualunque stack (Roadshow, AmiTCP, Miami, Genesis): l'SDK conta solo in compilazione.

## Test su AmigaOS 3.1

Provato in FS-UAE (A1200, Kickstart 3.1 40.68, Workbench 3.1, MUI 3.8, NList 0.102,
bsdsocket.library emulata da UAE) contro un server FTP locale:

- avvio, configurazione assente e presente (`PROGDIR:marranoftp.moo`), address book;
- connessione passiva e attiva, LIST, download e upload binari (MD5 identici);
- uscita con Quit da disconnessi, Quit da connessi (`QUIT`/`221`) e gadget di
  chiusura da connessi: sempre RC=0, nessun Guru.

## Compilazione

```
smake                 ; tutti e tre
smake MarranoFTP      ; MUI
smake MarranoFTP13GD  ; OS 1.3, GadTools emulata
smake MarranoFTPI     ; OS 1.3, intuition/graphics
```

Le opzioni del compilatore sono in `SCOPTIONS`:

- `DATA=FAR` è **obbligatoria**: `g_Connection` da solo occupa circa 300 KB e non entra nella sezione dati near (64 KB).
- `MATH=IEEE` è rimasta ma non serve più: il core misura i tempi in secondi + microsecondi interi (su 1.3 non ci sono le librerie matematiche in ROM).
- Per la build di debug aggiungi `DEFINE=DEBUG` alle righe `sc` dello `smakefile`.

## Cosa fa il codice quando è definito `__SASC` (che SAS/C definisce da solo)

- `DOSBase` e `SysBase` restano dello startup code (`c.o`): il programma non le apre, non le azzera e non le chiude. Era questa la causa del crash all'uscita.
- `MUIMasterBase` e `IntuitionBase` sono definite in `gui_mui.c` (`IntuitionBase`/`GfxBase` in `gt_bases.c` per MarranoFTP13GD e in `gui_int.c` per MarranoFTPI), così il linker non aggiunge gli stub di auto-open di `sc.lib`.
- `long __near __stack = 32768;` (16384 nelle versioni 1.3): lo stack di default della shell (4 KB) non basta.
- `SetErrnoPtr(&errno, sizeof(errno))` dopo l'apertura di `bsdsocket.library`. Senza, `errno` non viene mai aggiornato e ogni `connect()` non bloccante risulta fallita.
- Gli hook MUI sono funzioni `__saveds __asm` con i parametri nei registri A0/A2/A1, assegnate direttamente a `h_Entry`. Non serve `HookEntry` di amiga.lib e funzionano sia con `PARAMETERS=STACK` sia con `PARAMETERS=REGISTER`.
- `snprintf()`/`vsnprintf()` vengono mappate su `caf_snprintf()`/`caf_vsnprintf()` (`CafLib/stdio_helpers.c`), perché la `sc.lib` di SAS/C 6.x non le ha.
- `IPTR` è definito come `ULONG`; `CPU_BIG_ENDIAN` è definito, così il file di configurazione resta compatibile con le altre versioni.
- La tabella degli errori in `CafLib/errors.c` usa i codici BSD numerici di `bsdsocket.library`.
- `PROTO_TIMER_H` viene definito prima di ogni include, e al posto di `proto/timer.h`
  si usano `clib/timer_protos.h` + `pragmas/timer_pragmas.h`; `TimerBase` è una
  `struct Library *`. Serve perché AmiTCP 4.3 e NDK 3.2 dichiarano `TimerBase` con
  tipi diversi (SAS/C Error 72).
- `__stack` è `__near` perché, anche con `DATA=FAR`, lo startup `c.o` lo legge in modo
  near (altrimenti slink dà Error 510).

Tutto il codice specifico per piattaforma è in `CafLib/include/caf/caf_platform.h`,
più i blocchi `#ifdef __SASC` in `ftp_core.c`, `gui_mui.c` e in `CafLib/*.c`.

## Verifiche della versione separata (13/11/2026)

Compilazione con SAS/C 6.58 reale (vamos), SDK Roadshow: 0 errori, 0 warning.
MarranoFTP 71316 byte, MarranoFTP13 (ora MarranoFTP13GD) 114908 byte (~40 KB sono l'emulazione GadTools).

| Prova | MarranoFTP (MUI) | MarranoFTP13 (GadTools) |
|-------|------------------|-------------------------|
| KS 3.1 A1200 + TheWire13 + slipbridge: connessione, LIST | ok | ok |
| KS 3.1: download 20000 byte + upload, MD5 identico all'originale | ok | ok |
| KS 3.1: Quit da connesso (`QUIT`/`221`), RC=0 | ok | ok |
| KS 1.3 A500: avvio, configurazione da `S:`, rubrica, settings | - | ok |
| KS 1.3 A500: connessione, login, PWD, LIST passivo, doppio click → RETR | - | ok |
| KS 1.3 A500: download completo | - | **lentissimo, vedi sotto** |

Su A500/KS 1.3 emulato il trasferimento dei dati è bloccato dal collegamento
seriale, non dal programma: `slipstat` mostra frame SLIP da 1064 byte che arrivano
con byte mancanti (1058 byte, senza il primo `0x45`), cioè overrun della porta seriale
con serial.device 34 su 68000 a 7 MHz. I pacchetti piccoli (comandi, risposte, LIST)
passano, quelli grandi quasi mai, anche rallentando l'host a 1200 byte/s. Lo stesso
binario su KS 3.1 trasferisce senza problemi. Su un A500 vero con una scheda di rete
SANA-II (o con un MTU/MSS più piccolo nello stack) il problema non dovrebbe presentarsi.

## Verifiche di MarranoFTP Plus 1.0, MarranoFTP13GD e MarranoFTPI (13/11/2026)

SAS/C 6.58, SDK Roadshow: 0 errori, 0 warning. MarranoFTP 71332 byte,
MarranoFTP13GD 115228 byte, MarranoFTPI 72340 byte.

| Prova | MarranoFTP | 13GD | I |
|-------|-----------|------|---|
| KS 3.1: connessione, download + upload 20000 byte, MD5 identico, Quit RC=0 | ok | ok | ok |
| KS 3.1: titolo "MarranoFTP Plus 1.0 (13.11.2026)" | ok | - | - |
| KS 1.3: avvio, rubrica, connessione, LIST, CWD con doppio click | - | ok | ok |
| KS 1.3: menu, finestra settings, pulsanti a scelta, Quit (QUIT/221) | - | ok | ok |

Il download completo su A500/KS 1.3 resta limitato dalla seriale emulata (vedi sopra).
