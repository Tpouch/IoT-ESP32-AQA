# API HTTPS du capteur AQA

Contrat entre le firmware (≥ 2.0.0) et le serveur, par exemple
`https://sensor.devgoblin.fr`. Tout ce que la carte envoie et tout ce qu'elle
comprend est décrit ici. Les routes « app » de la fin sont des suggestions pour
ton interface web : le firmware ne les appelle pas.

Une implémentation de référence tourne en local avec
`python3 tools/mock_api.py --token dev-token` (stdlib + SQLite). Elle sert à
tester une carte avant que le vrai serveur existe.

```
 ┌──────────┐  POST /status  (60 s)          ┌──────────────┐        ┌─────────┐
 │  ESP32   │ ─────────────────────────────▶ │   serveur    │ ─────▶ │   BDD   │
 │  capteur │  POST /frames  (15 s, lots)    │ (ton app)    │        └─────────┘
 │          │ ◀───────────────────────────── │              │ ◀───── navigateur
 │          │  réponse : { commands: [...] } │              │        (routes app)
 │          │  POST /commands/{id}/ack       │              │
 └──────────┘ ─────────────────────────────▶ └──────────────┘
```

La carte ne reçoit jamais de connexion entrante : elle appelle toujours le
serveur, et les commandes voyagent dans les réponses. Ça traverse le NAT, le
Wi-Fi de résidence et le proxy Cloudflare sans configuration.

---

## 1. Conventions

| Sujet | Règle |
|---|---|
| Base | `{API_URL}/api/v1`, `API_URL` vient du `.env` (défaut `https://sensor.devgoblin.fr`, sans `/` final) |
| Transport | HTTPS. Le certificat est vérifié contre les racines de `src/api/ca_roots.h` (Let's Encrypt, Google Trust Services, SSL.com). `http://` n'est accepté que pour les tests en local. |
| Format | JSON UTF-8, `Content-Type: application/json` |
| Dates | secondes epoch UTC (entier) |
| Valeur absente | `null` (capteur absent ou aucune mesure dans la fenêtre). Ce n'est jamais `0`. |
| `sensor_id` | `aqa-` + MAC Wi-Fi en 12 hexa majuscules, ex. `aqa-24A160B3C8F0`. Stable, affiché au boot (`sensor id ...`). Regex : `^aqa-[0-9A-F]{12}$` |

### En-têtes envoyés par la carte

```
Authorization: Bearer <DEVICE_TOKEN>
Content-Type: application/json
X-Firmware-Version: 2.0.0
```

### Authentification

Chaque carte a son propre jeton (`DEVICE_TOKEN` dans le `.env`). Le serveur
doit vérifier que le jeton correspond **au `sensor_id` de l'URL**, sinon une
carte pourrait écrire au nom d'une autre. Stocke un hash du jeton (SHA-256
suffit pour un jeton aléatoire), pas le jeton en clair.

Générer un jeton :

```bash
python3 -c "import secrets; print(secrets.token_urlsafe(32))"
```

### Codes HTTP : ce que fait la carte

| Réponse | Comportement de la carte |
|---|---|
| `2xx` | Succès. Le corps est lu pour `commands` (corps vide accepté). |
| `400`, `413`, `422` | Requête définitivement refusée : un lot de trames est **jeté** (sinon il bloquerait la file pour toujours), un statut est considéré comme envoyé. Réponds ces codes uniquement pour une requête vraiment invalide. |
| `401`, `403` | Log `token refused`, nouvelle tentative avec backoff. Les trames restent en file. |
| Tout le reste (`404`, `429`, `5xx`, `52x` Cloudflare), erreur réseau ou TLS | Nouvelle tentative avec backoff exponentiel : 5 s, 10 s, 20 s… jusqu'à 5 min. Les trames restent en file. |

Délais côté carte : connexion 6 s, handshake TLS 10 s, réponse 8 s. Réponds
vite et fais les traitements lourds après coup.

---

## 2. Routes appelées par la carte

### 2.1 `POST /api/v1/devices/{sensor_id}/status`

État de la carte. Envoyé **au démarrage**, puis **toutes les 60 s**, et juste
après une commande `status` ou `config`. C'est le « heartbeat » qui permet de
savoir si la carte est en ligne.

Corps :

```json
{
  "t": 1795200000,
  "uptime_s": 3600,
  "boot_count": 12,
  "firmware_version": "2.0.0",
  "hardware_version": 14,
  "ip": "10.12.0.42",
  "rssi": -61,
  "ssid": "ASK4 WiFi",
  "geohash": "d347",
  "free_heap": 151240,
  "sensors": { "temperature": true, "co2": "MH-Z19", "pm": true, "sound": true },
  "queue": { "pending": 0, "dropped": 0 },
  "parameters": { "threshold": 1.5 },
  "last_command_id": "b03dc83795e74388"
}
```

