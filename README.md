# Pineappley-Player
My Open-Source, Custom built MP3 Player.
Using an ESP32-S3-Zero, cheap, available parts and a custom PCB I designed I've built a reliable and high quality audio playing MP3 Player.

<img width="1500" height="1154" alt="pic black" src="https://github.com/user-attachments/assets/5578cd89-1d2a-4b06-93a5-b72a33bcc9c3" />

## Hardware
- ESP32-S3-Zero
- 1.3 Inch I2C Oled
- Rotary Encoder
- TFT Micro SD Card Module
- KCX Bluetooth Emmiter
- TP4056 Charging Module
- MT3608 Boots Converter
- GY-PCM5102A
- 1800mah lithium battery
- Power Switch

<img width="1530" height="818" alt="pic1" src="https://github.com/user-attachments/assets/64136dbb-c6d8-4800-b7bd-2b97e5eac139" />

## Key Features
- High Quality Audio
- Headphone Out
- Bluetooth Connectivity
- Micro SD card reader
- Rechargeable Battery

<img width="1500" height="1087" alt="pic3" src="https://github.com/user-attachments/assets/1d9bc2d6-55ef-4341-a207-24ebbd0f3684" />

## PCB
I designed the PCB myself in KICAD. It features many through hole areas for each module, smd contacts for the KCX module and battery resistors and encoder capacitors.

## How Does it Work?
After loading your MP3 Files onto the SD card, the files are read over SPI to the ESP. After decoding, the PCM signal is sent over I2S to the PCM5102. The PCM5102 converts the digital audio to analog which can be read by the KCX Emitter.
The Pineappley Player utilises both cores of the ESP32-S3, forcing all audio tasks to core 0 to ensure clean audio as all other tasks such as drawing to the screen can run on core 1.
