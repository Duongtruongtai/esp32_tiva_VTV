# esp32_tiva_VTV — Tiva, Robot Lab Tour Guide (ESP32-S3)

Two standalone Arduino sketches to verify the microphone and speaker wiring of a
MINI ESP32-S3-N16R8 board before flashing the Xiaozhi firmware.

Pin mapping comes from the `bread-compact-wifi` board in xiaozhi-esp32
(`main/boards/bread-compact-wifi/config.h`, simplex I2S mode). The Vietnam boards
`xiaozhi-ai-iot-vietnam-dtd` and `xiaozhi-ai-iot-vietnam-nvt` use the same pins.

## Wiring

| Module | Module pin | ESP32-S3 GPIO |
|---|---|---|
| Mic INMP441 | WS (LRCL) | 4 |
| Mic INMP441 | SCK (BCLK) | 5 |
| Mic INMP441 | SD (DOUT) | 6 |
| Mic INMP441 | L/R | GND |
| Mic INMP441 | VDD / GND | 3V3 / GND |
| Amp MAX98357A | DIN | 7 |
| Amp MAX98357A | BCLK | 15 |
| Amp MAX98357A | LRC | 16 |
| Amp MAX98357A | VIN / GND | 5V / GND |

Other pins on the same board: OLED SDA=41, SCL=42; BOOT button=0; touch button=47;
volume up=40, volume down=39; built-in LED=48.

## Arduino IDE settings

- Board package: `esp32` by Espressif, version 3.x
- Board: ESP32S3 Dev Module
- Flash Size: 16MB, PSRAM: OPI PSRAM, USB CDC On Boot: Enabled

## Sketches

- `mic_test/mic_test.ino` - prints a live level bar (dB) in Serial Monitor; warns if there is
  no data or if the mic is on the wrong I2S slot.
- `speaker_test/speaker_test.ino` - plays a beep, a musical scale and a frequency sweep in a loop;
  BOOT button changes the volume.

## Voice assistant

`voice_assistant/voice_assistant.ino` - listens (voice activity detection or BOOT button), records
16 kHz WAV, sends it to the Robot Lab Tour Guide server (`POST /api/v1/voice/interact`,
fields `audio_file` + `format=wav`) and plays the returned WAV on the speaker.
Set `WIFI_SSID` / `WIFI_PASSWORD` at the top of the sketch before uploading.

Extra features: OLED face on SDA=41 / SCL=42 (needs the U8g2 library), volume control
(BOOT click = louder, double click = quieter, hold 1 s = talk; optional buttons on GPIO 40/39),
volume is saved in flash.

## Wake word (v3)

The voice assistant now waits for a wake word, detected fully on the chip with Espressif ESP-SR (free):
"Tiva oi", "Chao Tiva", "Hey Tiva", "Hi Tiva", "Hello Tiva", "Ok Tiva" (MultiNet7 phrases) or "Hi ESP" (WakeNet9).
After the answer it listens 6 s for a follow-up question without the wake word.

Arduino IDE -> Tools -> Partition Scheme must be **ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)**
(the sketch refuses to compile otherwise). Verified to compile with esp32 core 3.3.12: 81% of the app partition.


## Before uploading
Edit the top of `voice_assistant/voice_assistant.ino`: put your Wi-Fi networks in `KNOWN_WIFI`
and your server address in `SERVER_BASE`.
