/******************************************************************
 *  ANTENNA CONTROL CENTER  -  VERSION HORIZONTALE (480 x 320)
 *  ESP32-S3 + LovyanGFX
 *  ST7796 SPI + tactile FT5x06 + WT32 I2C
 *
 *  Auteur : F4BIT Stéphane
 *  Date   : 2026-09
 *  CopyLeft. Sous licence GNU General Public License v3.0
 *
 *  DESIGN LCD ALIGNE SUR LE WEB ETH01
 *
 *  Appui court  : ON / OFF
 *  Appui long   : modifier le nom (700 ms)
 *
 *  Noms mémorisés dans Preferences
 *  Noms envoyés à l'ESP32-ETH01
 *  Clavier virtuel AZERTY + chiffres
 *
 *  ADRESSE IP :
 *  L'ESP32-S3 demande l'adresse IP Ethernet
 *  directement à l'ESP32-ETH01 par I2C avec la commande 0x05.
 ******************************************************************/

#define LGFX_USE_V1

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <LovyanGFX.hpp>


// ============================================================
// ECRAN
// ============================================================

class LGFX : public lgfx::LGFX_Device {

  lgfx::Panel_ST7796 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Touch_FT5x06 _touch_instance;

public:

  LGFX(void) {

    {
      auto cfg = _bus_instance.config();

      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;

      cfg.pin_sclk = 12;
      cfg.pin_mosi = 11;
      cfg.pin_miso = 13;
      cfg.pin_dc   = 46;

      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();

      cfg.pin_cs = 10;
      cfg.pin_rst = -1;

      cfg.panel_width  = 320;
      cfg.panel_height = 480;

      cfg.offset_x = 0;
      cfg.offset_y = 0;

      cfg.invert = true;
      cfg.rgb_order = false;

      _panel_instance.config(cfg);
    }

    {
      auto cfg = _touch_instance.config();

      cfg.x_min = 0;
      cfg.x_max = 319;

      cfg.y_min = 0;
      cfg.y_max = 479;

      cfg.bus_shared = false;

      cfg.pin_sda = 16;
      cfg.pin_scl = 15;

      cfg.pin_int = -1;
      cfg.pin_rst = -1;

      cfg.freq = 400000;

      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }

    setPanel(&_panel_instance);
  }
};

LGFX lcd;

Preferences prefs;


// ============================================================
// CONFIGURATION
// ============================================================

const int BACKLIGHT_PIN = 45;
const int BUZZER_PIN    = 42;

const uint8_t WT32 = 0x12;

const int I2C_SDA = 16;
const int I2C_SCL = 15;

const uint8_t SCREEN_ROTATION = 1;

const uint8_t MAX_NAME_LEN = 14;


// ============================================================
// PALETTE EXACTE DU WEB ETH01
// ============================================================

#define C_BG          0x0884   // #0b1120
#define C_PANEL       0x10C4   // #111827
#define C_WHITE       0xFFDF   // #f8fafc
#define C_MUTED       0x9517   // #94a3b8
#define C_GREEN       0x262B   // #22c55e
#define C_RED         0xF1EB   // #f43f5e

#define C_BORDER      0x294A

#define C_PANEL_LIGHT 0x18E6

#define C_BLUE        0x4D9F

#define C_ORANGE      0xFD20
#define C_RED_DARK    0x9004


// ============================================================
// RELAIS
// ============================================================

char names[8][MAX_NAME_LEN + 1] = {

  "Relais 1",
  "Relais 2",
  "Relais 3",
  "Relais 4",
  "Relais 5",
  "Relais 6",
  "Relais 7",
  "Relais 8"
};

uint8_t states[8] = {0};


// ============================================================
// ADRESSE IP ESP32-ETH01
//
// L'ETH01 renvoie exactement 16 octets.
//
// Exemple :
// "192.168.1.100"
// puis '\0' et des zéros.
//
// La valeur est conservée dans ethernetIP[].
// ============================================================

char ethernetIP[16] = "0.0.0.0";


// ============================================================
// BOUTONS
// ============================================================

struct B {

  int x;
  int y;
  int w;
  int h;
};

B b[8];


// ============================================================
// TACTILE
// ============================================================

unsigned long touchStart = 0;

int touchIndex = -1;

bool wasTouch = false;

unsigned long lastPoll = 0;


// ============================================================
// EDITION
// ============================================================

String editName = "";

int editIndex = -1;

bool editing = false;


// ============================================================
// CLAVIER
// ============================================================

const char keys[] =
  "0123456789AZERTYUIOPQSDFGHJKLMWXCVBN";

const int NKEYS = 36;

