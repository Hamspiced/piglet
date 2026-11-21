#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include "soc/soc_caps.h"
#include <stdint.h>

// Waveshare ESP32-C5-Touch-LCD-3.5. Based on the NM-CYD-C5 variant (same C5
// core, same display SPI bus 6/7/2), with three deliberate differences:
//
//  * NO RGB_LED on GPIO27. On this board GPIO27 is the I2C SDA line (touch,
//    I/O expander, codec, RTC, PMU, IMU), NOT a WS2812. Defining a status LED
//    on it -- as the NM-CYD-C5 variant does -- would collide with every I2C
//    device. StatusLight is compiled out for WS_C5_35 (see status_light.cpp).
//  * SD card CS is GPIO9 (NM-CYD-C5 is GPIO10), on the shared display bus.
//  * No TFT_* / USER_SETUP_LOADED here: the display is configured solely by
//    include/wsc5_user_setup.h (force-included by the env), so the pin map
//    lives in exactly one place and cannot disagree with itself. GPIO16/17 are
//    the C5's internal MSPI flash/PSRAM pins and are never referenced.

static const uint8_t TX = 11;
static const uint8_t RX = 12;

static const uint8_t USB_DM = 13;
static const uint8_t USB_DP = 14;

// Default I2C: the shared sensor/touch/expander bus (SDA 27, SCL 26). Code
// that needs a non-default clock calls Wire.begin(27, 26, 100000) explicitly
// (the C5 per-read quirk), but these keep Arduino's bare Wire.begin() sane.
static const uint8_t SDA = 27;
static const uint8_t SCL = 26;

// SPI: the display + microSD shared bus. CS below is the SD card's.
static const uint8_t SS = 9;
static const uint8_t MOSI = 7;
static const uint8_t MISO = 2;
static const uint8_t SCK = 6;

// LP I2C / LP UART pins are fixed on the ESP32-C5.
static const uint8_t LP_SDA = 4;
static const uint8_t LP_SCL = 5;
#define WIRE1_PIN_DEFINED
#define SDA1 LP_SDA
#define SCL1 LP_SCL
static const uint8_t LP_RX = 12;
static const uint8_t LP_TX = 11;

#define SERIAL_TX 11
#define SERIAL_RX 12

#define SPI_SCK_PIN 6
#define SPI_MOSI_PIN 7
#define SPI_MISO_PIN 2
#define SPI_SS_PIN 9

// microSD, on the display's SPI bus. CS 9; driven through the display driver's
// own SPI instance (see sd_log.cpp) so it never tears down the live bus.
#define SDCARD_CS 9
#define SDCARD_SCK SPI_SCK_PIN
#define SDCARD_MISO SPI_MISO_PIN
#define SDCARD_MOSI SPI_MOSI_PIN

#endif /* Pins_Arduino_h */
