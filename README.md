# 🐠 Aquarium LED Controller - ESP-01

Een slimme aquarium LED controller gebaseerd op ESP-01 (ESP8266) met automatische daglichtsimulatie, webinterface en OTA updates.

## ✨ Features

- **Automatische Daglichtsimulatie**: Natuurlijk verloop van licht gedurende de dag
- **RGB LED Controle**: Onafhankelijke sturing van rood, groen en blauw kanalen
- **Webinterface**: Eenvoudige bediening via browser
- **Handmatige Override**: Schakel tussen automatisch en manueel
- **Test Modus**: Test individuele kanalen met pulsfunctie
- **OTA Updates**: Draadloos firmware updaten
- **NTP Tijdsync**: Automatische tijdsynchronisatie
- **WiFi Configuratie**: Eenvoudig aan te passen

## 🔧 Hardware

### Benodigdheden
- ESP-01 (ESP8266) module
- 3x MOSFET driver (bijv. IRLZ44N of vergelijkbaar)
- RGB LED strip (12V)
- 12V voeding
- 3.3V regulator voor ESP-01
- Pull-up weerstanden (10kΩ) voor GPIO0 en GPIO2
- Verbindingsdraden

### Pinout

```
ESP-01 Pin        Functie           Aansluiting
──────────────    ───────────       ─────────────────
GPIO 0            LED Kanaal 1      MOSFET Gate 1
GPIO 2            LED Kanaal 2      MOSFET Gate 2
GPIO 3 (RX)       LED Kanaal 3      MOSFET Gate 3
VCC               3.3V              3.3V Regulator
GND               Ground            GND
```

### Aansluitschema

```
12V Voeding
    │
    ├──────> LED Strip +12V
    │
    └──────> 3.3V Regulator ──> ESP-01 VCC
                │
                └──> ESP-01 GND ──> GND


ESP-01 GPIO    MOSFET (x3)    LED Strip
─────────────  ─────────────  ───────────
GPIO 0/2/3 ──> Gate           
               Source ───────> GND
               Drain  ───────> LED Strip (R/G/B-)
```

**Belangrijk:**
- GPIO0 en GPIO2 hebben pull-up weerstanden nodig (10kΩ naar 3.3V)
- GPIO3 (RX) kan gebruikt worden als output maar wees voorzichtig bij programmeren
- Gebruik logic level MOSFETs (IRLZ44N werkt goed met 3.3V)
- LED strip common anode (+12V vast, - geschakeld via MOSFET)

## 📦 Software Installatie

### PlatformIO

1. Clone of download dit project
2. Open in VSCode met PlatformIO extensie
3. Pas configuratie aan in `main.cpp`:
   ```cpp
   const char *ssid = "Jouw_WiFi_SSID";
   const char *password = "Jouw_WiFi_Wachtwoord";
   const char *otaHostname = "aquarium-esp01";
   ```
4. Pas GPIO pinnen aan indien nodig:
   ```cpp
   const uint8_t channelPins[3] = {0, 2, 3};  // GPIO0, GPIO2, GPIO3(RX)
   ```
5. Build en upload:
   ```bash
   platformio run --target upload
   ```

### Dependencies
Worden automatisch geïnstalleerd:
- ESP8266WiFi
- ESP8266WebServer
- ESP8266mDNS
- ArduinoOTA

## 🚀 Gebruik

### Eerste Keer Opstarten

1. Upload de firmware naar ESP-01
2. ESP-01 verbindt met WiFi en synchroniseert tijd via NTP
3. Open Serial Monitor (115200 baud) om het IP adres te zien
4. Ga naar het IP adres in je browser

### Webinterface

#### Hoofdpagina (/)
- **Systeem Status**: WiFi, tijd, operationele modus
- **LED Status**: Huidige waardes per kanaal (0-100%)
- **Kanaal Mapping**: Toewijzing van fysieke kanalen aan kleuren
- **Snelkoppelingen**:
  - Auto Mode: Schakel terug naar automatische daglichtsimulatie
  - Lights Off: Schakel alle LED's uit
  - Test Pulses: Test individuele kanalen

#### Configuratie Pagina (/config)
**Kanaal Mapping**: Wijs elke GPIO aan een kleur toe:
- Kanaal 1 (GPIO 0) → Rood/Groen/Blauw
- Kanaal 2 (GPIO 2) → Rood/Groen/Blauw
- Kanaal 3 (GPIO 3) → Rood/Groen/Blauw

**Handmatige Controle**:
- RGB sliders (0-100%)
- Direct effect bij aanpassing
- Blijft actief tot "Auto Mode" weer ingeschakeld wordt

**Test Modus**:
- Test individuele kanalen met 5-seconden puls
- Handig voor troubleshooting

#### OTA Update Pagina (/update)
- Upload nieuwe firmware (.bin bestand)
- ESP-01 herstart automatisch na succesvolle update
- Progress indicator

### mDNS
De ESP-01 is bereikbaar via:
```
http://aquarium-esp01.local/
```
(mDNS moet ondersteund worden door je OS/browser)

## 📅 Daglichtsimulatie Schema

Het standaard schema simuleert een natuurlijke dag:

| Tijd | RGB Levels | Fase |
|------|-----------|------|
| 00:00 | 0%, 0%, 0% | Nacht |
| 06:00 | 5%, 2%, 1% | Dageraad |
| 07:30 | 35%, 25%, 20% | Zonsopgang |
| 12:00 | 80%, 80%, 85% | Middagzon |
| 17:00 | 45%, 35%, 30% | Namiddag |
| 18:30 | 25%, 10%, 5% | Zonsondergang |
| 21:00 | 0%, 0%, 0% | Nacht |

### Schema Aanpassen

In `main.cpp`, pas het `daylightSchedule` array aan:

