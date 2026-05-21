# ProtoSync — instalacja zależności

Testowane na: **Raspberry Pi OS (Bookworm, arm64)**, jądro PREEMPT_RT.

---

## 1. Pakiety systemowe

```bash
sudo apt update
sudo apt install -y \
    cmake \
    git \
    build-essential \
    libspdlog-dev \
    libasound2-dev
```

---

## 2. pigpio

Pobierany automatycznie jako submoduł Git — nie wymaga instalacji przez apt.
Po kompilacji projektu uruchom demona:

```bash
sudo systemctl enable pigpiod
sudo systemctl start pigpiod
systemctl status pigpiod
```

---

## 3. Submoduły Git (Ableton Link, pigpio, spdlog)

Po sklonowaniu repozytorium zainicjuj submoduły:

```bash
git submodule update --init --recursive
```

Submoduły zostaną pobrane do:
- `libraries/link/`   — Ableton Link SDK
- `libraries/pigpio/` — pigpio
- `libraries/spdlog/` — spdlog (jeśli systemowy niedostępny)

---

## 4. Waveshare LCD — biblioteka C

Biblioteka Waveshare jest przechowywana w repozytorium pod ścieżką
`libraries/waveshare/`. Nie wymaga osobnej instalacji — CMake
kompiluje ją jako bibliotekę statyczną `waveshare_lcd`.

Wymagane zależności sprzętowe (SPI):

```bash
sudo raspi-config
# Interface Options → SPI → Enable
```

Lub ręcznie w `/boot/firmware/config.txt`:

```
dtparam=spi=on
```

---

## 5. UART — konfiguracja dla MIDI

Dodaj do `/boot/firmware/config.txt`:

```
enable_uart=1
dtoverlay=disable-bt
```

Wyłącz konsolę szeregową (zajmuje UART):

```bash
sudo raspi-config
# Interface Options → Serial Port
#   → Login shell over serial: No
#   → Serial port hardware: Yes
```

Zrestartuj urządzenie:

```bash
sudo reboot
```

Sprawdź czy UART jest dostępny:

```bash
ls -la /dev/ttyAMA0
```

---

## 6. I2C — konfiguracja dla ADS1015

```bash
sudo raspi-config
# Interface Options → I2C → Enable
```

Weryfikacja obecności przetwornika ADS1015 (adres 0x48):

```bash
sudo i2cdetect -y 1
```

Oczekiwany wynik: `48` widoczny w tabeli.

---

## 7. Kompilacja

```bash
mkdir -p build && cd build
cmake ..
make -j4
```

---

## 8. Uruchomienie

```bash
sudo ./build/bin/protosync
```

Uprawnienia `sudo` są wymagane przez pigpio i politykę `SCHED_FIFO`.