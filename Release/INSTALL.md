# Installation and first use

## What is required

- A classic ESP32 DevKit with **4 MB flash**.
- A USB cable that carries data (some charging cables do not).
- A current desktop browser with Web Serial support, such as Chrome, Edge, or
  Firefox, on Windows, macOS, Linux, or ChromeOS.
- The ATU interface hardware built according to the supplied schematic.

No VS Code, ESP-IDF installation, compiler, or Python installation is needed.

## Browser installer

Open the **Installer link** provided on the project's GitHub Release page in a
current desktop browser with Web Serial support, such as Chrome, Edge, or
Firefox. It opens the one-click installer website:

[Install ATU Remote Display](https://ptarsobastos.github.io/ATU_RemDisplay_V1/Release/web-installer/)

Do not download the release folder and open `index.html`, and do not click
`index.html` in the GitHub file browser. The installer must run from the link
above because browsers restrict USB/serial installation to secure websites.

Then connect the board, press **Install ATU Remote Display**, select its serial
port, and approve the erase/install prompts. If no port appears, use a data USB
cable and install the USB-to-UART driver required by the particular DevKit
(usually CP210x or CH340).

The ESP32 normally enters download mode automatically. For boards that do not,
hold **BOOT**, click Install, select the port, and release BOOT after the tool
starts connecting.

## First Wi-Fi setup

On a fresh installation, the firmware starts in access-point mode. Join the
open ESP32 Wi-Fi network named `ATU1000-SETUP-XXXXXX` with a phone or computer;
the final six characters are derived from that board's MAC address, so they
will differ between devices. This setup network has **no password**. Open its
local setup page and provide the normal Wi-Fi network name and password. The
device restarts and joins that network.

To erase saved Wi-Fi settings later, hold the physical TUNE/RESET signal low for
at least 10 seconds, as specified by the hardware design. The ESP32 restarts in
access-point mode.

## Manual recovery option

The files in `firmware/` are provided for a technician using a compatible ESP32
flashing utility. They must be written without changing these addresses:

| Address | File |
| --- | --- |
| `0x1000` | `bootloader.bin` |
| `0x8000` | `partition-table.bin` |
| `0x10000` | `ATU_RemDisplay_V1.bin` |
| `0x110000` | `spiffs.bin` |

Use flash mode `DIO`, frequency `40 MHz`, and flash size `4 MB`. Install all
four files; omitting `spiffs.bin` leaves the Web UI unavailable.

## Verify a downloaded release

Compare the SHA-256 hashes of the four firmware files with
`checksums.sha256` before flashing if the package was downloaded from a mirror.
