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
Le NSPanel n'a pas d'auto-reset : maintenir le bouton gauche enfoncé pendant
la mise sous tension pour entrer en mode bootloader.

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

## Itérer sur l'écran (HMI)

Deux voies, sans démonter le panneau :
- **Pont série** : maintenir le bouton droit ~700 ms au démarrage (ou
  `curl -X POST http://<ip>/bridge`), puis téléverser depuis Nextion Editor
  comme sur un écran nu.
- **TFT-OTA** : `curl -X POST http://<ip>/tft -d "url=http://<ha>:8123/local/nspanel.tft"`.

Le firmware tourne avec un `.tft` partiel — voir la section « Tolérance au
TFT partiel » du plan de fork.