const int KB_X0    = 6;
const int KB_Y0    = 72;
const int KB_W     = 44;
const int KB_H     = 38;
const int KB_PX    = 47;
const int KB_PY    = 41;
const int KB_COLS  = 10;
const int KB_ROWS  = 4;

const int ACT_Y    = 246;
const int ACT_H    = 52;
const int ACT_X0   = 8;
const int ACT_W    = 112;
const int ACT_PX   = 118;


// ============================================================
// UTILITAIRES GRAPHIQUES
// ============================================================

void textCenter(
  const String &txt,
  int x,
  int y,
  int font,
  uint16_t color
) {

  lcd.setTextDatum(MC_DATUM);
  lcd.setTextColor(color);
  lcd.drawString(txt, x, y, font);
}


// ============================================================
// LECTURE TACTILE
// ============================================================

bool touchRead(uint16_t &x, uint16_t &y) {

  int32_t tx, ty;

  if (!lcd.getTouch(&tx, &ty))
    return false;

  x = (uint16_t)tx;
  y = (uint16_t)ty;

  return true;
}


// ============================================================
// CREATION DES BOUTONS
// ============================================================

void makeButtons() {

  const int marginX = 9;
  const int gapX = 7;

  const int top = 42;
  const int gapY = 8;

  const int W = 110;
  const int H = 104;

  for (int i = 0; i < 8; i++) {

    int row = i / 4;
    int col = i % 4;

    b[i] = {

      marginX + col * (W + gapX),

      top + row * (H + gapY),

      W,
      H
    };
  }
}


// ============================================================
// I2C : COMMANDE RELAIS
// ============================================================

void sendCmd(uint8_t r, bool on) {

  Wire.beginTransmission(WT32);

  Wire.write(1);
  Wire.write(r);
  Wire.write(on ? 1 : 0);

  Wire.endTransmission();
}


// ============================================================
// I2C : ENVOI NOM
// ============================================================

void sendName(uint8_t r) {

  uint8_t l =
    min(
      (int)strlen(names[r]),
      (int)MAX_NAME_LEN
    );

  Wire.beginTransmission(WT32);

  Wire.write(3);
  Wire.write(r);
  Wire.write(l);

  Wire.write(
    (uint8_t *)names[r],
    l
  );

  Wire.endTransmission();
}


// ============================================================
// CHARGEMENT NOMS
// ============================================================

void loadNames() {

  prefs.begin("names", false);

  for (int i = 0; i < 8; i++) {

    String k = String("n") + i;

    String s =
      prefs.getString(k.c_str(), "");

    if (s.length()) {

      strncpy(
        names[i],
        s.c_str(),
        MAX_NAME_LEN
      );

      names[i][MAX_NAME_LEN] = 0;
    }
  }
}


// ============================================================
// SAUVEGARDE NOM
// ============================================================

void saveName() {

  if (editIndex < 0)
    return;

  editName.trim();

  if (!editName.length())
    return;

  if (editName.length() > MAX_NAME_LEN)
    editName =
      editName.substring(0, MAX_NAME_LEN);

  editName.toCharArray(
    names[editIndex],
    MAX_NAME_LEN + 1
  );

  prefs.putString(
    (String("n") + editIndex).c_str(),
    names[editIndex]
  );

  sendName(editIndex);
}


// ============================================================
// CARTE RELAIS
// ============================================================

void drawRelay(int i) {

  const B &r = b[i];

  bool on = states[i];


  // ----------------------------------------------------------
  // Fond
  // ----------------------------------------------------------

  if (on) {

    lcd.fillRoundRect(
      r.x,
      r.y,
      r.w,
      r.h,
      18,
      C_PANEL
    );

    uint16_t greenDark = 0x1246;

    lcd.fillRoundRect(
      r.x + 2,
      r.y + 2,
      r.w - 4,
      r.h - 4,
      16,
      greenDark
    );

  } else {

    lcd.fillRoundRect(
      r.x,
      r.y,
      r.w,
      r.h,
      18,
      C_PANEL
    );
  }


  // ----------------------------------------------------------
  // Bordure
  // ----------------------------------------------------------

  lcd.drawRoundRect(
    r.x,
    r.y,
    r.w,
    r.h,
    18,
    on ? C_GREEN : C_RED
  );


  // ----------------------------------------------------------
  // Nom
  // ----------------------------------------------------------

  String s = names[i];

  if (s.length() > MAX_NAME_LEN)
    s = s.substring(0, MAX_NAME_LEN);

  int font =
    s.length() > 11 ? 1 : 2;

  textCenter(
    s,
    r.x + r.w / 2,
    r.y + r.h / 2,
    font,
    C_WHITE
  );
}


