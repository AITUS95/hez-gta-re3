# Modalità agente — branch `police`

Avviare una **nuova partita**, con i normali asset PC di GTA III e il suo
`data/main.scm` originale. Il branch non installa un SCM sostitutivo e non usa
`main_freeroam.scm`, `main_d.scm`, cheat o il caricatore debug per la modalità.

## Avvio e mondo

Il normale interprete esegue MAIN e la sua inizializzazione di traffico, pedoni,
generatori, pickup, garage, oggetti dinamici e thread ambientali. `CREATE_PLAYER`
crea il normale `CPlayerPed` a Portland, presso la centrale, con il modello
`MI_COP`. Al posto del lancio della missione 0 (INTRO), `BeginShift` completa
l'equipaggiamento, lo sblocco e il passaggio alla camera/controlli normali.
MAIN continua a funzionare. Vigilante (`COPCAR`, missione 13 del MAIN retail)
usa nuovamente il caricatore, i trigger, gli obiettivi, i premi e la pulizia
missione vanilla. Storia e altre missioni dei veicoli restano bloccate.

Le due volanti di Portland usano i generatori originali del MAIN, senza
aggiungerne altri. I due stalli possono generare anche quando il giocatore
inizia entro il normale raggio di esclusione di 100 metri, mantenendo almeno
8 metri di distanza e i controlli vanilla di spazio libero e streaming.
Gli stalli restano disponibili anche se il limite ambientale di auto parcheggiate
è già raggiunto. Le portiere sono sbloccate per il servizio. Handle del veicolo,
attesa dopo il prelievo e ripristino al ritorno restano gestiti da `CCarGenerator`.

Lo sblocco usa `CStats::IndustrialPassed`, `CommercialPassed`, `SuburbanPassed`,
i metodi originali di `CPathFind` e le entità delle barriere di progressione
create dal MAIN. Le barriere di ponti, tunnel, metropolitana e aree recintate
vincolate alla storia perdono visibilità **e collisione**, conservando gli handle
SCM. Il ponte levatoio mantiene il suo ciclo vanilla. Non vengono inventati
missioni completate, pacchetti raccolti o un falso valore statistico del 100%.
I thread originali `C_RSTRT` e `S_RSTRT` impostano i rispettivi flag di isola
aperta quando ne leggono l'operando; `I_SAVE` può aprire il rifugio di Portland
senza completare Luigi's Girls. Gli altri controlli di radar, salvataggio e
missione rimangono originali. I respawn mantengono disponibili tutti e sei i
livelli Wanted per gli NPC, anche a Portland.

La camminata deriva dal gruppo animazioni del modello `MI_COP` in `ped.ide`,
anche quando cambia l'arma; combattimento e controlli restano quelli del motore.
Tutte le armi dell'inventario vanilla sono assegnate al giocatore. La riserva
non si consuma; capienza del caricatore, ricarica, cadenza, danni e proiettili
mantengono il comportamento originale. Anche gli agenti hanno riserva infinita.
Per il fucile di precisione degli NPC, il bersaglio dell'AI e la posizione
dell'arma sostituiscono la camera del giocatore: proiettile `CBulletInfo`, velocità,
danno e ricarica restano quelli vanilla. Il detonatore conserva la funzione
originale e richiede un esplosivo associato al suo utilizzatore.

## Comandi

| Comando | Effetto |
| --- | --- |
| **F6** | Genera un agente di strada |
| **F7** | Genera uno SWAT |
| **F8** | Genera un FBI |
| **F9** | Genera un militare |
| **Blocco Maiuscole / +** | Attiva/disattiva Vigilante su Police, Enforcer, FBI o Rhino (comando configurabile `TOGGLE_SUBMISSIONS`) |
| **A piedi: L1 + Cerchio** | Aumenta di una stella il sospetto, anche con un’arma equipaggiata |
| **In veicolo: L1 + Cerchio** | Stessa designazione su pedoni o veicoli occupati |
| **L1 + R2 / L2** | Bersaglio successivo / precedente, in ordine orizzontale con ritorno circolare |

