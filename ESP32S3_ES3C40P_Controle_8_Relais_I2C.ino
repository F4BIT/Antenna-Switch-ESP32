/* ESP32-S3 + LovyanGFX (ST7796 SPI + tactile FT5x06) + WT32 I2C
   Les noms sont modifiables sur l'ecran par appui long (700 ms).
   Les noms sont memorises dans Preferences et envoyes au WT32.
   Clavier virtuel : chiffres (0-9) + lettres (AZERTY sans accents).

   MODIFICATION :
   - ESPACE et ANNULER sont maintenant deux touches distinctes.
*/

#define LGFX_USE_V1

#include <LovyanGFX.hpp>
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>

// ---------- Configuration matérielle écran + tactile ----------

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Touch_FT5x06 _touch_instance;

public:
  LGFX(void) {

    { // Configuration du bus SPI matériel
      auto cfg = _bus_instance.config();

      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;

      cfg.pin_sclk = 12;
      cfg.pin_mosi = 11;
      cfg.pin_miso = 13;
      cfg.pin_dc = 46;

      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    { // Configuration de la surface LCD
      auto cfg = _panel_instance.config();

      cfg.pin_cs = 10;
      cfg.pin_rst = -1;

      cfg.panel_width = 320;
      cfg.panel_height = 480;

      cfg.offset_x = 0;
      cfg.offset_y = 0;

      cfg.invert = true;
      cfg.rgb_order = false;

      _panel_instance.config(cfg);
    }

    { // Configuration de l'écran tactile
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

// ---------- Broches ----------

const int BACKLIGHT_PIN = 45;
const int BUZZER_PIN = 42;

#define TFT_LIGHTBLUE 0xFBE0

// ---------- I2C vers le WT32-ETH01 ----------
// Partage les broches du tactile

const uint8_t WT32 = 0x12;

const int I2C_SDA = 16;
const int I2C_SCL = 15;

// ---------- Relais ----------

char names[8][18] = {
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

// ---------- Boutons ----------

struct B {
  int x;
  int y;
  int w;
  int h;
};

B b[8];

// ---------- Gestion tactile ----------

unsigned long touchStart = 0;
int touchIndex = -1;
bool wasTouch = false;

unsigned long lastPoll = 0;

// ---------- Edition du nom ----------

String editName = "";
int editIndex = -1;
bool editing = false;

// ---------- Clavier ----------
// Chiffres (10) + lettres AZERTY (26) = 36 touches

const char keys[] =
  "0123456789AZERTYUIOPQSDFGHJKLMWXCVBN";

const int NKEYS = 36;


// ============================================================
// LECTURE DU TACTILE
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
// CREATION DES BOUTONS RELAIS
// ============================================================

void makeButtons() {

  for (int i = 0; i < 8; i++) {

    int row = i / 2;
    int col = i % 2;

    b[i] = {
      10 + col * 155,
      65 + row * 91,
      145,
      82
    };
  }
}


// ============================================================
// ENVOI COMMANDE RELAIS AU WT32
// ============================================================

void sendCmd(uint8_t r, bool on) {

  Wire.beginTransmission(WT32);

  Wire.write(1);
  Wire.write(r);
  Wire.write(on ? 1 : 0);

  Wire.endTransmission();
}


// ============================================================
// ENVOI NOM AU WT32
// ============================================================

void sendName(uint8_t r) {

  uint8_t l = min((int)strlen(names[r]), 17);

  Wire.beginTransmission(WT32);

  Wire.write(3);
  Wire.write(r);
  Wire.write(l);

  Wire.write((uint8_t *)names[r], l);

  Wire.endTransmission();
}


// ============================================================
// CHARGEMENT DES NOMS
// ============================================================

void loadNames() {

  prefs.begin("names", false);

  for (int i = 0; i < 8; i++) {

    String k = String("n") + i;
    String s = prefs.getString(k.c_str(), "");

    if (s.length()) {

      strncpy(names[i], s.c_str(), 17);
      names[i][17] = 0;
    }
  }
}


// ============================================================
// SAUVEGARDE DU NOM
// ============================================================

void saveName() {

  if (editIndex < 0)
    return;

  editName.trim();

  if (!editName.length())
    return;

  // Limite à 17 caractères
  if (editName.length() > 17)
    editName = editName.substring(0, 17);

  editName.toCharArray(names[editIndex], 18);

  prefs.putString(
    (String("n") + editIndex).c_str(),
    names[editIndex]
  );

  sendName(editIndex);
}


// ============================================================
// AFFICHAGE DE L'ECRAN PRINCIPAL
// ============================================================

void draw() {

  lcd.fillScreen(TFT_BLACK);

  // Barre supérieure
  lcd.fillRect(
    0,
    0,
    320,
    52,
    TFT_BLACK
  );

  lcd.setTextColor(
    TFT_WHITE,
    TFT_BLACK
  );

  lcd.setTextDatum(MC_DATUM);

  lcd.drawString(
    "COMMANDE RELAIS D'ANTENNES",
    160,
    17,
    2
  );

  lcd.drawString(
    "",
    160,
    38,
    2
  );

  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setTextDatum(MC_DATUM);
  lcd.drawString("F4BIT@2026-Copyleft", 160, 465, 2);
  lcd.setTextDatum(TL_DATUM);

  // Boutons relais
  for (int i = 0; i < 8; i++) {

    uint16_t c =
      states[i]
      ? TFT_GREEN
      : TFT_RED;

    lcd.fillRoundRect(
      b[i].x,
      b[i].y,
      b[i].w,
      b[i].h,
      9,
      c
    );

    lcd.drawRoundRect(
      b[i].x,
      b[i].y,
      b[i].w,
      b[i].h,
      9,
      TFT_WHITE
    );

    lcd.setTextColor(
      TFT_WHITE,
      c
    );

    lcd.setTextDatum(MC_DATUM);

    String s = names[i];

    if (s.length() > 15)
      s = s.substring(0, 15);

    lcd.drawString(
      s,
      b[i].x + 72,
      b[i].y + 22,
      s.length() > 10 ? 1 : 2
    );

    lcd.drawString(
      states[i] ? "ON" : "OFF",
      b[i].x + 72,
      b[i].y + 58,
      4
    );
  }

  lcd.setTextDatum(TL_DATUM);
}


// ============================================================
// LECTURE DES ETATS DES RELAIS
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
// AFFICHAGE DU CLAVIER
// ============================================================

void keyboard() {

  lcd.fillScreen(TFT_BLACK);

  lcd.setTextColor(
    TFT_WHITE,
    TFT_BLACK
  );

  lcd.setTextDatum(TL_DATUM);

  lcd.drawString(
    "Nouveau nom :",
    8,
    8,
    2
  );

  // Zone du nom
  lcd.fillRoundRect(
    8,
    35,
    304,
    42,
    6,
    TFT_DARKGREY
  );

  lcd.setTextDatum(MC_DATUM);

  lcd.drawString(
    editName,
    160,
    56,
    2
  );

  // ---------- Touches clavier ----------

  int W = 29;
  int H = 43;

  for (int i = 0; i < NKEYS; i++) {

    int row = i / 10;
    int col = i % 10;

    int x = 5 + col * 31;
    int y = 90 + row * 47;

    lcd.fillRoundRect(
      x,
      y,
      W,
      H,
      5,
      TFT_NAVY
    );

    lcd.setTextColor(
      TFT_WHITE,
      TFT_NAVY
    );

    lcd.drawString(
      String(keys[i]),
      x + W / 2,
      y + H / 2,
      2
    );
  }

  // ---------- EFFACER ----------

  lcd.fillRoundRect(
    8,
    286,
    145,
    42,
    6,
    TFT_DARKGREY
  );

  // ---------- VALIDER ----------

  lcd.fillRoundRect(
    167,
    286,
    145,
    42,
    6,
    TFT_GREEN
  );

  lcd.setTextColor(TFT_WHITE);

  lcd.drawString(
    "EFFACER",
    80,
    307,
    2
  );

  lcd.drawString(
    "VALIDER",
    239,
    307,
    2
  );

  // ========================================================
  // NOUVELLES TOUCHES ESPACE / ANNULER
  // ========================================================

  // ESPACE
  lcd.fillRoundRect(
    8,
    338,
    145,
    42,
    6,
    TFT_NAVY
  );

  // ANNULER
  lcd.fillRoundRect(
    167,
    338,
    145,
    42,
    6,
    TFT_RED
  );

  lcd.setTextColor(TFT_WHITE);

  lcd.drawString(
    "ESPACE",
    80,
    359,
    2
  );

  lcd.drawString(
    "ANNULER",
    239,
    359,
    2
  );

  lcd.setTextDatum(TL_DATUM);
}


// ============================================================
// GESTION DES TOUCHES DU CLAVIER
// ============================================================

void editTouch(uint16_t x, uint16_t y) {

  // ========================================================
  // TOUCHES CHIFFRES + LETTRES
  // ========================================================

  if (y >= 90 && y < 278) {

    int row = (y - 90) / 47;
    int col = (x - 5) / 31;

    if (col >= 0 && col < 10) {

      int k = row * 10 + col;

      if (k < NKEYS) {

        // Maximum 17 caractères
        if (editName.length() < 17) {

          editName += keys[k];
        }

        keyboard();
      }
    }

    return;
  }


  // ========================================================
  // EFFACER / VALIDER
  // ========================================================

  if (y >= 286 && y < 328) {

    // EFFACER
    if (x < 155) {

      if (editName.length()) {

        editName.remove(
          editName.length() - 1
        );
      }
    }

    // VALIDER
    else {

      saveName();

      editing = false;

      draw();

      return;
    }

    keyboard();

    return;
  }


  // ========================================================
  // ESPACE
  // ========================================================

  if (y >= 338 && y < 380 && x < 155) {

    // Maximum 17 caractères
    if (editName.length() < 17) {

      editName += " ";
    }

    keyboard();

    return;
  }


  // ========================================================
  // ANNULER
  // ========================================================

  if (y >= 338 && y < 380 && x >= 155) {

    // Quitter sans sauvegarder
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

  // ---------- Rétroéclairage ----------

  pinMode(
    BACKLIGHT_PIN,
    OUTPUT
  );

  digitalWrite(
    BACKLIGHT_PIN,
    HIGH
  );


  // ---------- Ecran ----------

  lcd.init();

  lcd.setRotation(0);


  // ---------- I2C ----------

  Wire.begin(
    I2C_SDA,
    I2C_SCL,
    100000
  );


  // ---------- Noms ----------

  loadNames();

  makeButtons();


  // ---------- Envoi des noms au WT32 ----------

  for (int i = 0; i < 8; i++) {

    sendName(i);
  }


  // ---------- Lecture des états ----------

  readStates();


  // ---------- Affichage ----------

  draw();
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  // ========================================================
  // MODE EDITION
  // ========================================================

  if (editing) {

    uint16_t x, y;

    bool t = touchRead(x, y);

    if (t && !wasTouch) {

      editTouch(x, y);
    }

    wasTouch = t;

    delay(20);

    return;
  }


  // ========================================================
  // MODE NORMAL
  // ========================================================

  uint16_t x, y;

  bool t = touchRead(x, y);


  // ---------- Début d'un appui ----------

  if (t && !wasTouch) {

    for (int i = 0; i < 8; i++) {

      if (
        x >= b[i].x &&
        x < b[i].x + b[i].w &&
        y >= b[i].y &&
        y < b[i].y + b[i].h
      ) {

        touchIndex = i;
        touchStart = millis();

        break;
      }
    }
  }


  // ---------- Fin d'un appui ----------

  if (
    !t &&
    wasTouch &&
    touchIndex >= 0
  ) {

    unsigned long d =
      millis() - touchStart;


    // ---------- Appui long : édition ----------

    if (d >= 700) {

      editIndex = touchIndex;

      editName = names[editIndex];

      editing = true;

      keyboard();
    }


    // ---------- Appui court : relais ----------

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


  // ========================================================
  // ACTUALISATION DES ETATS
  // ========================================================

  if (millis() - lastPoll > 500) {

    lastPoll = millis();

    uint8_t old[8];

    memcpy(
      old,
      states,
      8
    );

    readStates();

    for (int i = 0; i < 8; i++) {

      if (old[i] != states[i]) {

        draw();

        break;
      }
    }
  }


  delay(10);
}