// ============================================================
// LOGO ANTENNE
// ============================================================

void drawAntennaLogo(int cx, int cy, float scale = 1.0f) {

  // Point central
  lcd.fillCircle(cx, cy - 5 * scale, 4 * scale, C_WHITE);

  // Mât / forme A
  lcd.drawLine(cx, cy, cx - 15 * scale, cy + 25 * scale, C_WHITE);
  lcd.drawLine(cx, cy, cx + 15 * scale, cy + 25 * scale, C_WHITE);
  lcd.drawLine(cx - 9 * scale, cy + 14 * scale, cx + 9 * scale, cy + 14 * scale, C_WHITE);

  // Ondes radio gauche
  lcd.drawArc(cx - 1 * scale, cy - 5 * scale, 18 * scale, 18 * scale, 135, 225, C_WHITE);
  lcd.drawArc(cx - 1 * scale, cy - 5 * scale, 27 * scale, 27 * scale, 135, 225, C_WHITE);

  // Ondes radio droite
  lcd.drawArc(cx + 1 * scale, cy - 5 * scale, 18 * scale, 18 * scale, 315, 45, C_WHITE);
  lcd.drawArc(cx + 1 * scale, cy - 5 * scale, 27 * scale, 27 * scale, 315, 45, C_WHITE);
}


// ============================================================
// ECRAN PRINCIPAL
// ============================================================

void draw() {

  lcd.fillScreen(C_BG);

  lcd.setTextDatum(TL_DATUM);


  // ----------------------------------------------------------
  // HEADER
  // ----------------------------------------------------------

  // Logo antenne juste avant le titre, groupe centré
  {
    const String title = " ANTENNA CONTROL CENTER";
    const int logoW = 20;
    const int gap = 8;
    lcd.setTextFont(2);
    const int titleW = lcd.textWidth(title);
    const int totalW = logoW + gap + titleW;
    const int startX = (lcd.width() - totalW) / 2;

    drawAntennaLogo(
      startX + logoW / 2,
      19,
      0.49f
    );

    textCenter(
      title,
      startX + logoW + gap + titleW / 2,
      20,
      2,
      C_WHITE
    );
  }


  // ----------------------------------------------------------
  // RELAIS
  // ----------------------------------------------------------

  for (int i = 0; i < 8; i++)
    drawRelay(i);


  // ----------------------------------------------------------
  // FOOTER
  // ----------------------------------------------------------

  lcd.setTextDatum(MC_DATUM);

  lcd.setTextColor(C_MUTED);


  // ----------------------------------------------------------
  // ADRESSE IP ESP32-ETH01
  // ----------------------------------------------------------

  lcd.drawString(
    String("IP : ") + ethernetIP,
    lcd.width() / 2,
    lcd.height() - 27,
    1
  );


  // ----------------------------------------------------------
  // COPYRIGHT
  // ----------------------------------------------------------

  lcd.drawString(
    "2026 - F4BIT@CopyLeft. Sous licence GNU General Public License v3.0",
    lcd.width() / 2,
    lcd.height() - 10,
    1
  );

  lcd.setTextDatum(TL_DATUM);
}


// ============================================================
// LECTURE ETATS RELAIS
// ============================================================

void readStates() {

  Wire.beginTransmission(WT32);

  Wire.write(2);

  Wire.endTransmission();

  delay(2);

  Wire.requestFrom(
    WT32,
    (uint8_t)8
  );

  for (int i = 0; i < 8; i++) {

    states[i] =
      Wire.available()
      ? Wire.read()
      : 0;
  }
}


// ============================================================
// LECTURE ADRESSE IP ESP32-ETH01
//
// Protocole ETH01 :
//
// LCD -> ETH01
//   0x05
//
// ETH01 -> LCD
//   16 octets
//
// Exemple :
//   "192.168.1.100"
//   + '\0'
//   + zéros
// ============================================================

bool readEthernetIP() {

  Wire.beginTransmission(WT32);

  Wire.write(0x05);

  uint8_t error =
    Wire.endTransmission(false);

  if (error != 0) {

    return false;
  }


  uint8_t received =
    Wire.requestFrom(
      WT32,
      (uint8_t)16
    );


  if (received != 16) {

    while (Wire.available()) {
      Wire.read();
    }

    return false;
  }


  char newIP[16];

  for (uint8_t i = 0; i < 16; i++) {

    if (Wire.available()) {

      newIP[i] =
        (char)Wire.read();

    } else {

      newIP[i] = '\0';
    }
  }


  newIP[15] = '\0';


  // ----------------------------------------------------------
  // Mise à jour du cache
  // ----------------------------------------------------------

  strncpy(
    ethernetIP,
    newIP,
    sizeof(ethernetIP)
  );

  ethernetIP[
    sizeof(ethernetIP) - 1
  ] = '\0';


  return true;
}


