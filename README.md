# TwinTrack

TwinTrack is a compact live departure board for the Spotpear ESP32C3-1.44. It
ships with Malden Manor (`MAL`) and Tolworth (`TOL`) as its origin stations,
filtered toward London Waterloo (`WAT`) or Chessington South (`CSS`). All four
locations can be changed from the web UI without rebuilding the firmware.

## Controls

- `B0` / GPIO 9: switch station
- `B1` / GPIO 8: switch direction
- `B2` / GPIO 10: refresh immediately

The display refreshes automatically every 30 seconds.

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
saved network, hold `B2` for three seconds during power-on.

NVS is local persistent storage, but it is not encrypted unless ESP32 flash
encryption is separately enabled. Someone with physical flash access may be
able to recover its contents.

## Local build

Build for `ESP32C3 Dev Module` with:

- Arduino ESP32 core 2.0.9
- USB CDC on boot enabled
- 160 MHz CPU
- 80 MHz flash frequency
- DIO flash mode
- Huge APP partition scheme
- TFT_eSPI 2.5.0 using [`config/TFT_eSPI_User_Setup.h`](config/TFT_eSPI_User_Setup.h)
- ArduinoJson 6.21.2

Copy `config/TFT_eSPI_User_Setup.h` over the installed TFT_eSPI library's
`User_Setup.h`, then compile the `twintrack` sketch directory. Generated build
artifacts are ignored and should not be committed.

## Data source

Live train information is supplied by National Rail Enquiries Darwin through
the Huxley 2 Community Edition JSON service. This personal display should be
updated if the community endpoint or its access policy changes.
