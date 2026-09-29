# Podpower Tray

A system tray app for KDE that shows battery levels for connected AirPods and other headphones. Click the tray icon to open the battery panel.

## Download

The ready-to-run build for Linux x86_64 is available on the [Releases](../../releases/latest) page as `Podpower-Tray-x86_64.AppImage`. Download it, allow it to run, then double-click it. The AppImage includes Qt runtime files. The BlueZ service must be running for Bluetooth battery readings.

## Build

Requirements: Qt 6 Widgets development files, qmake6, a C++ compiler, and BlueZ development files.

```sh
qmake6 podpower-tray.pro
make -j2
./podpower-tray
```
