# Shelly_Plug_Control

This piece of code is designed to be run on a ESP32 Super Mini.
It is able to search for a SHELLY Plug by looking for its MAC address. It can then control certain features of it.

When checking out there is one file missing "config.h". This file must be created after checking out and it should contain the following two items:
#define WIFI_SSID "THE_SSID_TO_CONNECT_TO"
#define WIFI_PASS "THE_PASSWORD_FOR_THAT_SSID"

## PCB
The PCB was created in KiCad. It's a free opensource program for creating schematics and PCB's.
https://www.kicad.org/