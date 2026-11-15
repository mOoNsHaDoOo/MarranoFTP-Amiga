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
| `ssh.c`, `sftp.c`, `core_sftp.c` | SFTP (vedi sotto) |
| `tls.c` | FTPS (vedi sotto) |
| `ftp_list.c/h` | parser delle risposte a LIST (Unix, DOS/IIS, EPLF, Netware...), C puro |
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

## SFTP

I tre programmi parlano anche SFTP: nella rubrica il tipo di connessione è
Active / Passive / **SFTP** / FTPS / FTPS implicit (porta di solito 22 per SFTP). Con un host SFTP lista, cambio
directory, Parent, download, upload, delete, makedir e coda funzionano come con FTP.

| File | Contenuto |
|------|-----------|
| `ssh.c/h` | client SSH-2: scambio chiavi `curve25519-sha256` e `diffie-hellman-group14-sha256/sha1`, chiavi host `ssh-ed25519`, `rsa-sha2-256/512`, `ssh-rsa`, cifratura `aes128-ctr`/`aes256-ctr`, MAC `hmac-sha2-256`/`hmac-sha1`, login con password o keyboard-interactive, rekey, strict KEX (contromisura Terrapin) |
| `sftp.c/h` | SFTP versione 3, con 4 richieste READ/WRITE da 16 KB in volo; i trasferimenti avanzano un blocco per giro del ciclo principale, la GUI resta reattiva |
| `ssh_crypto.h` | le funzioni OpenSSL usate, dichiarate a mano per SAS/C + pragmas di AmiSSL (gli header di OpenSSL 3 non sono leggibili da SAS/C 6) |
| `core_sftp.c/h` | apertura di AmiSSL, socket con timeout, chiavi host note |
| `tests/test_sftp_linux.c` | prova degli stessi `ssh.c`/`sftp.c` su Linux con OpenSSL |

- **Requisiti a runtime**: AmigaOS 3.0+, 68020+, **AmiSSL 5.25 o più recente** installato
  (`LIBS:amisslmaster.library`, `LIBS:AmiSSL/amissl_v3xx.library`, assign `AmiSSL:`),
  circa **3,5 MB di memoria libera** per AmiSSL. AmiSSL viene aperto solo alla prima
  connessione SFTP: senza AmiSSL (o su OS 1.3) i programmi funzionano lo stesso e
  segnalano solo che SFTP non è disponibile.
- **Chiavi host**: al primo collegamento il fingerprint (`SHA256:...`, lo stesso di
  `ssh-keygen -l`) viene salvato in `PROGDIR:marranoftp.hosts` (o `S:`); se in seguito
  cambia, la connessione viene rifiutata con un messaggio esplicito. Per accettare una
  chiave nuova basta togliere la riga corrispondente dal file.
- **Non supportati**: autenticazione con chiave pubblica, compressione, file oltre 4 GB
  (SAS/C non ha interi a 64 bit; la dimensione viene mostrata come `4GB+`).
- **Memoria della versione MUI**: MarranoFTP alloca circa 3,8 MB di buffer FTP, che
  con AmiSSL superano gli 8 MB di Fast RAM di un A1200 base. Su un A1200 emulato
  con 8 MB SFTP funziona con MarranoFTPI e MarranoFTP13GD (buffer ridotti); la versione
  MUI è stata provata con 64 MB.

### Prove SFTP (14/11/2026)

Su Linux contro OpenSSH 9.6 (`tests/test_sftp_linux.c`): tutte le combinazioni di
algoritmi (curve25519, DH group14 sha256/sha1, Ed25519, RSA sha2-256/512 e ssh-rsa,
aes128/256-ctr, hmac-sha2-256/sha1), keyboard-interactive via PAM, rekey ogni 64 KB,
password errata, server che accetta solo chiavi pubbliche, download/upload di 150 KB
con MD5 identico, mkdir, rename, remove, rmdir.