| Champ | Type | Sens |
|---|---|---|
| `t` | entier ou `null` | Heure de la carte. `null` tant que le NTP n'a pas répondu. |
| `uptime_s` | entier | Secondes depuis le démarrage |
| `boot_count` | entier | Nombre de démarrages (NVS). S'il augmente, la carte a redémarré. |
| `firmware_version`, `hardware_version` | chaîne, entier | Versions |
| `ip`, `rssi`, `ssid` | chaîne, entier (dBm), chaîne | Réseau. Au-dessous de −80 dBm, le lien est fragile. |
| `geohash` | chaîne | Position actuelle (modifiable par commande) |
| `free_heap` | entier | RAM libre en octets. Si elle baisse au fil des jours, il y a une fuite. |
| `sensors.temperature` / `pm` / `sound` | booléen | Capteur détecté au boot |
| `sensors.co2` | chaîne ou `null` | `"MH-Z19"`, `"SenseAir S8"`, ou `null` si absent |
| `queue.pending` | entier | Trames en attente d'envoi sur la carte |
| `queue.dropped` | entier | Trames perdues depuis le boot (file pleine après plus de 6 h hors ligne) |
| `parameters` | objet de nombres | Paramètres posés par la commande `config` |
| `last_command_id` | chaîne ou `null` | Dernière commande exécutée |

Réponse `200` :

```json
{ "commands": [] }
```

### 2.2 `POST /api/v1/devices/{sensor_id}/frames`

Les mesures. La carte produit **une trame toutes les 15 s**, la range dans une
file en RAM (1440 trames = 6 h), et l'envoie par **lots de 20 au plus**. En
temps normal, chaque requête contient 1 trame. Après une coupure, la carte
vide sa file par lots successifs.

Corps :

```json
{
  "geohash": "d347",
  "frames": [
    { "t": null,       "age_s": 40, "snd": 31, "co2": 612, "pm25": null, "hum": 45.2, "temp": 21.4, "stale": false },
    { "t": 1795200000, "age_s": 0,  "snd": 30, "co2": 615, "pm25": null, "hum": 45.1, "temp": 21.4, "stale": false }
  ]
}
```

Trames de la plus ancienne à la plus récente.

| Champ | Type | Sens |
|---|---|---|
| `geohash` | chaîne | Position de la carte au moment de l'envoi |
| `t` | entier ou `null` | Heure de capture, `null` si le NTP n'avait pas encore répondu |
| `age_s` | entier | Secondes écoulées entre la capture et cet envoi |
| `snd` | entier ou `null` | Son, amplitude crête-à-crête ADC (0–4095), moyenne sur 15 s |
| `co2` | entier ou `null` | CO2 en ppm, moyenne sur 15 s (une lecture toutes les 2 s) |
| `pm25` | entier ou `null` | PM2.5 en µg/m³, moyenne sur 15 s |
| `hum` | décimal ou `null` | Humidité relative en %, 1 décimale, dernière lecture |
| `temp` | décimal ou `null` | Température en °C, 1 décimale, dernière lecture |
| `stale` | booléen | `true` si les valeurs sont identiques depuis 3 trames (capteur figé ?) |

**Horodater une trame côté serveur :**

```
ts = t                      si t n'est pas null
ts = reçu_à - age_s         sinon (estimation à ~1 s près ; garde un drapeau "estimé")
```

**Doublons :** si la réponse se perd, la carte renvoie le même lot. Mets une
contrainte d'unicité sur `(sensor_id, ts)` et ignore les conflits
(`INSERT ... ON CONFLICT DO NOTHING`). Une trame sans `t` renvoyée plus tard
peut obtenir un `ts` décalé d'une seconde : le risque de doublon est faible
mais pas nul.

Réponse `200` :

```json
{ "accepted": 2, "commands": [] }
```

`accepted` est informatif, la carte ne le lit pas. Accepte au moins 20 trames
par lot.

### 2.3 `POST /api/v1/devices/{sensor_id}/commands/{command_id}/ack`

Avancement ou résultat d'une commande (voir §3).

```json
{ "status": "completed", "progress": 100, "detail": null, "t": 1795200000 }
```

