# TwinTrack

TwinTrack is a compact live departure board for the Spotpear ESP32C3-1.44, the
"Cheap Yellow Display" (ESP32-2432S028R), and an ESP32-WROOM-32D connected to a
black/white/red WeAct 2.9-inch e-paper display. It ships with Malden Manor
(`MAL`) and Tolworth (`TOL`) as its origin stations, filtered toward London
Waterloo (`WAT`) or Chessington South (`CSS`). All four locations can be
changed from the web UI without rebuilding the firmware.

## Choose a display edition

All editions have the same train feed, Wi-Fi provisioning, fallback behaviour,
web UI, station settings, NTP clock, and NVS storage. They differ in the board
they target and how the display is rendered.

| Edition | Board | Sketch | Graphics stack | Character |
| --- | --- | --- | --- | --- |
| Classic | Spotpear ESP32-C3 1.44" (128 x 128) | `twintrack/twintrack.ino` | TFT_eSPI | Lean, direct rendering |
| LVGL | Spotpear ESP32-C3 1.44" (128 x 128) | `twintrack-lvgl/twintrack-lvgl.ino` | LVGL 8.3.7 over TFT_eSPI | Styled panels, modern typography, pills and richer status hierarchy |
| CYD | ESP32-2432S028R 2.8" touchscreen (320 x 240) | `twintrack-cyd/twintrack-cyd.ino` | TFT_eSPI + XPT2046 touch | Four-row landscape board with per-service destinations, controlled by touch zones |
| CYD Matrix | ESP32-2432S028R 2.8" touchscreen (320 x 240) | `twintrack-cyd-matrix/twintrack-cyd-matrix.ino` | TFT_eSPI + XPT2046 touch | Station CIS-style amber dot matrix: ordinal rows, scrolling calling-at line, large live clock |
| E-Paper | ESP32-WROOM-32D + WeAct 2.9" (296 x 128) | `twintrack-epaper/twintrack-epaper.ino` | GxEPD2 1.6.9 | Two-row station-board layout with large times and red urgency blocks |

Compile and upload the sketch matching your board. All editions use the same
`twintrack` NVS namespace, so switching editions does not erase saved Wi-Fi or
train settings unless the device flash is explicitly erased.

## Browser installer

