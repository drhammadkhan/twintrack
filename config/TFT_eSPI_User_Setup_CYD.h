//                            USER DEFINED SETTINGS
//   TFT_eSPI configuration for the "Cheap Yellow Display" (ESP32-2432S028R):
//   ESP32-WROOM-32 with a 2.8-inch 240 x 320 ILI9341 panel on HSPI and an
//   XPT2046 resistive touch controller on its own SPI pins (handled by the
//   sketch, not by TFT_eSPI).
//
//   Copy this file over the installed TFT_eSPI library's User_Setup.h when
//   building the twintrack-cyd sketch. Values follow the community reference
//   configuration from the ESP32-Cheap-Yellow-Display project.
//
//   Variant note: most single-USB CYD boards work with this file unchanged.
//   Some dual-USB (micro-USB + USB-C) revisions ship a panel that needs
//   TFT_INVERSION_ON and/or ST7789_DRIVER instead - see the README.

#define USER_SETUP_INFO "User_Setup"

// ##################################################################################
//
// Section 1. Call up the right driver file and any options for it
//
// ##################################################################################

// Alternative ILI9341 driver, see https://github.com/Bodmer/TFT_eSPI/issues/1172
#define ILI9341_2_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// If colours are swapped or inverted on your board revision, try
// TFT_RGB_ORDER TFT_BGR and/or TFT_INVERSION_ON here.

// ##################################################################################
//
// Section 2. Define the pins that are used to interface with the display here
//
// ##################################################################################

#define TFT_BL   21            // LED back-light control pin
#define TFT_BACKLIGHT_ON HIGH  // Level to turn ON back-light (HIGH or LOW)

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15  // Chip select control pin
#define TFT_DC    2  // Data Command control pin
#define TFT_RST  -1  // Display RESET is connected to the ESP32 board RST

// The XPT2046 touch controller is NOT on the TFT SPI bus on this board.
// The sketch drives it directly on GPIO 25/32/33/36/39, so TOUCH_CS is
// deliberately left undefined here.

// ##################################################################################
//
// Section 3. Define the fonts that are to be used here
//
// ##################################################################################

#define LOAD_GLCD   // Font 1. Original Adafruit 8 pixel font needs ~1820 bytes in FLASH
#define LOAD_FONT2  // Font 2. Small 16 pixel high font, needs ~3534 bytes in FLASH, 96 characters
#define LOAD_FONT4  // Font 4. Medium 26 pixel high font, needs ~5848 bytes in FLASH, 96 characters
#define LOAD_FONT6  // Font 6. Large 48 pixel font, needs ~2666 bytes in FLASH, only characters 1234567890:-.apm
#define LOAD_FONT7  // Font 7. 7 segment 48 pixel font, needs ~2438 bytes in FLASH, only characters 1234567890:-.
#define LOAD_FONT8  // Font 8. Large 75 pixel font needs ~3256 bytes in FLASH, only characters 1234567890:-.
#define LOAD_GFXFF  // FreeFonts. Include access to the 48 Adafruit_GFX free fonts FF1 to FF48 and custom fonts

#define SMOOTH_FONT

// ##################################################################################
//
// Section 4. Other options
//
// ##################################################################################

#define SPI_FREQUENCY  55000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000

// The display sits on the HSPI port on this board.
#define USE_HSPI_PORT