| Champ | Type | Sens |
|---|---|---|
| `status` | `"in_progress"`, `"completed"`, `"failed"` | État |
| `progress` | entier 0–100 | Utile pour `ota` |
| `detail` | chaîne ou `null` | Message (raison d'un échec, `"rebooting"`…) |
| `t` | entier | Heure de la carte, `0` si non synchronisée |

Réponse : `204` (ou n'importe quel `2xx`). La carte n'en lit pas le corps, et
elle ne renvoie pas un ack perdu.

---

## 3. Commandes

Le serveur glisse les commandes en attente dans la réponse de `/status` ou de
`/frames` :

```json
{
  "commands": [
    { "id": "b03dc83795e74388", "type": "geohash", "params": { "geohash": "u09tv" } }
  ]
}
```

- `id` : unique, **`[A-Za-z0-9_-]{1,64}`** (il finit dans l'URL de l'ack).
- `params` : objet, peut être absent si la commande n'en prend pas.
- La carte exécute les commandes dans l'ordre reçu. Envoies-en peu à la fois (≤ 5).
- Délai : ≤ 15 s quand la carte envoie des trames, ≤ 60 s sinon.

| `type` | `params` | Effet et acks |
|---|---|---|
| `status` | — | Envoie un `/status` tout de suite. Ack `completed`. |
| `config` | objet `{ "nom": nombre, … }` non vide | Fusionné dans les paramètres (NVS, survit au reboot), renvoyé dans `status.parameters`. Ack `completed`, ou `failed` si `params` est vide. *Le firmware actuel les stocke seulement, aucune logique ne les lit encore.* |
| `geohash` | `{ "geohash": "u09tv" }` (1–12 caractères) | Change la position envoyée avec les trames (NVS). Ack `completed` / `failed`. |
| `restart` | — | Ack `completed` (`"rebooting"`), puis reboot ~1,5 s après. Le prochain `/status` aura `boot_count` +1. |
| `ota` | `{ "url": "...", "signature_url": "...", "version": "2.0.1" }` | Mise à jour signée, voir §3.2. |
| `rollback` | — | Redémarre sur l'image précédente si elle est valide. Ack `completed` puis reboot, ou `failed` (`"no valid previous image"`). |
| autre | — | Ack `failed` (`"unknown command type"`). |

### 3.1 Cycle de vie conseillé côté serveur

```
pending ──(glissée dans une réponse)──▶ sent ──ack──▶ in_progress ──ack──▶ completed | failed
                                          │
                                          └─ pas d'ack après 10 min ──▶ re-livrer (redevient sent)
```

- Une commande `sent` n'est **pas** renvoyée à chaque requête, seulement
  après le délai d'expiration.
- **Anti-doublon sur la carte :** elle retient l'id de la **dernière**
  commande exécutée (NVS). Si on lui renvoie cet id, elle répond
  `completed` / `"already executed"` sans la rejouer. C'est ce qui empêche un
  `restart` de tourner en boucle si son ack se perd pendant le reboot. Elle ne
  retient que la dernière : ne re-livre pas une commande plus ancienne.

### 3.2 OTA

1. Compile et signe :
   ```bash
   pio run -e ttgo-t7-v14-mini32
   tools/sign_firmware.sh       # → .pio/build/ttgo-t7-v14-mini32/firmware.sig
   ```
2. Publie `firmware.bin` et `firmware.sig` sur ton serveur, par exemple
   `https://sensor.devgoblin.fr/firmware/2.0.1/firmware.bin`. En HTTPS, le
   certificat doit venir d'une des autorités de `ca_roots.h` : sers-les depuis
   le même domaine. La réponse doit avoir un `Content-Length`.
3. Crée la commande :
   ```json
   { "type": "ota", "params": {
       "url": "https://sensor.devgoblin.fr/firmware/2.0.1/firmware.bin",
       "signature_url": "https://sensor.devgoblin.fr/firmware/2.0.1/firmware.sig",
       "version": "2.0.1" } }
   ```

Acks reçus : `in_progress` 0 % (`"downloading"`), puis `in_progress` à environ
25 / 50 / 75 / 100 %, puis :

- `completed` 100 % (`"verified; rebooting into new image"`) : reboot, et le
  `/status` suivant montre la nouvelle `firmware_version` ;
- `failed` avec la raison (`"signature verification failed"`,
  `"firmware download: http 404, size -1"`…) : l'ancien firmware tourne
  toujours, rien n'a été activé.

L'image est téléchargée **une seule fois**. Elle est hachée pendant l'écriture
dans la partition inactive, et cette partition ne devient démarrable qu'après
vérification de la signature RSA-2048 / SHA-256 contre la clé publique de
`src/public_key.h`.

---

## 4. Savoir si une carte est en ligne

Il n'y a pas de « dernière volonté » comme en MQTT : c'est le serveur qui
déduit l'état.

- `last_seen` = heure de la dernière requête authentifiée, quelle qu'elle soit.
- **En ligne** si `last_seen` date de moins de **3 min** (statut toutes les
  60 s, avec de la marge pour le backoff).
- **Redémarrage** : `boot_count` a augmenté, ou `uptime_s` a diminué.
- **Capteur en panne** : `sensors.*` à `false` / `null` dans le statut, ou
  `null` / `stale: true` qui se répètent dans les trames.
- **Données perdues** : `queue.dropped` > 0.

---

## 5. Routes suggérées pour ton app

Elles ne sont pas utilisées par la carte : c'est l'API que ton front pourrait
appeler. `tools/mock_api.py` les implémente toutes sauf `aggregate`, et **sans
authentification** : ça convient en local uniquement. En production, protège-les avec une session ou un
JWT utilisateur, sans rapport avec les jetons des cartes.

| Méthode | Route | Rôle |
|---|---|---|
| `GET` | `/api/v1/devices` | Liste : `id`, `last_seen`, `online` |
| `GET` | `/api/v1/devices/{id}` | Dernier statut complet + `online` |
| `GET` | `/api/v1/devices/{id}/frames?from=&to=&limit=` | Historique, `from` / `to` en epoch |
| `GET` | `/api/v1/devices/{id}/frames/aggregate?from=&to=&step=3600` | Moyennes par tranche, pour les graphes sur plusieurs semaines (≈ 40 000 trames par semaine et par capteur) |
| `POST` | `/api/v1/devices/{id}/commands` | Créer une commande `{ "type", "params" }` → `201 { "id", "state": "pending" }` |
| `GET` | `/api/v1/devices/{id}/commands` | Historique des commandes et de leurs acks |

### Schéma de base suggéré

```sql
CREATE TABLE devices (
    id           TEXT PRIMARY KEY,          -- aqa-XXXXXXXXXXXX
    token_hash   TEXT NOT NULL,             -- sha256 du DEVICE_TOKEN
    name         TEXT,
    last_seen    INTEGER,
    status_json  TEXT                       -- dernier /status brut
);

CREATE TABLE frames (
    device_id    TEXT NOT NULL REFERENCES devices(id),
    ts           INTEGER NOT NULL,          -- epoch s
    ts_estimated BOOLEAN NOT NULL,          -- t était null
    geohash      TEXT,
    snd INTEGER, co2 INTEGER, pm25 INTEGER, hum REAL, temp REAL,
    stale        BOOLEAN NOT NULL DEFAULT FALSE,
    received_at  INTEGER NOT NULL,
    PRIMARY KEY (device_id, ts)
);

CREATE TABLE commands (
    id          TEXT PRIMARY KEY,
    device_id   TEXT NOT NULL REFERENCES devices(id),
    type        TEXT NOT NULL,
    params_json TEXT NOT NULL DEFAULT '{}',
    state       TEXT NOT NULL,              -- pending|sent|in_progress|completed|failed
    progress    INTEGER NOT NULL DEFAULT 0,
    detail      TEXT,
    created_at  INTEGER NOT NULL,
    sent_at     INTEGER,
    updated_at  INTEGER NOT NULL
);
```

SQLite tient sans problème quelques capteurs sur des mois. Si tu veux des
agrégats rapides sur de longues périodes, prends PostgreSQL + TimescaleDB.

---

## 6. Cloudflare

- **Erreur 525 actuelle** : Cloudflare n'arrive pas à faire le TLS avec ton
  serveur d'origine. Installe un certificat sur l'origine (le plus simple : un
  « Origin Certificate » Cloudflare) et mets le mode SSL sur **Full (strict)**.
- **Anti-bots** : *Bot Fight Mode*, *Browser Integrity Check* et les challenges
  bloquent les clients qui ne sont pas des navigateurs. La carte recevrait un
  `403` en HTML, qu'elle loggerait comme `token refused`. Ajoute une règle WAF
  « Skip » pour `starts_with(http.request.uri.path, "/api/v1/devices/")`.
- **Certificat de bord** : si Cloudflare passe à une autorité absente de
  `src/api/ca_roots.h`, la carte refusera la connexion (erreur TLS dans le
  log). Vérifie la chaîne avec
  `openssl s_client -connect sensor.devgoblin.fr:443 -showcerts </dev/null`,
  puis relance `python3 tools/update_ca_roots.py` et reflashe.

---

## 7. Simuler une carte avec curl

```bash
API=http://127.0.0.1:8000/api/v1/devices/aqa-AABBCCDDEEFF   # ou https://sensor.devgoblin.fr/...
TOKEN=dev-token

curl -X POST $API/status -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"t":null,"uptime_s":12,"boot_count":3,"firmware_version":"2.0.0","hardware_version":14,
       "ip":"10.0.0.5","rssi":-61,"ssid":"test","geohash":"d347","free_heap":150000,
       "sensors":{"temperature":true,"co2":"MH-Z19","pm":false,"sound":true},
       "queue":{"pending":0,"dropped":0},"parameters":{},"last_command_id":null}'

curl -X POST $API/frames -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"geohash":"d347","frames":[{"t":null,"age_s":0,"snd":31,"co2":612,"pm25":null,
       "hum":45.2,"temp":21.4,"stale":false}]}'

# route app (mock) : mettre une commande en attente
curl -X POST $API/commands -H "Content-Type: application/json" -d '{"type":"status"}'
```
