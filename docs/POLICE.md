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
MAIN continua a funzionare. I successivi lanci di missioni non avviano la storia
né le missioni dei veicoli: il trigger viene fermato e restituisce i controlli.

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
| **A piedi: mani nude + Mira + Fuoco** | Aumenta di una stella il sospetto sul bersaglio |
| **In veicolo: Mira + Fuoco** | Stessa designazione su pedoni o altri veicoli |

I tasti funzionali sono documentati anche nel codice: l'array `CPad::F` parte
da zero (F6 corrisponde all'indice 5). Gli agenti vengono collocati in una
posizione libera vicino al giocatore, tramite streaming e `CPopulation::AddPed`.
Ricevono l'arma **equipaggiata al momento della richiesta** e usano la relazione
leader vanilla per seguire il giocatore; durante un inseguimento questo obiettivo
cede la precedenza all'AI della polizia. Le armi degli agenti già presenti non
cambiano con quelle del giocatore.

Tenere premuto **Mira (tasto destro del mouse)**, orientare la camera con il
mouse verso un pedone o un veicolo, poi premere **Fuoco (tasto sinistro)**.
A piedi occorre selezionare le mani nude; in auto funziona con qualsiasi arma.
Anche le assegnazioni personalizzate di Mira/Fuoco vengono lette, compreso
il comando Mira dei pedoni quando si guida. È mantenuto `CPad::GetTarget` per
il controller; in veicolo il pulsante può condividere il freno a mano secondo
il layout. Durante la designazione il mouse ruota la camera di inseguimento
esistente e non aziona lo sterzo; resta disponibile lo sterzo da tastiera/controller.
Non viene attivata una camera debug o una modalità Free Roam.

Un marcatore bianco al centro della visuale indica che la mira è attiva.
Il raggio parte dalla camera aggiornata e, se non colpisce un bersaglio,
la selezione cerca il candidato visibile più vicino al centro in un cono di
25 gradi, entro 100 metri dal giocatore. `CWorld::ProcessLineOfSight` esclude
bersagli dietro ostacoli. Anche pedoni neutrali e veicoli vuoti sono selezionabili.
Il marcatore vanilla diventa colorato sul bersaglio acquisito; il messaggio
«Sospetto: N / 6» mostra il livello dopo ogni incremento. Il marcatore viene
aggiornato dopo i controlli del giocatore e la camera, così la pulizia vanilla
del bersaglio dell'arma non lo cancella a mani nude o in auto.
Occorre rilasciare e premere nuovamente Fuoco per ogni incremento; durante
la designazione l'input non raggiunge armi, attacchi a pugni, drive-by o armi
montate sui veicoli.

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
- I pedoni sospetti usano gli obiettivi di fuga vanilla o conservano il proprio
  combattimento. Il conducente resta sospetto anche quando abbandona il mezzo.
  I mezzi vuoti usano l'obiettivo vanilla `OBJECTIVE_DESTROY_CAR`, insieme alla
  risposta veicolare della polizia.
- `SetArrestPlayer` condivide stato e animazione di arresto con gli NPC senza
  convertirli illegalmente a `CPlayerPed`. Si riusa l'arresto ravvicinato di un
  sospetto atterrato, estratto da un'auto o intercettato durante l'ingresso.

L'attacco del giocatore a un pedone e l'attacco di un pedone al giocatore o a un
agente registrano una minaccia da almeno 2 stelle. Gli agenti vicini prendono in
carico il sospetto attraverso il normale sistema di inseguimento. Un agente già
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
- Wanted, input, ciclo di gioco, streaming, danni e incendi: `src/core/Cam.cpp`, `src/core/Fire.cpp`, `src/core/Fire.h`, `src/core/Game.cpp`, `src/core/Pad.cpp`, `src/core/Pad.h`, `src/core/Streaming.cpp`, `src/core/Wanted.cpp`, `src/core/World.cpp`.
- Giocatore, relazioni, combattimento e popolazione: `src/peds/CopPed.cpp`, `src/peds/PedAI.cpp`, `src/peds/PedFight.cpp`, `src/peds/PlayerPed.cpp`, `src/peds/Population.cpp`.
- Danni, inseguimenti e supporto aereo: `src/vehicles/Automobile.cpp`, `src/vehicles/Heli.cpp`, `src/vehicles/Vehicle.cpp`.
- Munizioni e attribuzione del danno: `src/weapons/BulletInfo.cpp`, `src/weapons/Explosion.cpp`, `src/weapons/ShotInfo.cpp`, `src/weapons/Weapon.cpp`, `src/weapons/Weapon.h`.
- Ripristino della modalità dopo il caricamento: `src/save/GenericGameStorage.cpp`.
- Documentazione: `README.md`, `docs/POLICE.md`.