// ============================================================
// CLAVIER : TOUCHE
// ============================================================

void drawKey(
  int x,
  int y,
  int w,
  int h,
  String label,
  uint16_t color = C_PANEL
) {

  lcd.fillRoundRect(
    x,
    y,
    w,
    h,
    8,
    color
  );

  lcd.drawRoundRect(
    x,
    y,
    w,
    h,
    8,
    C_BORDER
  );

  textCenter(
    label,
    x + w / 2,
    y + h / 2,
    2,
    C_WHITE
  );
}


// ============================================================
// ECRAN CLAVIER
// ============================================================

void keyboard() {

  lcd.fillScreen(C_BG);


  // ----------------------------------------------------------
  // HEADER
  // ----------------------------------------------------------

  lcd.setTextDatum(MC_DATUM);

  lcd.setTextColor(C_WHITE);

  lcd.drawString(
    "RENOMMER LES BOUTONS",
    lcd.width() / 2,
    14,
    2
  );


  // ----------------------------------------------------------
  // CHAMP NOM
  // ----------------------------------------------------------

  lcd.fillRoundRect(
    8,
    30,
    464,
    34,
    10,
    C_PANEL
  );

  lcd.drawRoundRect(
    8,
    30,
    464,
    34,
    10,
    C_BORDER
  );

  lcd.setTextDatum(ML_DATUM);

  lcd.setTextColor(C_WHITE);

  String displayed = editName;

  if (displayed.length() == 0)
    displayed = "_";

  lcd.drawString(
    displayed,
    18,
    47,
    2
  );


  // ----------------------------------------------------------
  // COMPTEUR
  // ----------------------------------------------------------

  lcd.setTextDatum(MR_DATUM);

  lcd.setTextColor(C_MUTED);

  lcd.drawString(
    String(editName.length()) + "/" + String(MAX_NAME_LEN),
    462,
    48,
    1
  );


  // ----------------------------------------------------------
  // CLAVIER
  // ----------------------------------------------------------

  for (int i = 0; i < NKEYS; i++) {

    int row = i / KB_COLS;
    int col = i % KB_COLS;

    int x = KB_X0 + col * KB_PX;
    int y = KB_Y0 + row * KB_PY;

    drawKey(
      x,
      y,
      KB_W,
      KB_H,
      String(keys[i]),
      C_PANEL
    );
  }


  // ----------------------------------------------------------
  // ACTIONS
  // ----------------------------------------------------------

  drawKey(
    ACT_X0 + 0 * ACT_PX,
    ACT_Y,
    ACT_W,
    ACT_H,
    "EFFACER",
    C_ORANGE
  );

  drawKey(
    ACT_X0 + 1 * ACT_PX,
    ACT_Y,
    ACT_W,
    ACT_H,
    "ESPACE",
    C_BLUE
  );

  drawKey(
    ACT_X0 + 2 * ACT_PX,
    ACT_Y,
    ACT_W,
    ACT_H,
    "ANNULER",
    C_RED_DARK
  );

  drawKey(
    ACT_X0 + 3 * ACT_PX,
    ACT_Y,
    ACT_W,
    ACT_H,
    "VALIDER",
    C_GREEN
  );
}


// ============================================================
// GESTION CLAVIER
// ============================================================

