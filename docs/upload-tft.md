# Upload du .tft sur le NSPanel

Le panneau ne reçoit pas le fichier en POST : l'endpoint `/tft` prend une URL
(`url=` en form-urlencoded) et va chercher le fichier lui-même. Il faut donc
servir le `.tft` en HTTP depuis la machine de dev, puis dire au panneau
d'aller le chercher.

IP du panneau de test : `192.168.50.217` (voir mémoire `nspanel-test-ip`).

## 1. Servir le fichier

Depuis le dossier contenant le `.tft` (ex. `nextion/`) :

```bash
cd /home/pascal/dev/nspanel-ha/nextion
python3 -m http.server 8000
```

## 2. Lancer le transfert

Depuis un autre terminal, remplacer `<IP_PC>` par l'IP de la machine de dev
sur le même réseau que le panneau (ex. `192.168.50.209`) :

```bash
curl -X POST http://192.168.50.217/tft \
  -d "url=http://192.168.50.209:8000/nspanel.tft"
```

## 3. Suivre la progression

```bash
curl http://192.168.50.217/status
```

Si le débit UART a changé après l'upload, resonder avec :

```bash
curl -X POST http://192.168.50.217/scan
```

Pas de moniteur série possible sur ce montage (USB-TTL et alimentation ne
cohabitent pas) — tout le diagnostic passe par ces routes HTTP.