In FS-UAE, A1200 KS 3.1, AmiSSL 5.27, TheWire13 + slipbridge verso lo stesso sshd:

| Prova | MarranoFTP | 13GD | I |
|-------|-----------|------|---|
| connessione, chiave host salvata e poi riconosciuta, lista | ok (64 MB) | ok | ok |
| download + upload 20000 byte, MD5 identico | - | - | ok |
| cambio directory, Parent, delete | - | - | ok |
| coda di due download | - | ok | - |
| Disconnect / Quit (`disconnect 11: bye`, RC=0) | ok | ok | ok |

## FTPS

Nella rubrica ci sono anche **FTPS** (esplicito: `AUTH TLS` sulla porta FTP, di solito 21)
e **FTPS implicit (990)** (TLS subito dopo la connessione). Comandi, password e connessioni
dati sono cifrati (`PBSZ 0` + `PROT P`); le connessioni dati sono sempre passive.

| File | Contenuto |
|------|-----------|
| `tls.c/h` | TLS con AmiSSL (TLS 1.2 minimo); AmiSSL non tocca i socket: due BIO in memoria e gli stessi `send()`/`recv()` dell'FTP in chiaro, quindi funziona con qualunque stack e con i socket non bloccanti del core |
| `tests/test_ftps_linux.c` | prova di `tls.c` su Linux contro vsftpd |

- **Connessione dati**: l'handshake TLS si fa quando il server risponde 150/125 a
  `LIST`/`RETR`/`STOR`, riusando la sessione TLS del canale comandi (vsftpd e altri server
  lo esigono, `require_ssl_reuse`); prima di allora sulla connessione dati non passa nulla.
- **Certificati**: catena verificata con i certificati CA di AmiSSL (`AmiSSL:Certs`) e
  nome dell'host. Se la verifica non riesce (certificato self-signed, indirizzo IP, CA
  sconosciuta) si usa lo stesso "trust on first use" delle chiavi SSH: il fingerprint
  `SHA256:...` viene salvato in `marranoftp.hosts` (tipo `x509`) e un certificato diverso
  in seguito viene rifiutato.
- **Nessun ripiego in chiaro**: se il server rifiuta `AUTH TLS` la connessione viene chiusa
  prima di inviare la password; se rifiuta solo `PROT P` si prosegue con i dati in chiaro
  e un avviso.
- I dati già decifrati che TLS tiene in memoria vengono considerati dal ciclo principale
  (che altrimenti aspetterebbe in `WaitSelect()`), e un record TLS ricevuto a metà conta
  come attività per il timeout di trasferimento (importante sulle linee lente).
- Alla chiusura di una connessione TLS si attende il `close_notify` del server leggendo
  quello che resta: chiudere un socket con dati non letti (per esempio i session ticket
  TLS 1.3) manda un RST e il server può perdere la fine di un upload.

### Prove FTPS (14/11/2026)

Su Linux contro vsftpd 3.0.5 (`require_ssl_reuse=YES`, `force_local_data_ssl=YES`,
certificato self-signed), con `tests/test_ftps_linux.c`: esplicito e implicito, tre giri
ciascuno, download + upload + LIST, MD5 identico e trasferimenti completi nel log del server.

In FS-UAE, A1200 KS 3.1, AmiSSL 5.27, TheWire13 + slipbridge, verso gli stessi vsftpd:

| Prova | MarranoFTP (MUI, 64 MB) | MarranoFTPI |
|-------|------------------|-------------|
| FTPS esplicito: AUTH TLS, login, PBSZ/PROT P, LIST cifrata | ok | ok |
| FTPS esplicito: download + upload 20000 byte, MD5 identico | - | ok |
| FTPS implicito: login, LIST, download | - | ok |
| certificato self-signed: avviso + fingerprint salvato | ok | ok |
| Quit (`QUIT`/`221` sul canale TLS), RC=0 | ok | ok |
| FTP in chiaro subito dopo una sessione FTPS | ok | - |

