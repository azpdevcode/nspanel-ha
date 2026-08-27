# nspanel-ha

Fork simplifié de [ha-panel](../ha-panel) pour le **Sonoff NSPanel** (non-Pro,
ESP32-D0WD + écran Nextion déporté). Panneau mural minimal : accueil,
lumières, chauffage, météo, paramètres.

Contrairement au panneau CrowPanel P4 (LVGL, framebuffer local), l'ESP32 du
NSPanel n'a **aucun accès à l'écran** : la dalle est pilotée par un
co-processeur STM32G0 qui parle un protocole série (voir `main/bsp/nextion.*`).
Toute l'UI est donc un fichier `.tft` monté dans Nextion Editor, piloté par
le firmware via des commandes texte + événements tactiles.

Voir `CLAUDE.md` (à écrire au fil du fork) pour l'architecture détaillée, et
`design/HMI-SPEC.md` pour la spécification des pages/objets Nextion.

## Build

```bash
source $IDF_PATH/export.sh
idf.py set-target esp32
idf.py build
```

## Premier flash

Sauvegarder le firmware d'origine avant tout flash :
```bash
esptool.py -p /dev/ttyUSB0 -b 460800 read_flash 0 0x400000 nspanel-stock-backup.bin
```
Le NSPanel n'a pas d'auto-reset et le connecteur de programmation n'expose pas
`EN` : relier **`IO0` à `GND`** *avant* de mettre sous tension (c'est la mise
sous tension elle-même, pas un bouton, qui fait entrer en mode bootloader).
Les boutons muraux (GPIO14/27) n'y jouent aucun rôle.

Le port de programmation (UART0, GPIO1/3) et le port écran (UART2, GPIO16/17)
sont deux liaisons **indépendantes** : un flash qui réussit ne prouve rien sur
l'état de la seconde.

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

## Itérer sur l'écran (HMI)

Le `.HMI` est versionné dans `nextion/` — c'est la source du design, à ouvrir
depuis là dans l'Editor. Le `.tft` compilé, lui, est ignoré par git.

**TFT-OTA** (voie principale — n'exige aucun câble) :
```bash
python3 -m http.server 8000            # dans nextion/, sert le .tft
curl -X POST --data-urlencode "url=http://<pc>:8000/nspanel.tft" http://<ip>/tft
curl http://<ip>/status                # progression, puis vspec_rev et pages
```
Penser à **incrémenter `vSpec.val`** dans l'Editor à chaque upload, sans quoi le
firmware réutilise son cache de sondage. Ne pas recompiler pendant un transfert :
un `.tft` servi à moitié réécrit donne un « System data ERROR! » à l'écran
(récupérable par un upload complet).

**Pont série** : maintenir le bouton droit ~700 ms au démarrage (ou
`curl -X POST http://<ip>/bridge`), puis téléverser depuis Nextion Editor comme
sur un écran nu. Suppose l'adaptateur USB-TTL branché en même temps que
l'alimentation, ce que le montage ne permet pas toujours.

Le firmware tourne avec un `.tft` partiel — voir la section « Tolérance au
TFT partiel » du plan de fork.

## Diagnostic

Le moniteur série étant rarement disponible (voir ci-dessus), l'état passe par
HTTP : `GET /status` (WiFi, débit écran, `vSpec`, pages, TFT-OTA), `POST /nx`
(commande brute vers l'écran, réponse en hexa), `POST /scan` (rebalayage des
débits). Détails dans `design/FORK-PLAN.md`, section « Diagnostic sans moniteur
série ».