Open [drhammadkhan.github.io/twintrack](https://drhammadkhan.github.io/twintrack/)
in Chrome or Edge to install any display edition over USB. The installer has
separate hardware choices for the Spotpear ESP32-C3 Mini TV, the
ESP32-2432S028R Cheap Yellow Display, and the ESP32-WROOM/WeAct e-paper build,
plus merged images, checksums, and manual recovery instructions.

GitHub Actions rebuilds all sketches from source and republishes the installer
whenever the firmware, display configuration, installer, or build workflow
changes. Firmware files are generated in CI and are not stored in Git history.

## Controls

Spotpear editions (hardware buttons):

- `B0` / GPIO 9: switch station
- `B1` / GPIO 8: switch direction
- `B2` / GPIO 10: refresh immediately

CYD and CYD Matrix editions (touch zones):

- Tap the left third of the screen: switch station
- Tap the middle third: switch direction
- Tap the right third: refresh immediately

The CYD Matrix edition mimics a UK station platform display: every service
row is amber dot-matrix text (`1st  20:45  Waterloo   On time`), the next
service's calling points scroll beneath the top row, and a large HH:MM:SS
clock sits at the bottom. It requests the expanded feed so calling points are
available.

The display refreshes automatically every 30 seconds.

The e-paper target has no physical buttons. Its displayed origin and
destination are selected in the web UI. Live data is checked every minute, but
the slower three-colour panel refreshes immediately only for changed services,
platforms, delays, cancellations, or settings. Otherwise it refreshes every
five minutes to update the clock and countdowns while limiting distracting
full-screen flashes.

The two-line header shows the full origin and destination names, a clean
connection-state label, and the current Europe/London time synchronised over
NTP. Each departure row shows its scheduled time plus `Due` or the number of
minutes until departure. Expected times supplied by the live feed take
precedence over the scheduled time. The footer alternates between the compact
button guide and the `twintrack.local` web address so neither line runs beyond
the display edge.

## Train settings web UI

While TwinTrack is connected to the home network, open the `.local` address
shown in the display footer: `http://twintrack.local/`. The page
allows both origin station slots and both destination filters to be changed
using three-character National Rail CRS codes. Saved choices are stored in NVS
and refresh the display immediately.

The settings page has no user authentication and is intended for a trusted
home network. Anyone able to reach the device on that network can change the
displayed route configuration.

The e-paper target uses `http://twintrack-paper.local/` to avoid an mDNS name
collision when both devices are running. Its web UI also selects which of the
two saved origins and destinations is currently shown and provides a Wi-Fi
reset action.

## Wi-Fi setup

TwinTrack contains no compiled-in Wi-Fi credentials. On first boot it creates a
WPA2-protected setup network named `TwinTrack-XXXX` and shows its temporary
password on the display.

1. Join the displayed `TwinTrack-XXXX` network using the displayed password.
2. Open the captive portal, or browse to `http://192.168.4.1/`.
3. Enter the home Wi-Fi name and password.
4. TwinTrack tests the connection before saving it and restarting.

Validated credentials are stored in the ESP32's local NVS preferences. If the
saved network remains unavailable for one minute, TwinTrack reopens setup mode
without deleting it. While the setup network is available, TwinTrack continues
retrying the saved network every ten seconds. It automatically closes the setup
network and resumes departures when the saved connection recovers. To erase the
saved network, hold `B2` for three seconds during power-on (on the CYD
edition, hold the `BOOT` button instead).

NVS is local persistent storage, but it is not encrypted unless ESP32 flash
encryption is separately enabled. Someone with physical flash access may be
able to recover its contents.

## Local build

### Spotpear Mini TV build

For the Classic and LVGL editions, build for `ESP32C3 Dev Module` with:

- Arduino ESP32 core 2.0.9
- USB CDC on boot enabled
- 160 MHz CPU
- 80 MHz flash frequency
- DIO flash mode
- Huge APP partition scheme
- TFT_eSPI 2.5.0 using [`config/TFT_eSPI_User_Setup.h`](config/TFT_eSPI_User_Setup.h)
- ArduinoJson 6.21.2
- LVGL 8.3.7 for the LVGL edition, using [`config/lv_conf.h`](config/lv_conf.h)

Copy `config/TFT_eSPI_User_Setup.h` over the installed TFT_eSPI library's
`User_Setup.h`. For the LVGL edition, place `config/lv_conf.h` alongside the
installed `lvgl` library directory, as required by LVGL's Arduino integration.
Then compile either the `twintrack` or `twintrack-lvgl` sketch directory.
Generated build artifacts are ignored and should not be committed.

### Cheap Yellow Display build

For the CYD and CYD Matrix editions, build for `ESP32 Dev Module` with:

- Arduino ESP32 core 2.0.9
- 240 MHz CPU
- 80 MHz flash frequency
- DIO flash mode
- Huge APP partition scheme
- TFT_eSPI 2.5.0 using [`config/TFT_eSPI_User_Setup_CYD.h`](config/TFT_eSPI_User_Setup_CYD.h)
- XPT2046_Touchscreen 1.4
- ArduinoJson 6.21.2

Copy `config/TFT_eSPI_User_Setup_CYD.h` over the installed TFT_eSPI library's
`User_Setup.h` before compiling `twintrack-cyd` or `twintrack-cyd-matrix`.

The CYD editions target the common single-USB ESP32-2432S028R. Some board
revisions (notably dual-USB ones) ship panels that need `TFT_INVERSION_ON`,
a different `TFT_RGB_ORDER`, or the `ST7789_DRIVER` in the TFT_eSPI setup —
adjust `config/TFT_eSPI_User_Setup_CYD.h` if colours look wrong.

### ESP32-WROOM e-paper build

Build `twintrack-epaper/twintrack-epaper.ino` for `ESP32 Dev Module` with a
4 MB flash, DIO mode, Huge APP partition, ArduinoJson 6.21.2, and GxEPD2 1.6.9.
Wire the black/white/red WeAct 2.9-inch SSD1680 module as follows:

| E-paper pin | ESP32-WROOM-32D |
| --- | --- |
| VCC | 3.3 V |
| GND | GND |
| SDA / MOSI | GPIO 23 |
| SCL / SCK | GPIO 18 |
| CS | GPIO 5 |
| D/C | GPIO 17 |
| RES | GPIO 4 |
| BUSY | GPIO 16 |

The panel is configured as `GxEPD2_290_C90c` (SSD1680, 296 × 128) in landscape
orientation. The firmware deliberately uses 3.3 V power and logic.

## Data source

Live train information is supplied by National Rail Enquiries Darwin through
the Huxley 2 Community Edition JSON service. This personal display should be
updated if the community endpoint or its access policy changes.
