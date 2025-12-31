#pragma once

// PSRAM config first as that only depends on the board
// and not any additional thing it's plugged into
#ifdef PIMORONI_PICO_PLUS2_RP2350
#define PSRAM_CS_PIN PIMORONI_PICO_PLUS2_PSRAM_CS_PIN
#elif defined(PIMORONI_PICO_PLUS2_W_RP2350)
#define PSRAM_CS_PIN PIMORONI_PICO_PLUS2_W_PSRAM_CS_PIN
#elif defined(SOLDERPARTY_RP2350_STAMP_XL)
#define PSRAM_CS_PIN 8
#elif defined(ADAFRUIT_FRUIT_JAM)
#define PSRAM_CS_PIN 47
#elif !defined(DISABLE_PSRAM)
#error "No PSRAM CS!"
#endif

#ifdef EXTRA_BOARD_STAMP_CARRIER
#define DVI_CLK_P 14
#define DVI_D0_P  12
#define DVI_D1_P  18
#define DVI_D2_P  16

// these are not the slot on the carrier, it conflicts with the PSRAM CS
#define SD_SCK    39
#define SD_MOSI   37
#define SD_MISO   38
#define SD_CS     36

#define DISK_IO_LED_PIN 3
#define DISK_IO_LED_ACTIVE 0

#elif defined(EXTRA_BOARD_VGABOARD)
#define AUDIO_I2S_PIO 0
#define AUDIO_I2S_CLOCK_PIN_BASE 27
#define AUDIO_I2S_DATA_PIN       26

#define SD_SCK     5
#define SD_MOSI   18
#define SD_MISO   19
#define SD_CS     22

// until I finish the driver, these are just to make sure the pins are pulled up
#define SD_DAT1   20
#define SD_DAT2   21

#elif defined(EXTRA_BOARD_SD28)

#define AUDIO_I2S_PIO 0
#define AUDIO_I2S_MUTE_PIN 25
#define AUDIO_I2S_DATA_PIN 26
#define AUDIO_I2S_CLOCK_PIN_BASE 27

#define DISPLAY_WIDTH  320
#define DISPLAY_HEIGHT 240

#define DBI_8BIT
#define LCD_CS_PIN 2
#define LCD_DC_PIN 3
#define LCD_SCK_PIN 6 // WR
#define LCD_RD_PIN 7
#define LCD_MOSI_PIN 9 // D0
#define LCD_RESET_PIN 21
#define LCD_BACKLIGHT_PIN 23
#define LCD_VSYNC_PIN 22

#define LCD_MAX_CLOCK 15000000
#define LCD_ROTATION 270

#define SD_SCK  18
#define SD_MOSI 19
#define SD_MISO 20
#define SD_CS   17

// so far the only self-contained board
#elif defined(ADAFRUIT_FRUIT_JAM)

#define AUDIO_I2S_PIO 1
#define AUDIO_I2S_CLOCK_PIN_BASE ADAFRUIT_FRUIT_JAM_I2S_BCLK_PIN
#define AUDIO_I2S_DATA_PIN       ADAFRUIT_FRUIT_JAM_I2S_DIN_PIN

#define TLV320DAC3100_ADDR 0x18

#define DVI_CLK_P ADAFRUIT_FRUIT_JAM_DVI_CKP_PIN
#define DVI_D0_P  ADAFRUIT_FRUIT_JAM_DVI_D0P_PIN
#define DVI_D1_P  ADAFRUIT_FRUIT_JAM_DVI_D1P_PIN
#define DVI_D2_P  ADAFRUIT_FRUIT_JAM_DVI_D2P_PIN

#define SD_SCK    ADAFRUIT_FRUIT_JAM_SD_SCK_PIN
#define SD_MOSI   ADAFRUIT_FRUIT_JAM_SD_MOSI_PIN
#define SD_MISO   ADAFRUIT_FRUIT_JAM_SD_MISO_PIN
#define SD_CS     ADAFRUIT_FRUIT_JAM_SD_CS_PIN

