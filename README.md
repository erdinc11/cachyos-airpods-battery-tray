# Podpower Tray

KDE sistem tepsisinde bağlı AirPods ve diğer kulaklıkların pil durumunu gösterir. Tepsi simgesine tıklayınca pil paneli açılır.

## İndirme

Linux x86_64 için hazır sürüm [Releases](../../releases/latest) sayfasındaki `Podpower-Tray-x86_64.AppImage` dosyasıdır. İndirin, çalıştırma izni verin ve çift tıklayın. AppImage Qt dosyalarını içerir; Bluetooth için sistemde BlueZ servisi çalışmalıdır.

## Derleme

Gerekli paketler: Qt 6 Widgets geliştirme dosyaları, qmake6, C++ derleyicisi ve BlueZ geliştirme dosyaları.

```sh
qmake6 podpower-tray.pro
make -j2
./podpower-tray
```
