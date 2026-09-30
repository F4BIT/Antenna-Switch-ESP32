/******************************************************************
 *  ANTENNA CONTROL CENTER  -  VERSION VERTICAL (320 x 480)
 *  ESP32-S3 + LovyanGFX
 *  ST7796 SPI + tactile FT5x06 + WT32 I2C
 *  Auteur : F4BIT Stéphane
 *  Date   : 2026-09
 *  CopyLeft. Sous licence GNU General Public License v3.0
 *  Appui court  : commuter le relais
 *  Appui long   : modifier le nom (700 ms)
 *
 *  Noms mémorisés dans Preferences
 *  Noms envoyés a l'ESP32-ETH01
 *  Clavier virtuel AZERTY + chiffres
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

// Longueur maximale d'un nom
const uint8_t MAX_NAME_LEN = 14;


// ============================================================
// PALETTE MODERNE
// ============================================================

#define C_BG          0x1082
#define C_PANEL       0x18E3
#define C_PANEL2      0x2128

#define C_WHITE       0xFFFF
#define C_TEXT        0xE73C
#define C_MUTED       0x9CF3

#define C_BLUE        0x3D9F
#define C_CYAN        0x5DFF

#define C_GREEN       0x4FE9
#define C_GREEN_DARK  0x1CC5

#define C_RED         0xF206
#define C_RED_DARK    0x9004

#define C_ORANGE      0xFD20

#define C_KEY         0x2128
#define C_KEY_ACTIVE  0x3A56


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


void roundedPanel(
  int x,
  int y,
  int w,
  int h,
  uint16_t color,
  uint16_t border = C_PANEL
) {

  lcd.fillRoundRect(
    x,
    y,
    w,
    h,
    12,
    color
  );

  lcd.drawRoundRect(
    x,
    y,
    w,
    h,
    12,
    border
  );
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

  const int top = 68;
  const int gapY = 7;

  const int W = 147;
  const int H = 82;

  for (int i = 0; i < 8; i++) {

    int row = i / 2;
    int col = i % 2;

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
// PETIT INDICATEUR LED
// ============================================================

void drawStatusDot(
  int x,
  int y,
  bool active
) {

  lcd.fillCircle(
    x,
    y,
    5,
    active ? C_GREEN : C_MUTED
  );

  lcd.drawCircle(
    x,
    y,
    6,
    active ? C_GREEN : C_PANEL2
  );
}


// ============================================================
// CARTE RELAIS
// ============================================================

void drawRelay(int i) {

  const B &r = b[i];

  bool on = states[i];

  uint16_t panelColor =
    on ? C_GREEN_DARK : C_PANEL;

  uint16_t accent =
    on ? C_GREEN : C_RED;


  // ----------------------------------------------------------
  // Fond carte
  // ----------------------------------------------------------

  lcd.fillRoundRect(
    r.x,
    r.y,
    r.w,
    r.h,
    12,
    panelColor
  );


  // ----------------------------------------------------------
  // Bordure
  // ----------------------------------------------------------

  lcd.drawRoundRect(
    r.x,
    r.y,
    r.w,
    r.h,
    12,
    accent
  );


  // ----------------------------------------------------------
  // Nom (centré ; l'état est indiqué par la couleur de la carte)
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
// ECRAN PRINCIPAL
// ============================================================

void draw() {

  lcd.fillScreen(C_BG);

  lcd.setTextDatum(TL_DATUM);


  // ----------------------------------------------------------
  // HEADER
  // ----------------------------------------------------------

  textCenter(
    "ANTENNA CONTROL CENTER",
    lcd.width() / 2,
    28,
    2,
    C_WHITE
  );


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

  lcd.drawString(
    "F4BIT@2026 GNU General Public License v3.0",
    lcd.width() / 2,
    lcd.height() - 18,
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
// CLAVIER : TOUCHE
// ============================================================

void drawKey(
  int x,
  int y,
  int w,
  int h,
  String label,
  uint16_t color = C_KEY
) {

  lcd.fillRoundRect(
    x,
    y,
    w,
    h,
    6,
    color
  );

  lcd.drawRoundRect(
    x,
    y,
    w,
    h,
    6,
    C_PANEL2
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
    16,
    2
  );


  // ----------------------------------------------------------
  // CHAMP NOM
  // ----------------------------------------------------------

  lcd.fillRoundRect(
    8,
    39,
    304,
    43,
    9,
    C_PANEL
  );

  lcd.drawRoundRect(
    8,
    39,
    304,
    43,
    9,
    C_BLUE
  );

  lcd.setTextDatum(ML_DATUM);

  lcd.setTextColor(C_WHITE);

  String displayed = editName;

  if (displayed.length() == 0)
    displayed = "_";

  lcd.drawString(
    displayed,
    18,
    60,
    2
  );


  // ----------------------------------------------------------
  // COMPTEUR
  // ----------------------------------------------------------

  lcd.setTextDatum(MR_DATUM);

  lcd.setTextColor(C_MUTED);

  lcd.drawString(
    String(editName.length()) + "/" + String(MAX_NAME_LEN),
    302,
    61,
    1
  );


  // ----------------------------------------------------------
  // CLAVIER
  // ----------------------------------------------------------

  const int W = 29;
  const int H = 38;

  for (int i = 0; i < NKEYS; i++) {

    int row = i / 10;
    int col = i % 10;

    int x = 5 + col * 31;

    // +10 pixels
    int y = 100 + row * 41;

    drawKey(
      x,
      y,
      W,
      H,
      String(keys[i])
    );
  }


  // ----------------------------------------------------------
  // ACTIONS
  // ----------------------------------------------------------

  drawKey(
    8,
    350,
    145,
    38,
    "EFFACER",
    C_ORANGE
  );

  drawKey(
    167,
    350,
    145,
    38,
    "VALIDER",
    C_GREEN_DARK
  );

  drawKey(
    8,
    395,
    145,
    38,
    "ESPACE",
    C_BLUE
  );

  drawKey(
    167,
    395,
    145,
    38,
    "ANNULER",
    C_RED_DARK
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
    y >= 100 &&
    y < 264
  ) {

    int row =
      (y - 100) / 41;

    int col =
      (x - 5) / 31;

    if (
      col >= 0 &&
      col < 10
    ) {

      int k =
        row * 10 + col;

      if (k < NKEYS) {

        if (editName.length() < MAX_NAME_LEN)
          editName += keys[k];

        keyboard();
      }
    }

    return;
  }


  // ----------------------------------------------------------
  // EFFACER / VALIDER
  // ----------------------------------------------------------

  if (
    y >= 350 &&
    y < 388
  ) {

    if (x < 155) {

      // EFFACER
      if (editName.length())
        editName.remove(
          editName.length() - 1
        );

      keyboard();
    }

    else {

      // VALIDER
      saveName();

      editing = false;

      draw();
    }

    return;
  }


  // ----------------------------------------------------------
  // ESPACE
  // ----------------------------------------------------------

  if (
    y >= 395 &&
    y < 433 &&
    x < 155
  ) {

    if (editName.length() < MAX_NAME_LEN)
      editName += " ";

    keyboard();

    return;
  }


  // ----------------------------------------------------------
  // ANNULER
  // ----------------------------------------------------------

  if (
    y >= 395 &&
    y < 433 &&
    x >= 155
  ) {

    editing = false;

    draw();

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

  lcd.setRotation(0);


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

  for (int i = 0; i < 8; i++)
    sendName(i);


  // ----------------------------------------------------------
  // ETATS
  // ----------------------------------------------------------

  readStates();


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

        draw();

        break;
      }
    }
  }


  delay(10);
}