// like the VGA board above, this is a "make sure we pull up"
#define SD_DAT1   ADAFRUIT_FRUIT_JAM_SDIO_DATA1_PIN
#define SD_DAT2   ADAFRUIT_FRUIT_JAM_SDIO_DATA2_PIN

#define PIO_USB_DP_PIN_DEFAULT            ADAFRUIT_FRUIT_JAM_USB_HOST_DATA_PLUS_PIN
#define PICO_DEFAULT_PIO_USB_VBUSEN_PIN   ADAFRUIT_FRUIT_JAM_USB_HOST_5V_POWER_PIN
#define PICO_DEFAULT_PIO_USB_VBUSEN_STATE 1

// default LED, but inverted
#define DISK_IO_LED_PIN PICO_DEFAULT_LED_PIN
#define DISK_IO_LED_ACTIVE 0
// Per TheKitty mini-IBM PC build, pins on the Fruit Jam pin header for disk io
#define DISK_IO_FD_LED_PIN  7
#define DISK_IO_ATA_PRI_LED_PIN 6

#define DEFAULT_I2C_CLOCK 100000

// wifi
#define WIFI_ESP32_NINA
#define WIFI_ESP32_NINA_SPI 1
#define WIFI_ESP32_NINA_SCK  ADAFRUIT_FRUIT_JAM_WIFI_SCK_PIN
#define WIFI_ESP32_NINA_MOSI ADAFRUIT_FRUIT_JAM_WIFI_MOSI_PIN
#define WIFI_ESP32_NINA_MISO ADAFRUIT_FRUIT_JAM_WIFI_MISO_PIN
#define WIFI_ESP32_NINA_CS   ADAFRUIT_FRUIT_JAM_WIFI_CS_PIN
#define WIFI_ESP32_NINA_BUSY ADAFRUIT_FRUIT_JAM_WIFI_ACK_PIN // busy on the schematic

#elif defined(PIMORONI_PICO_PLUS2_RP2350)
// as I was using a mess of jumper wires, there's not really a right answer here
#define DVI_CLK_P 14
#define DVI_D0_P  12
#define DVI_D1_P  18
#define DVI_D2_P  16

#define SD_SCK  2
#define SD_MOSI 4
#define SD_MISO 3
#define SD_CS   5

#else
#error "No board configuration!"
#endif

// default to using built-in LED for disk activity
#if !defined(DISK_IO_LED_PIN) && defined(PICO_DEFAULT_LED_PIN)
#define DISK_IO_LED_PIN PICO_DEFAULT_LED_PIN
#define DISK_IO_LED_ACTIVE 1
#endif

// more disk LED defaults
// merge down to one per controller, then just one LED
// because we can't have two SD card accesses going at once this doesn't result in any conflicts
#ifdef DISK_IO_LED_PIN
#ifndef DISK_IO_FD_LED_PIN
#define DISK_IO_FD_LED_PIN DISK_IO_LED_PIN
#endif

#ifndef DISK_IO_ATA_PRI_LED_PIN
#define DISK_IO_ATA_PRI_LED_PIN DISK_IO_LED_PIN
#endif
#endif

#ifdef DISK_IO_FD_LED_PIN
#ifndef DISK_IO_FD0_LED_PIN
#define DISK_IO_FD0_LED_PIN DISK_IO_FD_LED_PIN
#endif
#ifndef DISK_IO_FD1_LED_PIN
#define DISK_IO_FD1_LED_PIN DISK_IO_FD_LED_PIN
#endif
#endif

#ifdef DISK_IO_ATA_PRI_LED_PIN
#ifndef DISK_IO_ATA_PRI0_LED_PIN
#define DISK_IO_ATA_PRI0_LED_PIN DISK_IO_ATA_PRI_LED_PIN
#endif
#ifndef DISK_IO_ATA_PRI1_LED_PIN
#define DISK_IO_ATA_PRI1_LED_PIN DISK_IO_ATA_PRI_LED_PIN
#endif
#endif
