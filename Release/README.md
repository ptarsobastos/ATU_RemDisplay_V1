# ATU Remote Display firmware release

This folder lets you install the tested ATU Remote Display firmware on a
standard 4 MB ESP32 DevKit without VS Code, ESP-IDF, Python, or a compiler.

## Install firmware

1. Connect the ESP32 DevKit to the computer with a **data-capable** USB cable.
2. On the project's GitHub Release page, click the **Installer link**. It opens
   [Install ATU Remote Display](https://ptarsobastos.github.io/ATU_RemDisplay_V1/Release/web-installer/)
   in the browser. Do not download or open `web-installer/index.html` directly.
3. In a current desktop browser with Web Serial support, such as Chrome, Edge,
   or Firefox, click **Install ATU Remote Display**, select the ESP32 serial
   port, and confirm the installation.
4. When the ESP32 restarts, join its open Wi-Fi network named
   `ATU1000-SETUP-XXXXXX`, then follow the Wi-Fi setup instructions in
   [INSTALL.md](INSTALL.md). The final six characters are unique to the ESP32.

The installer writes the complete firmware: bootloader, partition table,
application, and the web-interface filesystem. It is intended for the classic
ESP32 with 4 MB flash, not ESP32-C3, ESP32-S2, ESP32-S3, or ESP32-C6 boards.

## Contents

- `web-installer/` - browser-based installer and its firmware manifest.
- `firmware/` - the exact binaries installed by the browser tool.
- `hardware/` - electrical schematic PDF.
- `source/` - source snapshot for reference or future development.
- `INSTALL.md` - first-use, wiring, and troubleshooting information.
- `checksums.sha256` - SHA-256 hashes for verifying firmware downloads.

## Important hardware note

The firmware does not by itself make an ESP32 safe to connect to the ATU.
Build the level shifting, resistor dividers, and open-collector driver stages
shown in `hardware/ATU_RemDisplay_V1_schematic.pdf` before connecting it to the
ATU. Do not apply a 5 V signal directly to an ESP32 GPIO.

See `LICENSE` and `NOTICE` for distribution terms and third-party notices.

## Publishing a release

Enable GitHub Pages for this repository so that the `Release/` folder is
available at the installer address above. Add that address as the **Installer
link** in every GitHub Release description. This gives end users one link to
click; they do not need to run a webserver or handle HTTPS themselves.