Senza l'assign `AmiSSL:` (installazione incompleta) non compaiono richieste "inserisci il
volume": la verifica con le CA fallisce e si passa al fingerprint.

## Requisiti

Per compilare:

- SAS/C 6.5x (verificato con 6.58 + patch Y2K)
- NDK 3.x (verificato con NDK 3.2 R4, cartella `Include_H`)
- SDK di Roadshow (le netinclude dell'NDK 3.2) oppure AmiTCP SDK 4.3
- solo per **MarranoFTP**: MUI developer (verificato con MUI 3.8, `Developer/C/Include`)
  e le include di NList/NListview (verificato con MCC_NList 0.128, `Developer/C/include`,
  non fanno parte dell'SDK di MUI)
- per **MarranoFTP13GD**: `GTE/GTE.lib` e `GTE/nl_gadtools.h`, già nel progetto
- per **MarranoFTPI**: niente oltre all'NDK
- per SFTP (tutti e tre): la cartella `Developer/include` dell'SDK di AmiSSL 5.25+ in `INCLUDE:`
  (servono solo `pragmas/` e `libraries/amisslmaster.h`)

Per eseguire:

| Programma | Serve |
|-----------|-------|
| MarranoFTP | Kickstart 2.04+, MUI 3.8+, NList/NListview, uno stack TCP/IP con `bsdsocket.library` |
| MarranoFTP13GD | Kickstart 1.2+, uno stack con `bsdsocket.library` (su 1.3 TheWire13) |
| MarranoFTPI | Kickstart 1.2+, uno stack con `bsdsocket.library` (su 1.3 TheWire13) |

Tutti usano `bsdsocket.library` solo a runtime, quindi funzionano con qualunque stack
(Roadshow, AmiTCP, Miami, Genesis, TheWire13): l'SDK scelto conta solo in compilazione.
La configurazione (`marranoftp.moo`) è cercata in `MARRANO:`, `PROGDIR:` (solo 2.0+) e `S:`.

## Assign per compilare

| Assign | Punta a |
|--------|---------|
| `INCLUDE:` | NDK `Include_H` + `SC:include` + AmiSSL SDK `Developer/include` (e, per MarranoFTP, MUI `C/Include` + NList `C/include`) |
| `LIB:` | `SC:lib` |
| `NETINCLUDE:` | netinclude di Roadshow o di AmiTCP (è in `SCOPTIONS` come `INCLUDEDIR`) |

`CafLib/include` è già in `SCOPTIONS`, non serve un assign. Esempio su un Amiga:

```
Assign INCLUDE: NDK:Include_H SC:include MUI:Developer/C/Include NList:Developer/C/include AmiSSL-SDK:Developer/include
Assign NETINCLUDE: NDK:SANA+RoadshowTCP-IP/netinclude   ; oppure AmiTCP-SDK-4.3/netinclude
```

## Compilazione

```
smake                 ; tutti e tre
smake MarranoFTP      ; MUI
smake MarranoFTP13GD  ; OS 1.3, GadTools emulata
smake MarranoFTPI     ; OS 1.3, intuition/graphics
smake clean
```

`ftp_core.c` è compilato due volte: `ftp_core.o` per MarranoFTP e `ftp_core13.o` con
`DEFINE=MFTP_SMALL` per le due versioni 1.3, che hanno buffer più piccoli.

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

## Verifiche (13/11/2026)

Compilazione con il vero SAS/C 6.58 (eseguito con vamos), SDK Roadshow: 0 errori, 0 warning.

| Programma | Dimensione |
|-----------|------------|
| MarranoFTP | 71332 byte |
| MarranoFTP13GD | 115228 byte (circa 40 KB sono l'emulazione GadTools) |
| MarranoFTPI | 72340 byte |

Prove in FS-UAE contro un server FTP locale, con TheWire13 + slipbridge.device:

| Prova | MarranoFTP | 13GD | I |
|-------|-----------|------|---|
| KS 3.1 A1200: connessione, LIST, download + upload di 20000 byte con MD5 identico all'originale, Quit (`QUIT`/`221`) con RC=0 | ok | ok | ok |
| KS 3.1: titolo "MarranoFTP Plus 1.0 (13.11.2026)" | ok | - | - |
| KS 1.3 A500: avvio, configurazione da `S:`, rubrica, connessione, LIST, doppio click | - | ok (*) | ok |
| KS 1.3 A500: menu, finestra settings, pulsanti a scelta, Quit | - | ok (*) | ok |
| KS 1.3 A500: download completo | - | vedi sotto | vedi sotto |

(*) provato su 1.3 prima che le righe delle liste passassero in `gui_lines.c`;
la build attuale di MarranoFTP13GD è stata riprovata su KS 3.1.

Con la versione precedente monolitica (prima della separazione) erano state provate
anche, su KS 3.1 con la bsdsocket.library di UAE, la modalità attiva e la chiusura
dal gadget di chiusura da connessi.

### Formati della risposta a LIST

Il parser è in `ftp_list.c` (C puro, senza dipendenze Amiga, collaudabile su qualunque
macchina) e segue l'approccio di FileZilla: nelle righe Unix cerca la data, la dimensione
è il numero che la precede e il nome è quello che la segue, quindi non importa quanti campi
utente/gruppo ci sono o se manca il contatore dei link. Riconosce:

| Formato | Esempio |
|---------|---------|
| Unix (con o senza gruppo, ACL `+`) | `drwxr-xr-x 2 user group 4096 Nov 13 10:00 nome` |
| Unix, mesi in italiano/tedesco/francese/spagnolo (ISO-8859-1 e UTF-8) | `-rw-r--r-- 1 user group 500 ott 13 10:00 nome` |
| Unix, giorno prima del mese | `-rw-r--r-- 1 user group 500 13 nov 2025 nome` |
| Unix, data ISO | `-rw-r--r-- 1 user group 500 2026-11-13 10:00 nome` |
| link simbolici (mostrati come directory) | `lrwxrwxrwx 1 root root 12 Nov 13 10:00 www -> /web/htdocs` |
| Netware | `d [RWCEAFMS] admin 512 Nov 13 10:00 nome` |
| DOS/IIS | `11-13-26  03:00PM  <DIR>  nome`, `2026-11-13  15:00  1234 nome` |
| EPLF | `+i8388621.48594,m825718503,r,s280,<TAB>nome` |

Vengono tolti i segni di `ls -F` in fondo ai nomi (`/`, `*`, `|`) e ignorate le righe
`total`, `.` e `..`. Se una riga non è riconosciuta, nello stato compare la prima in chiaro.
Non c'è (ancora) MLSD: richiede di inviare il comando `MLSD` quando il server lo annuncia in `FEAT`.

Provato con server di test che rispondono in formato DOS e con tutti i formati mescolati:
la lista è completa, i nomi passati a `RETR`/`CWD` sono esatti, MD5 identico.

### Download su A500/KS 1.3 emulato

Su A500/KS 1.3 emulato il trasferimento dei dati è bloccato dal collegamento
seriale, non dal programma: `slipstat` mostra frame SLIP da 1064 byte che arrivano
con byte mancanti (1058 byte, senza il primo `0x45`), cioè overrun della porta seriale
con serial.device 34 su 68000 a 7 MHz. I pacchetti piccoli (comandi, risposte, LIST)
passano, quelli grandi quasi mai, anche rallentando l'host a 1200 byte/s. Gli stessi
binari su KS 3.1 trasferiscono senza problemi. Su un A500 vero con una scheda di rete
SANA-II (o con un MTU/MSS più piccolo nello stack) il problema non dovrebbe presentarsi.
