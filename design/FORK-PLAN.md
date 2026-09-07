# Fork `nspanel-ha` — panneau HA simplifié pour Sonoff NSPanel (non-Pro)

## Contexte

`ha-panel` cible le CrowPanel Advance 5" ESP32-P4 : LVGL plein écran 800×480, PSRAM 200 MHz,
WiFi déporté sur C6, wake word TFLM, Assist, Music Assistant, caméras MJPEG, interphone.
Le besoin ici est l'inverse : un **NSPanel Sonoff non-Pro** mural 86, réduit à l'essentiel —
accueil, lumières, chauffage, météo, paramètres (choix de pièce + WiFi/HA).

Le matériel impose une rupture d'architecture, pas une simple réduction de périmètre :

| | CrowPanel P4 | NSPanel non-Pro |
|---|---|---|
| SoC | ESP32-P4 400 MHz | ESP32-D0WD 240 MHz |
| PSRAM | 32 Mo HEX 200 MHz | **aucune** |
| Flash | 16 Mo | **4 Mo** |
| Écran | RGB parallèle piloté par le SoC | **STM32G0 déporté, UART 115200** |
| UI | LVGL 9.5 + esp_lvgl_port | **objets Nextion dans un `.tft`** |
| WiFi | ESP-HOSTED sur C6 | natif |
| Audio | micro PDM + HP NS4168 | buzzer seul |

Conséquence : **LVGL disparaît entièrement.** L'ESP32 n'a aucun accès au framebuffer ; il
envoie des commandes ASCII (`obj.txt="…"`, `obj.val=…`, `page 2`) et reçoit des événements
tactiles (`0x65 <page> <cid> <event> FF FF FF`). Tout `main/ui/` (~10 200 lignes) est à
réécrire. Ce qui se réutilise tel quel, c'est la moitié basse : réseau, client HA, modèle
pièces, réglages NVS, portail de config, OTA.

**Décisions prises :**
- Dépôt **séparé** (`~/dev/nspanel-ha`), pas de branche ni de cible sélectionnable.
- **HMI maison à la Nextion** : ce plan produit une spécification d'objets ; tu montes le
  `.HMI` dans Nextion Editor (Wine/VM) ; le firmware pilote les objets **par nom**.
- **Le `.HMI` se construit par itérations.** La spec décrit les 6 pages d'emblée, mais tu les
  montes une par une, dans l'ordre qui t'arrange. Le firmware doit donc **tourner avec un
  `.tft` partiel** : objet absent = fonction silencieusement dégradée, jamais un plantage ni
  une boucle d'erreurs. C'est une contrainte de conception de premier ordre, traitée en
  section « Tolérance au TFT partiel » ci-dessous.
- Relais/boutons : **local d'abord, report vers HA** (l'interrupteur marche WiFi coupé).

---

## Ce qui est repris, ce qui tombe

### Repris quasi verbatim
| Source `ha-panel` | Destination | Note |
|---|---|---|
| `net/app_config.c/h` (101 l.) | idem | NVS ns `ha_panel` → renommer `nspanel` |
| `net/wifi.c/h` (236 l.) | idem | compile tel quel sur WiFi natif ; seul le `REQUIRES` change |
| `net/config_portal.c/h` (226 l.) | idem | indispensable ici : aucun clavier pour un JWT de 180 car. |
| `ha/ha_weather.c/h` (204 l.) | idem | WS pur, sans PSRAM ni matériel |
| `ha/ha_ws.c/h` (491 l.) | idem, allégé | retirer les 4 fonctions pipeline vocal |
| `app_diag.c/h` | idem | seuils à réviser (plus de pool LVGL) |
| `ota/ota_manager.c` (359 l.) | idem | retirer le garde-fou `bsp_battery` (secteur) |

### Repris en extrayant de `home_ui.c`
`home_ui.c` fait 1534 lignes dont trois blocs sont indépendants de LVGL et méritent enfin
leurs propres fichiers :

