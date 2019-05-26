# Sterownik urządzenia HardDoom

## O rozwiązaniu

Sterownik działa w sposób synchroniczny i używa jedynie przerwania PONG_SYNC. Dowolne inne przerwanie oznacza błąd i powoduje (bezpieczne) wyłączenie danego urządzenia pci - aby włączyć je ponownie, należy przeładować sterownik, chociaż jest możliwe że (bezpieczne) usunięcie i ponowne włożenie urządzenia także zadziała. Bufory są związane z urządzeniem a nie z otwartym kontekstem `/dev/doomx`, dzięki czemu (przy zachowaniu odpowiedniej synchronizacji) programy mogą przekazywać sobie wzajemnie bufory. Sterownik sprawdza poprawność przekazanych mu parametrów tylko tam, gdzie zależy od tego stabilność urządzenia lub jądra, więc użytkownik może na przykład bez problemów użyć powierzchni (`surface`) jako bufora (`buffer`). Wszystkie operacje niezgodne ze specyfikacją mają jednak niesprecyzowaną semantykę.

Sterownik został przetestowany z jądrem w wersji `4.20.13`.


## Poszczególne pliki rozwiązania
  * `Makefile` jest skonfigurowany tak aby wykonać out-of-source build. Po uruchomieniu `make` moduł jądra `harddoom2.ko` będzie znajdował się w katalogu `build`.
  * Pliki `doomcode2.h`, `doomdev2.h`, `harddoom2.h` zostały dostarczone z dokumentacją do urządzenia.
  * Plik `doomdriver.h` zawiera interfejsy między poszczególnymi podmodułami sterownika.
  * Plik `drv.c` jest głównym plikiem modułu jądra.
  * Plik `pci.c` odpowiada za uruchomienie urządzenia pci (zapisywanego w `struct doomdevice`) oraz obsługę przerwań.
  * Plik `buffer.c` zawiera implementację stronicowanego bufora (`struct doombuffer`) umieszczonego w pamięci DMA. Bufor jest związany z instancją urządzenia doomdevice.
  * Plik `chardev.c` zawiera implementację urządzenia znakowego `/dev/doomx`, którego poszczególne otwarte konteksty są reprezentowane jako `struct doomfile`. W tym pliku znajduje się także logika odpowiedzialna za walidację oraz tłumaczenie poleceń użytkownika na polecenia karty graficznej i wysyłanie ich na urządzenie.
