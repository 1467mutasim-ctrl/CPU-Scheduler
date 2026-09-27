// ===========================================================================
//   TFT_eSPI configuration for the CPU Scheduler Visualizer
//   1.3" ST7789 240x240 IPS (7-pin, no CS) on ESP32 VSPI
//
//   Copy this file over  <Arduino>/libraries/TFT_eSPI/User_Setup.h
//   (back up the original first), then rebuild the sketch.
// ===========================================================================

#define USER_SETUP_INFO "CPU_Scheduler_ST7789_240x240"

// ---- Driver ---------------------------------------------------------------
#define ST7789_DRIVER
#define TFT_WIDTH   240
#define TFT_HEIGHT  240

// ---- Pins (ESP32) ---------------------------------------------------------
#define TFT_MOSI  23   // module pin "SDA"
#define TFT_SCLK  18   // module pin "SCL"
#define TFT_CS    -1   // module has no CS pin
#define TFT_DC     2   // module pin "DC"
#define TFT_RST    4   // module pin "RES"
// BLK (backlight) is not driven by firmware: tie it to 3V3.

// The CS-less ST7789 modules only respond in SPI mode 3.
#define TFT_SPI_MODE SPI_MODE3

// ---- Fonts used by the sketch (1, 2, 4, 6) --------------------------------
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6

// ---- SPI clock ------------------------------------------------------------
#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