- **lignes 442-597 → `ui/rooms.c/h`** : `ROOMS_TEMPLATE` (le Jinja qui interroge `areas()`,
  `label_entities('Light ha panel')`, le premier `climate.*` de l'aire), `apply_rooms_json`,
  `build_glights`, `start_rooms_fetch`. Retirer le champ `player` de `droom_t` (plus de
  Musique) et **descendre `MAX_ROOMS` de 24 à 8** : la structure actuelle pèse ~28 Ko de BSS,
  intenable sur ~290 Ko de DRAM.
- **lignes 184-441 → `ui/settings.c/h`** : `settings_load` + les `settings_save_*`. Clés
  conservées : `def_room`, `brightness`, `sleep_en`, `sleep_sec`. Clés supprimées : `volume`,
  `ww_cutoff`, `night_*`, `ss_anim`. Clé ajoutée : `ntc_off` (offset sonde température, voir
  section « Relais, boutons, sonde » — les relais/boutons finissent par MQTT discovery, pas
  par des entités miroir, donc pas de clé `rly*_ent`), `tft_url`.
- **`ui/ha_worker.c` (698 l.) → `ui/ui_worker.c`** : garder le bloc lumières + le bloc
  climate + `post_state_event` / `pause_poll` ; supprimer tout le bloc média/pochette.
  La pile est créée par `xTaskCreateWithCaps` en PSRAM → repasser en `xTaskCreate` interne.

### Stripping de `ha_client.c` (1579 → ~450 lignes)
Garder : plomberie HTTP (`ha_http_request`, `ha_http_get_json`), lumières
(`ha_parse_light_state`, `ha_get_light`, `ha_light_toggle`, `ha_light_set_brightness`),
climate (`ha_parse_climate_state`, `ha_get_climate_state`, `ha_climate_set_temp/preset`),
`ha_render_template`, `ha_get_weather_pressure`, `ha_script_run`/`ha_scene_activate`.
(La sonde NTC, les relais et les boutons ne passent PAS par `ha_client` : voir
`ha/ha_mqtt.c`, section « Relais, boutons, sonde ».)
Supprimer : média + Music Assistant, `ha_get_logbook` (**buffer de 256 Ko, impossible ici**),
calendriers, pochettes (décodeur JPEG matériel P4), caméras MJPEG, timers, notifications.

### Supprimé intégralement
Voix + wake word (`ha_voice`, `ha_wake`, `ww_engine.cpp`, `components/micro_speech_features/`,
`alexa.tflite`, esp-sr, esp-tflite-micro), interphone (`intercom/`, 1330 l.), sonnette
(`ha_doorbell`), audio (`bsp/audio.c`), musique, caméras, énergie (`ha_energy` +
`page_energie`), agendas, historique, alarme, timer, notifications (`ha_notify` +
`notify_ui`), batterie (`bsp_battery` + `battery_ui`), `ota/c6_ota.c`, la cible CMake
`flash_c6_fw`, **le garde-fou de patch IDF en tête de `main/CMakeLists.txt`**, l'injection
`LV_ATTRIBUTE_LARGE_RAM_ARRAY=EXT_RAM_BSS_ATTR`, et les ~400 lignes des 4 animations de
veille (le Nextion gère la veille lui-même, cf. plus bas).

---

## Nouvelle couche écran

### `bsp/nextion.c/h` — le pilote série

Broches confirmées (E32-MSW-NX) : **TX=GPIO16, RX=GPIO17** vers l'écran, **GPIO4 = alim
écran** (à mettre et maintenir HAUT au boot, sinon écran noir), relais **GPIO22/GPIO19**,
boutons **GPIO14/GPIO27**, buzzer **GPIO21**, NTC sur **GPIO38** (ADC1).

```c
esp_err_t nextion_init(void);            /* GPIO4 haut, UART, négo débit, attend 0x88 */
void nx_cmd(const char *fmt, ...);       /* ajoute le terminateur FF FF FF */
bool nx_set_txt(nx_obj_t o, const char *s);   /* échappe " et \ ; false si absent */
bool nx_set_val(nx_obj_t o, int32_t v);
bool nx_set_pco(nx_obj_t o, uint16_t rgb565);
bool nx_vis(nx_obj_t o, bool on);
bool nx_page(nx_page_t p);
void nx_dim(uint8_t pct);                /* dim= : remplace display_brightness_set() */
typedef struct { uint8_t page, cid, event; } nx_touch_t;
```
Une tâche RX décode les codes retour (`0x65` tactile, `0x66` page courante, `0x88` prêt,
`0x1A`/`0x1B` erreurs — à logger, c'est le diagnostic principal d'une faute de frappe dans
un nom d'objet) et poste les `nx_touch_t` dans la file de `ui_worker`.

**Débit : on ne négocie rien, on découvre.** (Corrigé en phase 1 — la version initiale de ce
plan prescrivait une montée à `baud=921600` ; c'était une fausse bonne idée, mesurée comme
nuisible sur le matériel réel.)

- Envoyer `baud=921600` à un écran encore en **reparse actif** (firmware Sonoff d'origine)
  suffit à rendre son analyseur muet pour **toute la session** : plus aucune commande n'est
  interprétée ensuite, y compris la séquence de sortie du reparse. Le panneau paraît alors
  débranché alors que l'écran fonctionne parfaitement. La config ESPHome de référence
  (`Blackymas/NSPanel_HA_Blueprint`, `esphome/nspanel_esphome_hw_display.yaml`) ne négocie
  jamais le débit — elle reste à celui du `.tft`.
- Un `.tft` **embarque son propre réglage de débit**, qui change donc à chaque téléversement
  (un projet Nextion neuf est souvent à 9600). Supposer 115200 en dur condamne le pilote à
  devenir muet dès qu'on téléverse un `.tft` réglé autrement.

D'où `nextion_scan_baud()` : balayage des débits standard (115200 et 9600 en tête) jusqu'à ce
que l'écran réponde à `get sleep`, appelé au boot et après chaque upload TFT.

**Piège de syntaxe, vérifié :** `sleep` est une variable **système**, sa syntaxe est
`get sleep` — **sans** suffixe `.val`. `get sleep.val` est invalide et reste sans réponse à
tous les débits ; la détection échoue alors silencieusement alors que l'écran répond
`71 00 00 00 00 FF FF FF` à la requête bien formée. Seuls les composants (`vSpec`, `tStat`…)
prennent la forme `get <nom>.<attribut>`. C'est `get_sync(name, NULL, …)` vs
`get_sync(name, "val", …)` dans `bsp/nextion.c`.

### Modèle de concurrence — nettement plus simple qu'en LVGL

Plus de `lvgl_port_lock()`. **Une seule tâche `ui_worker` écrit sur le port série** ; elle
consomme une file unique alimentée par : les touches Nextion, les pushes WS HA
(`ha_ws` → `ui_worker_post_state_event`, mécanisme déjà en place), un timer 1 Hz (horloge),
et le poll HA. Tout le code de page est mono-thread, sans verrou.

### Veille — déléguée à l'écran

Le P4 dépense ~400 lignes de machine à états (`screen_dim`, `bat_sleep_*`, `night_off_*`,
`ss_refr_period_set`, 4 moteurs d'animation). Le Nextion fait ça nativement :
`thsp=<sec>` (mise en veille sur inactivité tactile), `thup=1` (réveil au toucher),
`dim=<pct>`. Il reste à écrire une dizaine de lignes : au réveil (`0x87`), rafraîchir la
page courante et relancer le poll. Pas de coupure nocturne dans la v1.

---

## Tolérance au TFT partiel

Le firmware ne suppose **jamais** qu'un objet ou une page existe. Trois mécanismes.

### 1. Registre d'objets, généré à partir de la spec

Un seul fichier `ui/nx_objects.def` en X-macro, source unique de vérité, tenu synchrone avec
`design/HMI-SPEC.md` :

```c
/*      énum            page     nom Nextion   attribut de sonde */
NX_OBJ( HOME_CLOCK,     NXP_HOME, "tClock",    "txt")
NX_OBJ( LIGHTS_SLIDER0, NXP_LIGHTS, "hN0",     "val")
```
Il en découle l'énum `nx_obj_t`, la table de noms, et un **bitmap de présence** en RAM.
Toutes les écritures passent par `nx_set_*(nx_obj_t, …)` : si le bit de présence est à 0, la
fonction retourne `false` immédiatement — aucun octet n'est envoyé, aucune erreur générée.
Le code appelant ignore le plus souvent la valeur de retour ; il ne la consulte que là où
l'absence change une décision (cf. point 3).

### 2. Sondage au démarrage

Le NSPanel répond `0x1A` (variable invalide) à toute écriture sur un objet inexistant : c'est
non fatal, mais ce n'est pas corrélable à une commande précise, donc on ne peut pas s'en
servir pour déduire la présence. On sonde explicitement.

- La page 0 du `.HMI` porte une **Variable globale `vSpec`** dont `val` est le numéro de
  révision de la spec. Le firmware lit `get vSpec.val` au boot.
  - Pas de réponse → TFT d'origine ou muet : rester sur `page boot`, afficher l'IP et le
    portail, ne rien tenter d'autre. Le panneau reste flashable et configurable.
  - Révision inférieure à celle attendue → on continue, en mode dégradé, avec un
    avertissement dans les logs et sur `boot`.
- Puis un balayage : pour chaque entrée du registre, `get <nom>.<attribut>` séquentiel avec
  timeout court. Réponse `0x70`/`0x71` → présent ; `0x1A` ou timeout → absent. Une page est
  réputée absente si son objet d'ancrage l'est.
- Le bitmap résultant est mis en cache en NVS, **clé = valeur de `vSpec`**. Tu bumpes `vSpec`
  à chaque upload de TFT, le firmware re-sonde ; sinon il démarre sans balayage.
  Un appui long sur le bouton gauche force un re-sondage, au cas où tu oublies le bump.

### 3. Dégradation dans les pages

- **Navigation** : les hotspots d'une page absente sont ignorés (log + bip d'erreur), et
  `nx_page()` retourne `false` sans changer d'état interne. `home` est le repli.
- **Lumières** : si `hN<i>` (slider) est absent mais `bN<i>` présent → la lumière est en
  bascule seule. Si `bN<i>` est absent → la ligne n'est pas rendue, et le nombre de lignes
  réellement disponibles est déduit du bitmap, pas de `MAX_RLIGHTS`.
- **Chauffage / Météo** : chaque valeur est écrite indépendamment ; un objet manquant fait
  juste un trou visuel. Aucune de ces pages n'a de logique conditionnée à sa complétude.
- **Paramètres** : si `cfg` est absente, la configuration reste intégralement faisable par le
  portail web — qui est déjà la voie principale pour l'URL et le token HA.

Le corollaire est que la page `boot` est le **seul** élément dont l'absence est bloquante.
Elle est donc la première chose à monter dans l'Editor, et elle se réduit à un `text`.

### 4. Itération sur le `.tft`

Le cycle « je retouche l'HMI dans l'Editor → je vois le résultat sur le mur » doit être
court, donc les deux voies d'upload arrivent en **phase 1**, pas à la fin :

- **Mode pont série** (le plus rapide pour itérer) : un appui long sur le bouton droit au
  démarrage, ou `POST /bridge` sur le portail, met l'ESP32 en pont transparent
  USB-UART0 ↔ écran-UART2. Nextion Editor téléverse alors le `.tft` directement depuis le PC,
  comme sur un écran nu. C'est exactement l'usage du « Serial Bridge » GPIO16/17 de Tasmota.
- **TFT-OTA HTTP** : `ota/tft_ota.c` télécharge le `.tft` depuis n'importe quelle URL HTTP
  (`/local/` de HA, ou un simple `python3 -m http.server` sur le PC de dev) et le streame par
  blocs de 4096 avec accusé (protocole d'upload Nextion v1.2). Utile une fois le panneau au
  mur — et en pratique la voie principale, le pont série imposant d'avoir l'adaptateur
  USB-TTL branché en même temps que l'alimentation, ce que le montage ne permet pas toujours.

### 5. Diagnostic sans moniteur série

Le port de programmation (UART0, GPIO1/3) n'est pas toujours accessible en même temps que
l'alimentation du panneau. Tout l'état nécessaire est donc exposé en HTTP par
`net/dev_portal.c` — c'est ce qui a permis de trouver les trois bugs de la phase 1 :

| Route | Usage |
|---|---|
| `GET /status` | JSON complet : version, WiFi, débit écran détecté, `vSpec` + révision, présence par page, état du TFT-OTA avec progression et dernier point de contrôle du pilote |
| `POST /nx` | REPL série vers l'écran : `cmd=<commande>` (vide = écoute seule), `baud=`, `window=` ; renvoie les octets reçus en hexadécimal |
| `POST /scan` | Rebalaye les débits puis re-sonde — après un upload qui a changé le débit |
| `POST /tft` | Lance un TFT-OTA : `url=<url du .tft>` (un seul transfert à la fois, 409 sinon) |
| `POST /bridge` | Bascule en pont série transparent (noreturn, redémarrage requis) |

Deux réflexes utiles avec `/nx` : une écoute seule (`cmd=`) sur 3 s montre si l'écran émet
spontanément — un firmware Sonoff en reparse actif « bavarde » son état en clé/valeur
(`92 "current_page" 00 "boot" 00 FF FF FF`…), un `.tft` neuf reste silencieux tant qu'on ne
l'interroge pas. Et `cmd=get sleep` doit répondre `71 00 00 00 00 FF FF FF` : c'est le test
canonique « on se comprend au débit courant ».

**Attention à la concurrence sur l'UART écran.** `vTaskSuspend()` n'annule pas proprement une
tâche bloquée dans `uart_read_bytes()` : elle reste inscrite comme lecteur du ring buffer, et
un `uart_flush_input()` appelé depuis une autre tâche s'y bloque **indéfiniment** (constaté :
TFT-OTA figé pour toujours sur `flush_input`). D'où la pause **coopérative** de `rx_task`
(drapeau `s_rx_paused` + lecture bornée à 100 ms), et non une suspension de tâche.

---

## Spécification HMI (livrable de ce plan → `design/HMI-SPEC.md`)

320×480 **portrait**. Palette RGB565 dérivée des `COL_*` de `ui_common.h`
(`COL_BG 0x0d1117` → `0x08E2`, `COL_CARD`, `COL_LIGHTS 0xf5b133`, `COL_HEAT 0xf2683c`,
`COL_HOME 0x3b82f6`…) pour garder la parenté visuelle avec le panneau P4.

**Règle structurante :** tout composant que le firmware écrit doit avoir `vscope=global`
dans l'Editor. Sans ça, impossible de pré-remplir une page avant d'y basculer, et chaque
changement de page fait clignoter des valeurs périmées.

**Reparse mode :** le `.tft` doit sortir avec l'*active reparse mode* désactivé (le firmware
Sonoff d'origine l'active ; c'est la cause n°1 des échecs d'upload TFT signalés en
communauté).

**Icônes :** pas de police d'icônes. Les glyphes Material utilisés (~20) sont exportés en PNG
et ajoutés en **ressources Picture** du `.tft`, affichés par `pic`/`picq`. Les polices `.zi`
Nextion sont à chasse fixe : trois suffisent — `f0` 16 px, `f1` 24 px, `f2` 48 px.

Pages (nom Nextion → contenu, objets nommés) :

| # | Page | Objets pilotés |
|---|---|---|
| 0 | `boot` | `tStat` (WiFi / HA / IP), `jProg` (progression OTA) |
| 1 | `home` | `tClock`, `tDate`, `tRoom`, `tLights` (« 3 allumées »), `tTemp`, `tSet`, `pWx` (icône), `tWxT`, `tWxC` + 4 hotspots de nav `mLight`/`mHeat`/`mWx`/`mCfg` |
| 2 | `lights` | `tRoom`, `bPrev`/`bNext` (pièce), `bAll` (tout éteindre), 8 lignes `tN0..7` / `bN0..7` / `hN0..7` (slider) — masquées via `vis` selon `s_rooms[].count` |
| 3 | `climate` | `tCur` (f2), `tSet` (f2), `bUp`/`bDn`, `tAction`, `bAway`/`bFrost`/`bAuto` |
| 4 | `weather` | `pNow`, `tNow`, `tCond`, `tHum`/`tWind`/`tPres`, 4 colonnes `pD0..3`/`tD0..3`/`tHi0..3`/`tLo0..3` |
| 5 | `cfg` | `bRoomPrev`/`bRoomNext`/`tRoom`, `hDim`, `hSleep`, `tSsid`/`tIp`/`tHa`, `bOta`, `bTft`, `bReboot`, `tVer` |

Plus, sur la page 0, la Variable globale **`vSpec`** (`val` = révision de la spec, à
incrémenter à chaque upload) décrite en section « Tolérance au TFT partiel ».

La page `cfg` n'accueille **pas** la saisie du token HA : c'est le portail web
(`config_portal.c`, déjà écrit) qui s'en charge, et `cfg` affiche juste l'URL à ouvrir.

**Ordre de montage suggéré** — chaque étape donne un panneau utilisable, et le firmware
tourne à tous les stades intermédiaires :
`boot` + `vSpec` → `home` (horloge et date seules, sans les tuiles) → `lights` avec 2 lignes
→ `lights` complète avec sliders → `climate` → `weather` → `cfg` → tuiles de `home`.
Rien n'oblige à suivre cet ordre : le registre de présence rend n'importe quelle combinaison
d'objets valide.

---

## Relais, boutons, sonde — `bsp/nspanel_io.c/h` + `ha/ha_mqtt.c/h`

Comportement **local d'abord**, exposition côté HA par **MQTT discovery** (pas de miroir
`input_boolean` : le broker Mosquitto + la découverte HA sont déjà actifs sur l'installation
cible, ce qui donne de vraies entités du registre plutôt qu'un helper à créer à la main) :

1. Appui sur GPIO14/27 → ISR + anti-rebond 30 ms par timer → bascule immédiate de
   GPIO22/19 (sauf canal détaché, voir ci-dessous). Fonctionne WiFi coupé, HA éteint,
   écran planté.
2. Le nouvel état est ensuite poussé vers HA : `ha_mqtt.c` publie sur
   `nspanel/<node>/relay/<1|2>/state` (retenu), sous un appareil « NSPanel » découvert
   via `homeassistant/switch/<node>/relais_<1|2>/config`.
3. Sens inverse : `nspanel/<node>/relay/<1|2>/set` est souscrit ; une commande HA appelle
   `nspanel_relay_set()` (n'émet pas le callback relais, donc pas de boucle) puis republie
   l'état.
4. **Bouton découplé du relais** : chaque bouton remonte systématiquement un événement
   (`event.*` HA, `press`/`long_press`) même quand il ne pilote plus le relais. Une entité
   `switch.*_bouton_N_detache` (entity_category config) permet de couper le lien local par
   canal, pour piloter le relais uniquement depuis une automatisation HA.

NTC GPIO38 : lecture ADC1 périodique (30 s), conversion Steinhart-Hart, offset réglable
(`settings_ntc_offset_tenths`, entité HA `number.*`) puis publication retenue sur
`nspanel/<node>/sensor/temperature`. La sonde est sous la dalle et lit ~3-5 °C au-dessus de
l'ambiante — d'où l'offset, plutôt qu'une valeur brute.

RSSI et uptime sont publiés en entités diagnostic (`sensor.*`, ~60 s) sur le même appareil.

Buzzer GPIO21 : simple PWM LEDC, un bip d'accusé de réception tactile.

---

## Configuration de build

`sdkconfig.defaults` repart de zéro. À reprendre : `COMPILER_OPTIMIZATION_PERF`,
`ESP_MAIN_TASK_STACK_SIZE=8192`, `APP_PROJECT_VER_FROM_CONFIG`,
`BOOTLOADER_APP_ROLLBACK_ENABLE`, `WIFI_MAX_RETRY`, `HA_POLL_INTERVAL_MS`.
À jeter : tout `SPIRAM_*`, tout `LV_*`, tout `ESP_HOSTED_*`, tout `SR_*`,
`ESP32P4_*`, `LCD_RGB_RESTART_IN_VSYNC`, `LV_USE_PPA`.

`idf_component.yml` tombe à **trois** dépendances : `idf >=5.5`,
`espressif/esp_websocket_client`, `espressif/cjson`. Disparaissent : esp_lvgl_port, lvgl,
esp_lcd_touch(_gt911), esp_wifi_remote, esp_hosted, esp-sr, esp-tflite-micro, esp-nn,
esp-dsp, dl_fft, esp-libhelix-mp3, plus le composant local `micro_speech_features`.

`partitions.csv` pour 4 Mo — le `.tft` est streamé HTTP → série sans jamais toucher la flash,
donc aucun SPIFFS n'est nécessaire :
```
nvs,      data, nvs,     ,        24K
otadata,  data, ota,     ,         8K
phy_init, data, phy,     ,         4K
ota_0,    app,  ota_0,   ,      1792K
ota_1,    app,  ota_1,   ,      1792K
```
Budget à vérifier dès la phase 1 avec `idf.py size` : sans LVGL, sans esp-sr, sans MP3,
l'application devrait tenir autour de 1,1-1,3 Mo. Si ça dépasse 1792 K, replier sur un
schéma `factory` unique de 3,5 Mo et abandonner l'OTA A/B.

---

## Séquence de boot (`app_main.c`, ~90 lignes)

`TRY_INIT` est conservé. NVS et `nextion_init()` sont les deux seuls fatals.

1. `nvs_flash_init` → `esp_ota_mark_app_valid_cancel_rollback`
2. `nspanel_io_init()` — GPIO4 haut **en premier**, relais dans leur état NVS, boutons armés
3. `nextion_init()` — attend `0x88`, affiche `page boot`
4. `app_config_init()`
5. `wifi_start()` → `tStat` suit les événements
6. SNTP + TZ `CET-1CEST,M3.5.0,M10.5.0/3`
7. `config_portal_start()`
8. `ui_worker_start()` puis `ha_ws_start()`
9. `ota_manager_init()`
10. `start_rooms_fetch()` → bascule sur `page home` au premier rendu

---

## Découpage en phases

Chacune se termine par un flash et une vérification sur la carte. Le firmware est complet et
tolérant dès la phase 2 : à partir de là, **tu peux avancer sur le `.HMI` en parallèle**, à
ton rythme, sans jamais casser un panneau qui tourne.

1. **Squelette, lien série, boucle d'itération.** Repo, build ESP32, `nextion.c` (avec le
   registre et le sondage), `nspanel_io.c`, WiFi, portail, **mode pont série**, `tft_ota.c`,
   page `boot`. Valide les deux plus gros risques d'un coup : la réponse de l'écran ITead à
   un `.tft` Nextion standard, et le cycle d'upload. À la fin de cette phase, tu peux
   téléverser un HMI que tu viens de dessiner et le voir à l'écran.
2. **Cœur HA + Lumières.** `rooms.c`, `settings.c`, `ui_worker.c`, `ha_client` allégé,
   `ha_ws`, page `lights` avec sliders et pushes WS, `home` minimal (horloge, date, pièce).
3. **Chauffage + Météo.** Pages `climate` et `weather` ; `ha_weather.c` repris tel quel.
4. **Accueil complet + Paramètres.** Tuiles agrégées de `home`, page `cfg`, relais/boutons/NTC
   exposés par MQTT discovery (`ha/ha_mqtt.c`), veille `thsp`/`dim`.
5. **OTA applicatif.** `ota_manager` + `scripts/release.sh` adapté. (Le TFT-OTA est déjà là
   depuis la phase 1.)

---

## Vérification

**Avant tout flash — sauvegarder le firmware d'origine :**
```bash
esptool.py -p /dev/ttyUSB0 -b 460800 read_flash 0 0x400000 nspanel-stock-backup.bin
```
Le NSPanel n'a pas d'auto-reset : maintenir le bouton gauche (GPIO14) enfoncé pendant la mise
sous tension pour entrer en mode bootloader.

Par phase :
```bash
idf.py -p /dev/ttyUSB0 build flash monitor
idf.py size                       # marge sur ota_0 = 1792K
```
- **Phase 1** ✅ *(atteinte)* : `curl http://<ip>/status` doit montrer `wifi.connected`,
  `screen.baud` non nul, et après téléversement d'un `.HMI` réduit à `boot` + `vSpec` :
  `screen.vspec_present: true` avec la bonne `vspec_rev` et `pages.boot: true`.
  Le moniteur série n'est pas requis (et souvent pas disponible : sur ce montage
  l'adaptateur USB-TTL ne peut pas être branché en même temps que l'alimentation).
- **Tolérance (à rejouer à chaque phase)** : flasher le firmware complet avec un `.tft` qui
  n'a que `boot`, puis un qui n'a que `boot` + `home`. Attendu à chaque fois : boot propre,
  `nx: N/M objets absents` dans les logs, **zéro `0x1A` après le sondage**, aucun reboot.
  C'est le test qui garantit que tu peux dessiner l'HMI dans le désordre.
- **Phase 2** : sur HA, basculer une lumière portant le label `Light ha panel` → l'état doit
  suivre sur l'écran en moins d'une seconde (push WS, pas le poll). Dans l'autre sens,
  vérifier avec `mcp__home-assistant__ha_get_state` que le service part bien.
- **Phase 4** : couper le WiFi de HA, appuyer sur les boutons physiques → les relais doivent
  claquer quand même. Rebrancher → `switch.*_relais_*` doit se resynchroniser sur l'état réel
  (état retenu republié à la reconnexion MQTT, voir `ha_mqtt_start`).
- **Phase 5** : bumper la version, publier, vérifier la bascule A/B et le rollback en
  provoquant un crash volontaire.

`app_diag` reste le garde-fou mémoire : sans PSRAM, surveiller le tas interne libre à chaque
changement de page (attendu > 100 Ko au repos).

---

## Risques identifiés

- **Le `.tft` reste un goulot humain.** Chaque itération de design repasse par Nextion Editor
  sous Wine. Le registre de présence supprime le couplage *bloquant* (le firmware n'attend
  plus l'HMI), et le pont série raccourcit la boucle, mais le temps de souris demeure. La
  spec doit donc rester la source de vérité : un objet ajouté dans l'Editor sans entrée dans
  `nx_objects.def` n'existe pas pour le firmware, et l'inverse est simplement inerte.
- ~~**Écran Sonoff ≠ Nextion standard.**~~ **Levé en phase 1** : le NX4832F035_011C accepte
  bien un `.tft` Nextion standard, monté dans l'Editor et téléversé par TFT-OTA, et répond
  au jeu de commandes standard une fois sorti du reparse mode. Le vrai obstacle n'était pas
  la variante ITead mais nos propres bugs (cf. ci-dessous).
- **Trois pièges du pilote série, tous rencontrés et corrigés en phase 1.** À ne pas
  réintroduire — chacun rend l'écran *totalement* muet, symptôme identique et trompeur
  (on croit à un problème de câblage alors que l'écran fonctionne) :
  1. `baud=921600` envoyé à un écran en reparse actif le fige pour toute la session.
     → ne plus négocier le débit, le découvrir (`nextion_scan_baud()`).
  2. `get sleep.val` au lieu de `get sleep` sur une variable **système** : invalide, donc
     sans réponse à tous les débits, donc détection toujours en échec.
  3. `whmi-wri` envoyé au débit d'upload au lieu du débit courant de l'écran : ne marchait
     que tant que les deux valaient 115200.
  Corollaire de méthode : **un écran muet n'est pas une preuve de panne matérielle.**
  Vérifier d'abord avec `POST /nx` en écoute seule, et se rappeler que le port de
  programmation (UART0) et le port écran (UART2) sont deux liaisons indépendantes —
  un flash qui réussit ne dit rien de l'état de la seconde.
- **28 Ko de BSS pour `s_rooms`.** Réduire `MAX_ROOMS` à 8 et `MAX_RLIGHTS` à 8 dès la
  phase 2, avant que le reste ne se cale dessus.
- **Pas de retour en arrière sur le firmware d'origine** sans la sauvegarde flash ci-dessus.
