# Train2WiFi

Train2WiFi sitzt zwischen einem BLE-FE-C-Trainer (Tacx Neo 2T, Vortex und vergleichbare Geräte) und der Trainingssoftware. Der Trainer wird per Bluetooth verbunden, die Software sieht ihn über Wahoo Direct Connect (DirCon) im LAN.

Lizenz: **GPL-3.0** (siehe `LICENSE` und `NOTICE.txt`).

- **Durchreichen:** FE-C-Dienste über DirCon (SIM und ERG).
- **Zwift Virtual Shifting:** zusätzlicher Zwift-Dienst. Erklärung in [VirtualShifting.md](VirtualShifting.md).
- **FTMS-Emulation für MyWhoosh:** DirCon liefert FTMS, intern gehen die Befehle an den BLE-FE-C-Trainer. Virtual Shifting ist dabei aus.
- **Leistungs-Offset:** Im Webinterface verschiebt ein Slider die gemeldeten Watt um −10 % bis +10 %.

mDNS-Dienst `_wahoo-fitness-tnp._tcp` auf Port **8080**.

Diese Firmware wurde mit ESP-IDF 6.0.1 für den ESP32-S3 gebaut.

## Hardware

Zielboard ist das **Seeed Studio XIAO ESP32-S3** (ESP32-S3R8, 8 MB Flash, 8 MB Octal-PSRAM, nur WLAN). Die USB-C-Buchse ist der eingebaute USB-Serial/JTAG-Port.

- Nach dem Start öffnet das Modul einen Access Point `Train2WiFi-XXXXXX`. Die Webseite ist unter `http://192.168.4.1`.
- Eine gespeicherte WLAN-Station wird zusätzlich als Client verbunden, damit Zwift oder MyWhoosh das Gerät im Heimnetz sehen. Der Access Point bleibt dabei an.

## Bauen und flashen

ESP-IDF 6.0 (Ziel `esp32s3`):

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem-XXXX flash monitor
```

Der XIAO geht über USB-C in den Download-Modus. Falls `idf.py flash` den Chip nicht findet, die Taste BOOT halten, kurz RESET tippen und BOOT wieder loslassen.

Updates laufen über USB und `idf.py flash`.

## Netzwerk und Weboberfläche

Nach dem Start ist der Access Point `Train2WiFi-XXXXXX` offen. Das Passwort ist das gespeicherte WLAN-Passwort, sonst der Hostname (mindestens 8 Zeichen). Im Access Point ist die Seite `http://192.168.4.1`. Sobald eine WLAN-Station gespeichert ist, gilt zusätzlich `http://Train2WiFi-XXXXXX.local` im Heimnetz.

Die Seite zeigt Status, Leistungs-Offset, Trainer und WLAN. Lesen ist offen. Speichern von Trainer und WLAN verlangt Basic-Auth, Benutzer `admin`, Passwort wie der Access Point. Groß- und Kleinschreibung zählt. Der Leistungs-Slider speichert sofort und startet das Gerät nicht neu.

Unter Trainer den Modus wählen: Zwift (Virtual Shifting), MyWhoosh (FTMS) oder nur Durchreichen. In Zwift das Gerät `Train2WiFi …` wählen, nicht den Trainer direkt. Speichern startet das Modul neu.

Standardwerte: Kettenblatt 34, Ritzel 14, Schwierigkeit 100, Virtual Shifting an, Modus Basic Resistance, Steigungsglättung an, FTMS aus, Leistungs-Offset 0 %.

## Aufbau

| Bestandteil | In dieser Firmware |
|---|---|
| Laufzeit | ESP-IDF, `app_main` |
| Bluetooth | `h2zero/esp-nimble-cpp` 2.5 |
| DirCon | `select()`-Server auf Port 8080 |
| Einstellungen | NVS-Namespace `train2wifi` und `esp_http_server` |
| Zeitgeber | `esp_timer` |
