# Mouse Drive

**Mouse Drive** is an ESP32-S3 project that turns a microSD card into a
dual-mode storage device. It can expose the card to a computer over
native USB as a USB mass-storage drive, or provide a Wi-Fi access point
with a browser-based interface for uploading and downloading files.

> **Project status:** The firmware includes USB mass storage, Wi-Fi file
> transfer, and a physical mode-switch button. PS/2 mouse input code is
> present but disabled by default (`USE_PS2_MOUSE 0`).

## Features

-   **USB mass storage:** Presents the microSD card to a connected
    computer as a USB drive.
-   **Wi-Fi file server:** Creates a Wi-Fi access point named
    `MouseDrive`.
-   **Browser-based file management:** Open the web page to view files
    on the card, download files, and upload one or more files.
-   **Live mode switching:** Press the mode button to switch between
    USB-drive mode and Wi-Fi mode without manually unplugging the board.
-   **SD-card speed fallback:** Tries 20 MHz, 10 MHz, and 4 MHz SPI
    speeds and uses the first speed that passes its read checks.
-   **Status LED:** Uses the board's built-in RGB LED when `RGB_BUILTIN`
    is available.
-   **USB HID mouse support:** Includes USB HID mouse support and button
    handling. PS/2 mouse movement support is optional and disabled in
    the current configuration.

## Hardware

The wiring below matches the pin definitions in the firmware. Confirm
your exact ESP32-S3 board and SD module pinout before wiring.

  Component               ESP32-S3 pin Notes
  --------------------- -------------- ----------------------------------------
  SD module CS                 GPIO 10 SPI chip select
  SD module MOSI               GPIO 11 SPI data to SD card
  SD module SCK / CLK          GPIO 12 SPI clock
  SD module MISO               GPIO 13 SPI data from SD card
  Left button                   GPIO 4 Other button terminal to GND
  Right button                  GPIO 5 Other button terminal to GND
  Mode button                   GPIO 6 Other button terminal to GND
  PS/2 mouse CLK               GPIO 15 Used only when PS/2 support is enabled
  PS/2 mouse DATA              GPIO 16 Used only when PS/2 support is enabled
  Shared ground                    GND Common ground for connected modules

### Power and USB safety

-   Power the SD module according to its specifications. Use 3.3 V
    unless the module explicitly supports the supply voltage you intend
    to use.
-   ESP32-S3 GPIO pins are **not 5 V tolerant**. Do not connect a 5 V
    signal directly to a GPIO pin.
-   The firmware uses the ESP32-S3's native USB interface. Use the
    board's designated USB data connector and a suitable data cable.
-   Do not connect a USB mouse's D+ and D− wires directly to GPIO pins.
    USB host input requires appropriate host-capable hardware and
    software.
-   If you enable PS/2 support, check the mouse's voltage requirements
    and signal levels before connecting it.

## Software requirements

-   An ESP32-S3 development board with native USB support
-   Arduino IDE or a compatible Arduino build environment
-   ESP32 Arduino core with support for `USB.h`, `USBHIDMouse.h`, and
    `USBMSC.h`
-   A microSD card and SPI-compatible SD module
-   A USB data cable
-   A computer or phone with Wi-Fi and a web browser for Wi-Fi mode

The sketch uses these libraries:

``` cpp
#include "USB.h"
#include "USBHIDMouse.h"
#include "USBMSC.h"
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
```

The USB classes are provided by the ESP32 Arduino core. Check that your
installed core version supports the APIs used by the sketch.

## Configuration

The important pin and feature definitions are near the top of the
sketch:

``` cpp
#define SD_CS    10
#define SD_MOSI  11
#define SD_SCK   12
#define SD_MISO  13

#define BTN_LEFT  4
#define BTN_RIGHT 5
#define BTN_MODE  6

#define USE_PS2_MOUSE 0
#define PS2_CLK  15
#define PS2_DATA 16

#define SD_MAX_HZ 20000000
```

-   Set `USE_PS2_MOUSE` to `1` only if you have connected and tested a
    compatible PS/2 mouse.

-   If the SD card is unstable or file corruption occurs, lower
    `SD_MAX_HZ` to `10000000` and test again.

-   The Wi-Fi access point credentials are currently set in the sketch:

    -   **SSID:** `MouseDrive`
    -   **Password:** `12345678`

    Change these values in the firmware before sharing or deploying the
    project. The default password is intended for development, not
    security-sensitive use.

## Build and upload