```cpp
const DayPhase daylightSchedule[] = {
  {  0, {0.00f, 0.00f, 0.00f}},  // Minuten, {R, G, B} (0.0-1.0)
  {360, {0.05f, 0.02f, 0.01f}},  // 360 min = 06:00
  {720, {0.80f, 0.80f, 0.85f}},  // 720 min = 12:00
  // ... meer fases
};
```

**Minuten berekenen**: `uren × 60 + minuten`
- 06:00 = 360 minuten
- 12:00 = 720 minuten
- 18:30 = 1110 minuten

## ⚙️ Configuratie

### Tijdzone Instelling

Pas GMT offset aan in `main.cpp`:

```cpp
constexpr long gmtOffsetSec = 3600;      // +1 uur (West-Europa)
constexpr int daylightOffsetSec = 3600;  // +1 uur zomertijd
```

Voorbeelden:
- **Nederland (winter)**: `gmtOffsetSec = 3600`, `daylightOffsetSec = 0`
- **Nederland (zomer)**: `gmtOffsetSec = 3600`, `daylightOffsetSec = 3600`
- **UK**: `gmtOffsetSec = 0`, `daylightOffsetSec = 0/3600`
- **US Eastern**: `gmtOffsetSec = -18000`, `daylightOffsetSec = 3600`

### PWM Frequentie

De ESP8266 PWM werkt op 1000 Hz met 10-bit resolutie (0-1023):

```cpp
constexpr uint16_t PWM_MAX = 1023;
```

## 🐛 Troubleshooting

### ESP-01 Start Niet
- Check 3.3V voeding (niet 5V!)
- GPIO0 moet HIGH zijn tijdens boot (pull-up weerstand)
- GPIO2 moet HIGH zijn tijdens boot (pull-up weerstand)
- TX/RX niet gekruist tijdens normale werking

### Kan Niet Programmeren
- GPIO0 moet naar GND tijdens boot voor flash modus
- Gebruik USB-to-Serial adapter met 3.3V levels
- Reset ESP-01 na het verbinden van GPIO0 naar GND

### WiFi Verbindt Niet
- Check SSID en wachtwoord in code
- Controleer WiFi signaalsterkte
- ESP-01 ondersteunt alleen 2.4 GHz (geen 5 GHz)
- Sommige ESP-01 hebben problemen met WPA3

### LED's Werken Niet
- Check MOSFET aansluitingen (Gate, Source, Drain)
- Verify channel mapping in /config
- Test met test pulse functie
- Check 12V voeding LED strip
- Meet voltage op MOSFET gate (moet 3.3V zijn bij AAN)

### LED's Flikkeren
- Voeding instabiel → betere 12V adapter
- PWM frequentie te laag → is standaard 1000 Hz (goed)
- Lange kabels → houd kabels kort tussen ESP-01 en MOSFET

### Tijd Niet Gesynchroniseerd
- Check internet connectie
- NTP server niet bereikbaar → pas `ntpServer` aan
- Firewall blokkeert NTP (UDP port 123)
- Wacht 30 seconden na boot

### OTA Update Werkt Niet
- Check of ESP-01 voldoende vrij geheugen heeft
- Firmware bestand te groot → optimaliseer build
- WiFi connectie verbroken tijdens update
- ESP-01 heeft mogelijk onvoldoende flash (1MB versie nodig)

## 📝 Technical Details

### Hardware Vereisten
- **ESP-01**: Minimaal 1MB flash versie
- **RAM**: ~20 kB vrij tijdens runtime
- **PWM**: 3 kanalen, 1000 Hz, 10-bit resolutie

### Software
- **Platform**: ESP8266 Arduino Core
- **Webserver**: Embedded HTTP server
- **Update**: ArduinoOTA via WiFi
- **Time Sync**: NTP over UDP

### Performance
- **PWM refresh**: 1000 Hz (flicker-free)
- **Schedule update**: Elk 5 seconden
- **Time sync**: Bij boot + automatisch refresh
- **Web response**: <100ms

### Memory Usage
- **Program**: ~300 kB
- **Static RAM**: ~15 kB
- **Dynamic RAM**: ~5 kB
- **Free heap**: ~30 kB

## 🔐 Security

⚠️ **Belangrijk**: Deze controller heeft GEEN authenticatie!
- Gebruik alleen op vertrouwd netwerk
- Niet blootstellen aan internet
- Overweeg VLAN/netwerk segmentatie

## 💡 Tips

### Voor Aquariumplanten
- Meer rood/blauw voor plantgroei
- Minder groen (wordt gereflecteerd door planten)
- Pas schema aan voor 8-10 uur licht per dag

### Voor Koralen (Zeewater)
- Meer blauw/wit licht
- Vermijd veel rood (algengroei)
- Simuleer maanfases met nachtmodus

### Voor Display Aquarium
- Balanceer RGB voor natuurlijk wit licht
- Zorg voor geleidelijke overgangen
- Vermijd plotse helderheidsveranderingen

### Energiebesparing
- Gebruik LED strips met hoge efficiency
- Pas maximale helderheid aan (reduceer max waarden in schema)
- Overweeg kortere lichtperiodes

## 📄 License

Dit project is open source. Gebruik naar eigen inzicht.

## 🙏 Credits

Ontwikkeld voor aquarium LED verlichting met natuurlijke daglichtsimulatie.

## 🔗 Links

- [ESP8266 Arduino Core](https://github.com/esp8266/Arduino)
- [ESP-01 Pinout](https://www.electronicshub.org/esp8266-pinout/)
- [PlatformIO](https://platformio.org/)
- [Aquarium Lighting Guide](https://www.practicalfishkeeping.co.uk/features/how-to-choose-aquarium-lighting/)

---

**Made with ❤️ for aquarium enthusiasts**
