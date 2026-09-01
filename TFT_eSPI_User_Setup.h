/* A ADAPTER AUX GPIO DE TA CARTE ESP32-S3 4 pouces.
   Ce fichier est un exemple de configuration TFT_eSPI.
   Le brochage exact du LCD n'est pas garanti sans photo/revision.
*/
#define USER_SETUP_INFO "ESP32-S3 4in 320x480 - WT32 relais"
#define ST7796_DRIVER
#define TFT_WIDTH  320
#define TFT_HEIGHT 480
#define TFT_MISO  13
#define TFT_MOSI  11
#define TFT_SCLK  12
#define TFT_CS    10
#define TFT_DC     9
#define TFT_RST   -1
#define TFT_BL    45
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_GFXFF
#define SPI_FREQUENCY 40000000