1.  Install the Arduino IDE and the ESP32 board package.
2.  Select the correct ESP32-S3 board.
3.  Open the Mouse Drive `.ino` sketch.
4.  Verify the GPIO assignments and SD module voltage/wiring.
5.  Confirm that the USB connector selected for upload supports data and
    native USB operation.
6.  Compile and upload the sketch.
7.  Open Serial Monitor at **115200 baud** to view startup messages and
    mode changes.

At startup, the firmware initializes the SD card and USB interfaces. If
the card passes its checks, the board exposes it as USB mass storage.

## Using Mouse Drive

### USB-drive mode

1.  Connect the ESP32-S3 to your computer using its USB data connector.
2.  Wait for the computer to detect the mass-storage device.
3.  Use the drive as you would a normal removable drive.

**Important:** Before switching to Wi-Fi mode, finish file operations
and safely eject the drive from the computer where possible. The
firmware attempts to wait until USB activity has been idle, but it can
force a switch after a timeout. Switching while the computer is writing
may corrupt the file system.

### Wi-Fi mode

1.  Press the **mode button** connected to GPIO 6.
2.  Wait for the USB media to be removed and the Wi-Fi access point to
    start.
3.  Connect your phone or computer to the `MouseDrive` Wi-Fi network.
4.  Open the access point IP address printed in Serial Monitor. The
    default ESP32 soft-AP address is commonly `http://192.168.4.1`, but
    use the address reported by the device.
5.  Use the web page to upload files to the SD card or download existing
    files.

The firmware currently allows up to two Wi-Fi clients to connect to the
access point.

### Return to USB mode

Press the mode button again. The web server and access point stop, the
SD card is reinitialized, and the USB mass-storage media is made
available again if initialization succeeds.

### Status LED

The firmware uses these intended indications when the board exposes
`RGB_BUILTIN`:

-   **Green:** USB-drive mode
-   **Blue:** Wi-Fi mode
-   **Red blinking:** Wi-Fi-mode startup failure or recovery path

LED behavior can vary by board.

## Web server endpoints

  Endpoint              Method   Purpose
  --------------------- -------- --------------------------------------------------
  `/`                   GET      Shows the web interface and files on the SD card
  `/get?f=<filename>`   GET      Downloads a file
  `/upload`             POST     Uploads a file using multipart form data

The web server is available only while Wi-Fi mode is active.

## Troubleshooting

### SD card is not detected

-   Check CS, MOSI, SCK, MISO, VCC, and GND.
-   Confirm that the card is formatted with a file system supported by
    the Arduino SD library.
-   Try a lower `SD_MAX_HZ` value.
-   Check Serial Monitor for `[BOOT] SD FAILED` or SD initialization
    messages.

### Wi-Fi network does not appear

-   Press the mode button and wait for startup.
-   Check Serial Monitor for `[WIFI] softAP try ...` and `[WIFI] ready`
    messages.
-   Ensure the SD card initialized successfully before Wi-Fi mode
    starts.

### Upload or download fails

-   Confirm that the browser is connected to the `MouseDrive` access
    point.
-   Keep the device powered while transferring files.
-   Check that the SD card has free space and is writable.
-   Avoid pressing the mode button during a transfer.

### USB drive disappears or the file system becomes corrupted

-   Safely eject the USB drive before switching modes.
-   Do not access the same SD card simultaneously through USB and Wi-Fi.
-   If errors persist, lower the SD SPI clock and test with a known-good
    card.

### Mouse movement does not work

The current sketch sets `USE_PS2_MOUSE` to `0`, so PS/2 mouse movement
processing is disabled. USB HID mouse support is present, but actual
movement requires an enabled and connected input source.

## Design notes and limitations

-   USB mass-storage and Wi-Fi file-server access are intended to be
    mutually exclusive so that both interfaces do not modify the SD card
    simultaneously.
-   The current Wi-Fi password is a simple development default. Anyone
    who knows it and is within range may be able to connect while the
    access point is active.
-   The web interface does not provide authentication or HTTPS. Use it
    only on a trusted, local connection and do not expose it to an
    untrusted network.
-   The firmware writes uploaded files to the root of the SD card.
    Uploading a file with the same name replaces the existing file.
-   The sketch uses raw SD sectors for USB mass storage. Use caution
    when changing the storage implementation, and always keep backups of
    important data.

## Future improvements

Potential extensions include:

-   SD-card capacity and free-space statistics
-   File search, sorting, and deletion
-   Transfer speed display
-   Improved upload error reporting
-   Optional authentication for the web interface
-   More detailed LED/status reporting

## Contributing

Contributions and improvements are welcome. Test changes carefully with
a backup SD card, especially when modifying USB mass-storage callbacks,
SD-card access, or mode switching.