I tasti funzionali sono documentati anche nel codice: l'array `CPad::F` parte
da zero (F6 corrisponde all'indice 5). Gli agenti vengono collocati in una
posizione libera vicino al giocatore, tramite streaming e `CPopulation::AddPed`.
Ricevono l'arma **equipaggiata al momento della richiesta** e usano la relazione
leader vanilla per seguire il giocatore; durante un inseguimento questo obiettivo
cede la precedenza all'AI della polizia. Le armi degli agenti già presenti non
cambiano con quelle del giocatore.

Tenere premuto **L1**, orientare la camera verso il bersaglio e premere
**Cerchio** per aggiungere una stella. Funziona sia a piedi sia in auto, con
qualsiasi arma equipaggiata. Si leggono `CPad::GetLeftShoulder1` e `GetCircle`,
indipendentemente dall'azione Fuoco del layout. **R1 rimane il normale comando
di puntamento delle armi** e non attiva la designazione. Durante L1 i percorsi
Fuoco, pugni, drive-by e armi del veicolo sono bloccati; Cerchio designa soltanto.
Ogni incremento richiede un nuovo clic di Cerchio. La precedente combinazione
Mira/Fuoco con mouse non attiva più il sospetto.
Durante la designazione il mouse ruota la camera esistente e non aziona lo
sterzo; resta disponibile lo sterzo da tastiera/controller.

Un marcatore bianco al centro della visuale indica che la mira è attiva.
Il raggio parte dalla camera aggiornata e, se non colpisce un bersaglio,
la selezione cerca il candidato visibile più vicino al centro in un cono di
25 gradi, entro 100 metri dal giocatore. `CWorld::ProcessLineOfSight` esclude
bersagli dietro ostacoli. Sono esclusi anche i bersagli alle spalle del giocatore
rispetto alla direzione di mira, compresi quelli fra camera e giocatore. La mira
disarmata conserva la direzione della visuale, cancella eventuali agganci della
vecchia arma e impedisce il ricentraggio automatico e la camera da combattimento
durante la designazione. Anche pedoni neutrali sono selezionabili; le auto vuote sono escluse.
Il marcatore vanilla diventa colorato sul bersaglio acquisito; il messaggio
«Sospetto: N / 6» mostra il livello dopo ogni incremento. Il marcatore viene
aggiornato dopo i controlli del giocatore e la camera, così la pulizia vanilla
del bersaglio dell'arma non lo cancella a mani nude o in auto.
Occorre rilasciare e premere nuovamente Cerchio per ogni incremento; durante
la designazione l'input non raggiunge armi, attacchi a pugni, drive-by o armi
montate sui veicoli.

Se un vecchio salvataggio è stato creato dopo aver tentato Vigilante nella
versione che ne eliminava il trigger, iniziare una nuova partita: quel thread
SCM non è più presente nel salvataggio.

## Wanted, alleanze e inseguimenti

`CPoliceDuty` mantiene un piccolo registro di identità dei sospetti; ciascuna
ha un vero `CWanted`. Non sostituisce il puntatore del giocatore, non usa un
`CPlayerPed` fittizio e non imposta stelle al giocatore per ottenere rinforzi.

- `CWanted::SetWantedLevel` e `UpdateWantedLevel` conservano soglie, limiti di
  agenti/veicoli e densità dei posti di blocco vanilla.
- `CCopPed::SetPursuit`, `ClearPursuit`, `CopAI` e `ProcessControl` ricevono il
  relativo sospetto e conservano prenotazione degli agenti, scelta delle armi,
  ricerca, combattimento e comportamento vicino ai veicoli.
- `CCarAI` e `CCarCtrl` riutilizzano le missioni di blocco/speronamento, le
  transizioni lontano/vicino, velocità, ricerca dei nodi stradali e sterzata.
- `CStreaming`, `CPopulation`, la scelta dei modelli della polizia,
  `CRoadBlocks` e `CHeli` usano l'incidente prioritario per la risposta:
  pattuglie, elicotteri, SWAT a 4 stelle, FBI a 5, esercito a 6, con le
  condizioni e probabilità originali.
- L'escalation dà precedenza a un Enforcer/SWAT a 4 stelle, FBI a 5 e a
  entrambi Rhino e Barracks a 6. Il generatore stradale `CCarCtrl` può superare
  temporaneamente la quota Wanted occupata dalle pattuglie precedenti per
  inserire i tipi mancanti; conserva il limite globale dei veicoli, streaming,
  nodi stradali e controlli di collisione/visibilità. Una volta presenti,
  distribuzione casuale e quote delle nuove pattuglie tornano vanilla.
- I posti di blocco usano i nodi vanilla entro 100 metri dal sospetto e 180
  dal giocatore. Da 3 stelle i tentativi non dipendono più dalla probabilità
  una tantum: un nodo non disponibile viene ritentato dopo 2 secondi, mentre
  un blocco creato impone una pausa globale di 8 secondi. Questi sono intervalli
  minimi, non una promessa di spawn ogni 8 secondi: occorrono una strada adatta,
  modelli caricati e spazio libero, almeno 25 metri da giocatore e sospetto.
  Un nodo viene segnato occupato solo dopo la creazione effettiva del blocco.
  La composizione è Police/agenti a 3 stelle, Enforcer/SWAT a 4, FBI a 5,
  Barracks/militari a 6. Non si ripiega su volanti ai livelli speciali: si attende
  lo streaming o si prova un'altra strada. Gli agenti mantengono il bersaglio
  assegnato. Ora tutta la fila viene preparata fuori dal world e pubblicata
  solo se ogni mezzo può essere collocato, con due agenti per mezzo già presenti.
  Le auto della stessa fila non si respingono attraverso i controlli sferici
  durante la generazione. Il numero e l'orientamento seguono la larghezza
  effettiva del nodo: sulle strade strette i Barracks vengono affiancati
  longitudinalmente. Restano possibili varchi creati successivamente da danni
  o spostamenti fisici, senza riparazioni o respawn artificiali.
- I pedoni sospetti usano gli obiettivi di fuga vanilla o conservano il proprio
  combattimento. Il conducente resta sospetto anche quando abbandona il mezzo.
  I mezzi vuoti usano l'obiettivo vanilla `OBJECTIVE_DESTROY_CAR`, insieme alla
  risposta veicolare della polizia.
- `SetArrestPlayer` condivide stato e animazione di arresto con gli NPC senza
  convertirli illegalmente a `CPlayerPed`. Si riusa l'arresto ravvicinato di un
  sospetto atterrato, estratto da un'auto o intercettato durante l'ingresso.

Le risse a pugni registrano un incidente da 1 stella riservato agli agenti
che sono già disarmati, senza richiamare pattuglie. Gli spari portano la risposta
ad almeno 2 stelle: intervengono anche gli agenti armati e i disarmati coinvolti
ricevono una pistola. Un incidente già armato o designato manualmente non viene
declassato da un pugno successivo. Gli agenti vicini usano gli obiettivi vanilla. Un agente già
impegnato mantiene il proprio incidente; i rinforzi generali danno precedenza
al livello più alto, poi alla vicinanza al giocatore.

I metodi di registrazione dei crimini e di aggiornamento Wanted impediscono
**internamente** che il giocatore accumuli caos, reati in coda o livelli di
sospetto. I filtri di relazione, obiettivo, puntamento, reazione e danno rendono
alleati giocatore, polizia, SWAT, FBI e militari. La protezione reciproca riguarda
anche pugni, investimenti, proiettili, armi da veicolo, fuoco ed esplosioni; la
provenienza del danno viene propagata alle esplosioni concatenate. Gli alleati
non possono essere trasformati in nemici dalla designazione manuale.

Il registro usa riferimenti del motore in indirizzi stabili. Prima di riutilizzare
una voce si scollegano gli agenti e si eliminano i vecchi riferimenti con
`PruneReferences`. Morte, arresto, distruzione del mezzo senza un conducente
ancora valido e rimozione dallo streaming chiudono l'incidente. Non si cambia
la disposizione in memoria delle classi vanilla né il formato dei salvataggi.

## Build e limiti tecnici

La workflow `.github/workflows/police-windows.yml` compila e collega
Debug e Release x64/D3D9/OpenAL con MSVC v143 su `windows-2022`. Usa i submodule
fissati dal repository, Premake e le dipendenze audio già incluse. Ogni errore
interrompe il job; gli eseguibili e le DLL vengono pubblicati come artifact.
Le vecchie workflow di altre piattaforme non scattano per il branch `police`.

La configurazione sostituisce, nel solo branch `police`, la precedente
`re3_msvc_amd64.yml`, disattivata manualmente nelle impostazioni GitHub.
Il nuovo percorso consente una workflow attiva per questo branch senza
riattivare le vecchie build degli altri branch e senza duplicare la build x64.

La build completa locale Release x64/OpenGL/GLFW/OpenAL è stata compilata e
collegata con CMake e GCC 13.3. La validazione è **solo compilazione**: nessun avvio del
gioco, test di gameplay, emulatore o test automatico aggiuntivo.

Limiti espliciti:

- Fino a 32 sospetti e 32 agenti richiamati contemporaneamente, oltre alle
  normali pattuglie; lo spawn rispetta lo spazio libero del pool e le collisioni.
- Streaming e popolazione restano centrati sul vero giocatore. Un sospetto
  rimosso dal mondo non viene mantenuto artificialmente in memoria.
- La risposta generale seleziona un incidente prioritario; agenti e pattuglie
  già assegnati conservano il proprio incidente anche passando dal veicolo
  all'inseguimento a piedi. Sono disponibili 32 assegnazioni veicolari simultanee. Il motore non diventa una
  simulazione globale di tutti i sospetti su isole non caricate.
- Sospetti e richiami sono transitori e non vengono serializzati. Caricando
  un salvataggio si ripristinano uniforme, armi e sblocco del mondo, conservando
  la posizione salvata. Usare una nuova partita per iniziare la modalità;
  l'importazione di salvataggi vanilla nel mezzo di una missione non è supportata.
- Per un veicolo vuoto non esiste una persona da arrestare. Si chiude il relativo
  inseguimento alla distruzione o invalidazione del veicolo. L'arresto degli NPC
  riusa le condizioni vanilla, senza aggiungere trasporto in centrale o detenzione.

## File interessati

- Build Windows e filtri delle workflow esistenti: `.github/workflows/build-cmake-conan.yml`, `.github/workflows/build-switch.yml`, `.github/workflows/police-windows.yml` (sostituisce `re3_msvc_amd64.yml`), `.github/workflows/re3_msvc_x86.yml`.
- Avvio SCM, registro sospetti, inseguimenti e posti di blocco: `src/control/CarAI.cpp`, `src/control/CarAI.h`, `src/control/CarCtrl.cpp`, `src/control/PoliceDuty.cpp`, `src/control/PoliceDuty.h`, `src/control/RoadBlocks.cpp`, `src/control/Script.cpp`, `src/control/Script2.cpp`, `src/control/Script6.cpp`.
- Wanted, input, ciclo di gioco, streaming, danni e incendi: `src/core/Cam.cpp`, `src/core/Camera.cpp`, `src/core/Fire.cpp`, `src/core/Fire.h`, `src/core/Game.cpp`, `src/core/Pad.cpp`, `src/core/Pad.h`, `src/core/Streaming.cpp`, `src/core/Wanted.cpp`, `src/core/World.cpp`.
- Giocatore, relazioni, combattimento e popolazione: `src/peds/CopPed.cpp`, `src/peds/PedAI.cpp`, `src/peds/PedFight.cpp`, `src/peds/PlayerPed.cpp`, `src/peds/Population.cpp`.
- Danni, inseguimenti e supporto aereo: `src/vehicles/Automobile.cpp`, `src/vehicles/CarGen.cpp`, `src/vehicles/CarGen.h`, `src/vehicles/Heli.cpp`, `src/vehicles/Vehicle.cpp`.
- Munizioni e attribuzione del danno: `src/weapons/BulletInfo.cpp`, `src/weapons/Explosion.cpp`, `src/weapons/ShotInfo.cpp`, `src/weapons/Weapon.cpp`, `src/weapons/Weapon.h`.
- Ripristino della modalità dopo il caricamento: `src/save/GenericGameStorage.cpp`.
- Documentazione: `README.md`, `docs/POLICE.md`.

### Strisce chiodate

Questo branch GTA III/re3 non contiene un sistema di strisce chiodate né
un comportamento degli agenti per lanciarle. `CAutomobile::BurstTyre` e la
fisica delle gomme danneggiate esistono, ma non costituiscono quel sistema.
Non vengono simulate forature a distanza o aggiunte strisce invisibili:
il lancio di strisce chiodate rimane una funzionalità non implementata.

### Bersagli Vigilante e arresti alla portiera

Il controllo del criminale nel thread missione `COPCAR` rimuove la protezione
«danneggiabile solo dal giocatore» anche nelle missioni caricate da salvataggio
senza assegnare automaticamente stelle o pattuglie al bersaglio. Il sospetto
si attiva tramite la designazione del giocatore oppure una reale aggressione.
Solo quel thread riconosce anche l'arresto concluso come esito del controllo
originale di eliminazione: il sospetto rimane vivo, mentre premi, contatore,
pulizia e generazione del prossimo ricercato seguono il codice SCM originale.
Durante la posa di arresto il thread attende, evitando ordini di fuga o uscita
dall'auto che annullerebbero la custodia. Le altre missioni e i loro controlli
di morte/protezione non vengono modificati.

L'agente termina correttamente l'ingresso/carjacking, rilascia le prenotazioni
della portiera e disattiva i callback della vecchia animazione prima di arrestare
un NPC. La posa non riapplica l'allineamento alla portiera di Claude; il mezzo
del sospetto viene fermato senza riutilizzare lo stato del veicolo del giocatore.

### Cambio bersaglio con L1

Le auto sono selezionabili solo se contengono almeno un occupante vivo e non
arrestato. R2/L2, mentre si tiene L1, scorrono i bersagli validi da sinistra a
destra e viceversa, con ritorno circolare. Si applicano gli stessi controlli
di distanza, cono frontale e visibilità della selezione normale. Il bersaglio
scelto rimane agganciato finché valido e visibile o fino al rilascio di L1;
non viene sovrascritto dal raggio centrale al fotogramma successivo.
Durante L1 questi pulsanti non cambiano arma né ruotano la visuale laterale
in auto. Fuori dalla designazione mantengono le funzioni originali.

### Alleati a bordo

Il controllo delle relazioni annulla solo obiettivi ostili verso alleati,
non gli obiettivi di seguito. Gli agenti richiamati usano l'AI del leader
senza sovrapporre la pattuglia inattiva e mantengono `OBJECTIVE_WAIT_IN_CAR`
mentre condividono il veicolo del giocatore in assenza di un incidente.
Quando il giocatore scende riprende il normale comportamento di uscita/seguito.

### Reclute e personale dei blocchi

Le reclute mantengono il leader anche oltre 30 metri e non usano la scansione
ambientale delle minacce per fuggire. Le aggressioni reali restano gestite da
`ReportAttack` e dall'AI di polizia. Eventuali stati di fuga ambientale già
attivi vengono chiusi ripristinando seguito/inseguimento, salvo la fuga quando
la recluta è in fiamme.

I due agenti di ogni auto del blocco vengono collocati fuori dalle dimensioni
reali del mezzo, sul lato opposto all'arrivo del sospetto, controllando terreno
e spazio libero. Non iniziano un attacco forzato durante la generazione:
`CopAI` gestisce copertura, distanza e tiro. Se mancano spazio, modelli o posti
nel pool, la generazione della coppia resta pendente e viene ritentata invece
di essere considerata completata. La correzione riguarda tutti i livelli.
