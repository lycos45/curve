# CuveGuard - GROUPE4

Surveillance d'une cuve d'eau (niveau, temperature, humidite) avec remplissage
automatique piloté par un agent Python via ThingsBoard, simulation Wokwi (ESP32).

## Structure du zip

```
epreuve_cuve_GROUPE4/
├── wokwi/
│   ├── diagram.json          # montage (ESP32, HC-SR04, DHT22, relais, LED, buzzer)
│   ├── sketch.ino            # firmware
│   └── libraries.txt         # librairies Arduino utilisees
├── python/
│   ├── agent.py               # agent de decision (boucle continue)
│   ├── tb_client.py           # client REST ThingsBoard (login, telemetrie, RPC)
│   ├── requirements.txt
│   └── config.example.yaml    # config d'exemple (tokens masques)
├── thingsboard/
│   └── dashboard_cuveguard.json  # export du dashboard
├── video/
│   └── demo.mp4                # a ajouter (voir section Video)
└── README.md
```

## 1. Prerequis

- Un compte sur https://eu.thingsboard.cloud
- Un compte Wokwi (https://wokwi.com)
- Python 3.9+ pour l'agent

## 2. Simulation Wokwi

1. Cree un nouveau projet ESP32 sur wokwi.com.
2. Remplace le contenu de `sketch.ino` par [wokwi/sketch.ino](wokwi/sketch.ino), et
   `diagram.json` par [wokwi/diagram.json](wokwi/diagram.json) (onglet "diagram.json" du projet).
3. Onglet "Library Manager" -> ajoute les librairies listees dans
   [wokwi/libraries.txt](wokwi/libraries.txt) (PubSubClient, DHT sensor library,
   Adafruit Unified Sensor, ArduinoJson).
4. Dans `sketch.ino`, remplace `TB_TOKEN` par le token du device ESP32 cree dans
   ThingsBoard (etape 3 ci-dessous). **Ne remets jamais le vrai token dans le fichier
   qui part dans le zip final** : avant de zipper, repasse `TB_TOKEN` a la valeur
   placeholder `ESP32_DEVICE_ACCESS_TOKEN`.
5. Lance la simulation. Le moniteur serie doit afficher la connexion Wi-Fi
   (reseau `Wokwi-GUEST`, simule, sans mot de passe), puis la connexion MQTT a
   ThingsBoard et l'envoi periodique de la telemetrie.
6. Pendant la simulation, clique sur le capteur HC-SR04 pour faire glisser le
   slider "distance" : cela simule le niveau d'eau qui monte ou descend (la
   pompe simulee ne remplit pas la cuve toute seule, voir enonce section 2).
7. Calibration : dans `sketch.ino`, `TANK_FULL_DISTANCE_CM` (distance capteur/eau
   quand la cuve est pleine) et `TANK_EMPTY_DISTANCE_CM` (quand elle est vide)
   definissent la conversion distance -> pourcentage. Adapte-les si besoin a ton
   scenario de demo.

### Cablage (resume, voir diagram.json)

| Composant        | Broche ESP32 |
|------------------|--------------|
| HC-SR04 TRIG     | D5           |
| HC-SR04 ECHO     | D18          |
| DHT22 DATA       | D4           |
| Relais IN (pompe)| D26          |
| LED rouge alerte | D25          |
| Buzzer           | D33          |

## 3. ThingsBoard (eu.thingsboard.cloud)

1. Cree un compte / connecte-toi sur eu.thingsboard.cloud.
2. **Devices > + Add device** : cree un device `ESP32 CuveGuard` (profile par
   defaut). Ouvre-le, onglet "Details" > "Copy access token" -> colle-le dans
   `sketch.ino` (`TB_TOKEN`).
3. Cree un second device `Agent Python`. Copie aussi son access token -> il ira
   dans `python/config.yaml` (voir section 4), champ `agent_device_token`.
4. Recupere l'**id** (UUID) du device `ESP32 CuveGuard` (onglet Details, en haut) ->
   c'est `esp32_device_id` dans la config Python (utilise pour lire la
   telemetrie et envoyer les RPC via l'API REST).
5. Verifie que la telemetrie arrive : onglet "Latest telemetry" du device ESP32,
   apres avoir lance la simulation Wokwi.

### RPC geres par le firmware

- `setPump` (`params.value`: bool) : force l'etat de la pompe (relais + sortie).
- `setManualMode` (`params.value`: bool) : bascule le mode manuel/auto (le
  firmware se contente de memoriser/publier ce flag ; c'est l'agent Python qui
  respecte ce mode en ne forcant pas la pompe si `manualMode` est vrai).

Ces deux RPC peuvent etre testees manuellement depuis ThingsBoard : device
ESP32 -> onglet "Details" > bouton RPC, ou depuis un widget "Switch" du
dashboard (voir section Dashboard).

## 4. Agent Python

```bash
cd python
python -m venv .venv
.venv\Scripts\activate      # Windows
pip install -r requirements.txt
copy config.example.yaml config.yaml   # puis edite config.yaml avec tes vraies valeurs
python agent.py config.yaml
```

`config.yaml` (copie locale, non versionnee, **ne pas inclure dans le zip**)
contient :

- `thingsboard.username` / `password` : identifiants de connexion au compte
  ThingsBoard (utilises par l'agent pour lire la telemetrie et envoyer les RPC
  via l'API REST `/api/auth/login`, `/api/plugins/telemetry/...`, `/api/rpc/oneway/...`).
- `thingsboard.esp32_device_id` : id du device ESP32 (etape 3.4).
- `thingsboard.agent_device_token` : token MQTT du device `Agent Python`.
- `control.low_threshold_pct` / `high_threshold_pct` : seuils 30 / 90.
- `control.min_command_interval_sec` : delai minimum entre deux changements de
  commande pompe (anti-rafale), en plus de l'hysteresis naturelle entre les
  deux seuils.

L'agent tourne en boucle (`while True`) : a chaque cycle il lit
`waterLevelPct` / `manualMode` / `pumpOn`, decide `START` / `STOP` / `HOLD`,
envoie la RPC `setPump` si necessaire (seulement si `manualMode` est faux), et
publie sa propre telemetrie (`agentDecision`, `observedLevelPct`,
`pumpCommandSent`, `autoMode`) sur son device ThingsBoard.

Seul `python/config.example.yaml` (valeurs bidons) part dans le zip -
`config.yaml` reste local.


## 6. Avant de zipper

- Retire tout vrai token/mot de passe de `sketch.ino` (remets le placeholder)
  et ne mets pas `config.yaml` dans le zip (seulement `config.example.yaml`).
- Verifie que `thingsboard/dashboard_cuveguard.json` est bien l'export final
  (post-modifications faites dans l'UI), pas seulement le gabarit de depart.
- Nomme le fichier final `epreuve_cuve_GROUPE4.zip`.
