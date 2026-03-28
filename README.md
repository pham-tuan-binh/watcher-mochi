# Watcher Mochi

Build your own [Dasai Mochi](https://dasai.co) with [SenseCAP Watcher](https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html). Tap the screen or wait, it picks a random GIF from the SD card, plays it with a satisfying pop sound, then returns to idle.

| | | | | |
|:-:|:-:|:-:|:-:|:-:|
| ![happy](sd_content/happy.gif) | ![dancing](sd_content/dancing.gif) | ![love](sd_content/love.gif) | ![sleepy](sd_content/sleepy.gif) | ![surprised](sd_content/surprised.gif) |
| happy | dancing | love | sleepy | surprised |
| ![devil](sd_content/devil.gif) | ![sparkle](sd_content/sparkle.gif) | ![sushi](sd_content/sushi.gif) | ![rain](sd_content/rain.gif) | ![wink](sd_content/wink.gif) |
| devil | sparkle | sushi | rain | wink |

## Features

- Plays random GIFs from the SD card on the 412px round LCD
- Tap-to-play with zoom animation and pop sound feedback
- Auto-plays a random GIF every 30 seconds when idle
- Deep sleep after 5 minutes of inactivity (or long-press the button)
- Wakes on button press

## SD Card Setup

Format a microSD card as FAT32 and copy the contents of `sd_content/` to the root:

- **blank.gif** - idle/default animation shown between GIFs
- **\*.gif** - any other GIF files to be randomly played (63 included in `sd_content/`)

## Build & Flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) v5.4+.

```sh
idf.py build
idf.py flash monitor
```

## Configuration

Board-specific defaults are in `sdkconfig.defaults`. Use `idf.py menuconfig` to adjust settings.