void editTouch(
  uint16_t x,
  uint16_t y
) {


  // ----------------------------------------------------------
  // TOUCHES AZERTY + CHIFFRES
  // ----------------------------------------------------------

  if (
    y >= KB_Y0 &&
    y < KB_Y0 + KB_ROWS * KB_PY
  ) {

    if (x >= KB_X0) {

      int row =
        (y - KB_Y0) / KB_PY;

      int col =
        (x - KB_X0) / KB_PX;

      if (
        col >= 0 &&
        col < KB_COLS
      ) {

        int k =
          row * KB_COLS + col;

        if (k < NKEYS) {

          if (editName.length() < MAX_NAME_LEN)
            editName += keys[k];

          keyboard();
        }
      }
    }

    return;
  }


  // ----------------------------------------------------------
  // ACTIONS
  // ----------------------------------------------------------

  if (
    y >= ACT_Y &&
    y < ACT_Y + ACT_H &&
    x >= ACT_X0
  ) {

    int a =
      (x - ACT_X0) / ACT_PX;

    switch (a) {

      case 0:

        if (editName.length())
          editName.remove(
            editName.length() - 1
          );

        keyboard();

        break;


      case 1:

        if (editName.length() < MAX_NAME_LEN)
          editName += " ";

        keyboard();

        break;


      case 2:

        editing = false;

        draw();

        break;


      case 3:

        saveName();

        editing = false;

        draw();

        break;
    }

    return;
  }
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);


  // ----------------------------------------------------------
  // RETROECLAIRAGE
  // ----------------------------------------------------------

  pinMode(
    BACKLIGHT_PIN,
    OUTPUT
  );

  digitalWrite(
    BACKLIGHT_PIN,
    HIGH
  );


  // ----------------------------------------------------------
  // ECRAN
  // ----------------------------------------------------------

  lcd.init();

  lcd.setRotation(
    SCREEN_ROTATION
  );


  // ----------------------------------------------------------
  // I2C
  // ----------------------------------------------------------

  Wire.begin(
    I2C_SDA,
    I2C_SCL,
    100000
  );


  // ----------------------------------------------------------
  // NOMS
  // ----------------------------------------------------------

  loadNames();

  makeButtons();


  // ----------------------------------------------------------
  // ENVOI DES NOMS
  // ----------------------------------------------------------

  for (int i = 0; i < 8; i++) {

    sendName(i);

    delay(2);
  }


  // ----------------------------------------------------------
  // ETATS RELAIS
  // ----------------------------------------------------------

  readStates();


  // ----------------------------------------------------------
  // LECTURE IP ETH01
  // ----------------------------------------------------------

  readEthernetIP();


  // ----------------------------------------------------------
  // AFFICHAGE
  // ----------------------------------------------------------

  draw();
}


// ============================================================
// LOOP
// ============================================================

void loop() {


  // ==========================================================
  // MODE EDITION
  // ==========================================================

  if (editing) {

    uint16_t x, y;

    bool t =
      touchRead(x, y);

    if (
      t &&
      !wasTouch
    ) {

      editTouch(x, y);
    }

    wasTouch = t;

    delay(20);

    return;
  }


  // ==========================================================
  // MODE NORMAL
  // ==========================================================

  uint16_t x, y;

  bool t =
    touchRead(x, y);


  // ----------------------------------------------------------
  // DEBUT APPUI
  // ----------------------------------------------------------

  if (
    t &&
    !wasTouch
  ) {

    for (int i = 0; i < 8; i++) {

      if (
        x >= b[i].x &&
        x < b[i].x + b[i].w &&
        y >= b[i].y &&
        y < b[i].y + b[i].h
      ) {

        touchIndex = i;

        touchStart =
          millis();

        break;
      }
    }
  }


  // ----------------------------------------------------------
  // FIN APPUI
  // ----------------------------------------------------------

  if (
    !t &&
    wasTouch &&
    touchIndex >= 0
  ) {

    unsigned long d =
      millis() - touchStart;


    // --------------------------------------------------------
    // APPUI LONG
    // --------------------------------------------------------

    if (d >= 700) {

      editIndex =
        touchIndex;

      editName =
        names[editIndex];

      editing = true;

      keyboard();
    }


    // --------------------------------------------------------
    // APPUI COURT
    // --------------------------------------------------------

    else {

      sendCmd(
        touchIndex,
        !states[touchIndex]
      );

      delay(20);

      readStates();

      draw();
    }

    touchIndex = -1;
  }


  wasTouch = t;


  // ==========================================================
  // ACTUALISATION AUTOMATIQUE
  // ==========================================================

  if (
    millis() - lastPoll > 500
  ) {

    lastPoll = millis();

    bool redraw = false;


    // --------------------------------------------------------
    // ETATS RELAIS
    // --------------------------------------------------------

    uint8_t old[8];

    memcpy(
      old,
      states,
      8
    );

    readStates();


    for (int i = 0; i < 8; i++) {

      if (
        old[i] != states[i]
      ) {

        redraw = true;

        break;
      }
    }


    // --------------------------------------------------------
    // ADRESSE IP ESP32-ETH01
    // --------------------------------------------------------

    char oldIP[16];

    strncpy(
      oldIP,
      ethernetIP,
      sizeof(oldIP)
    );

    oldIP[
      sizeof(oldIP) - 1
    ] = '\0';


    readEthernetIP();


    if (
      strcmp(
        oldIP,
        ethernetIP
      ) != 0
    ) {

      redraw = true;
    }


    // --------------------------------------------------------
    // RAFRAICHISSEMENT ECRAN
    // --------------------------------------------------------

    if (redraw) {

      draw();
    }
  }


  delay(10);
}
