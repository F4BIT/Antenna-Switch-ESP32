/*
 * ============================================================
 * ANTENNA CONTROL CENTER - ESP32 RELAY V2
 * Arduino ESP32 Core 2.0.17
 * ============================================================
 *
 * ============================================================
 * GPIO RELAIS
 * ============================================================
 *
 * Relay 1 -> GPIO 4
 * Relay 2 -> GPIO 5
 * Relay 3 -> GPIO 6
 * Relay 4 -> GPIO 7
 * Relay 5 -> GPIO 8
 * Relay 6 -> GPIO 9
 * Relay 7 -> GPIO 10
 * Relay 8 -> GPIO 11
 *
 * ============================================================
 * GPIO I2C SLAVE
 * ============================================================
 *
 * Address : 0x12
 * SDA     : GPIO 12
 * SCL     : GPIO 13
 * Speed   : 100 kHz
 */
// ============================================================
// CONFIGURATION
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>

// I2C
#define I2C_ADDRESS 0x12
#define I2C_SDA     12
#define I2C_SCL     13
#define I2C_SPEED   100000

// Relais
#define RELAY_COUNT 8
#define MAX_NAME_LEN 14
#define NAME_SLOT_LEN (MAX_NAME_LEN + 1)

// Relais actifs à LOW
const uint8_t relayPins[RELAY_COUNT] = {
  4, 5, 6, 7, 8, 9, 10, 11
};

// ============================================================
// VARIABLES
// ============================================================

Preferences preferences;

// Noms des relais
char relayNames[RELAY_COUNT][NAME_SLOT_LEN];

// Buffer de réception I2C
volatile bool commandPending = false;
volatile uint8_t rxLength = 0;
volatile uint8_t rxBuffer[128];

// Dernière commande reçue
volatile uint8_t lastCommand = 0;

// ============================================================
// RELAIS
// ============================================================

void setRelay(uint8_t relay, bool state)
{
  if (relay >= RELAY_COUNT)
    return;

  // Relais actifs LOW
  // state = true  -> ON  -> LOW
  // state = false -> OFF -> HIGH

  digitalWrite(
    relayPins[relay],
    state ? LOW : HIGH
  );
}

// ------------------------------------------------------------

bool getRelay(uint8_t relay)
{
  if (relay >= RELAY_COUNT)
    return false;

  return digitalRead(relayPins[relay]) == LOW;
}

// ------------------------------------------------------------

void allRelaysOff()
{
  for (uint8_t i = 0; i < RELAY_COUNT; i++)
  {
    digitalWrite(
      relayPins[i],
      HIGH
    );
  }
}

// ============================================================
// NOMS DES RELAIS
// ============================================================

void loadNames()
{
  preferences.begin("relay", true);

  for (uint8_t i = 0; i < RELAY_COUNT; i++)
  {
    String key = "name" + String(i);

    String name = preferences.getString(
      key.c_str(),
      "RELAY" + String(i + 1)
    );

    name.toCharArray(
      relayNames[i],
      NAME_SLOT_LEN
    );
  }

  preferences.end();
}

// ------------------------------------------------------------

void saveName(uint8_t relay, const char *name)
{
  if (relay >= RELAY_COUNT)
    return;

  preferences.begin("relay", false);

  String key = "name" + String(relay);

  preferences.putString(
    key.c_str(),
    name
  );

  preferences.end();

  strncpy(
    relayNames[relay],
    name,
    MAX_NAME_LEN
  );

  relayNames[relay][MAX_NAME_LEN] = '\0';
}

// ============================================================
// CALLBACK I2C : RECEPTION
// ============================================================

void onReceiveI2C(int count)
{
  uint8_t len = 0;

  while (Wire.available() && len < sizeof(rxBuffer))
  {
    rxBuffer[len++] = Wire.read();
  }

  rxLength = len;

  if (len > 0)
  {
    lastCommand = rxBuffer[0];
    commandPending = true;
  }
}

// ============================================================
// CALLBACK I2C : DEMANDE
// ============================================================

void onRequestI2C()
{
  // ----------------------------------------------------------
  // 0x02 = lecture état des 8 relais
  // ----------------------------------------------------------

  if (lastCommand == 0x02)
  {
    uint8_t states[RELAY_COUNT];

    for (uint8_t i = 0; i < RELAY_COUNT; i++)
    {
      states[i] = getRelay(i) ? 1 : 0;
    }

    Wire.write(
      states,
      RELAY_COUNT
    );
  }

  // ----------------------------------------------------------
  // 0x04 = lecture de tous les noms
  // 8 x 15 = 120 octets
  // ----------------------------------------------------------

  else if (lastCommand == 0x04)
  {
    uint8_t buffer[RELAY_COUNT * NAME_SLOT_LEN];

    memset(
      buffer,
      0,
      sizeof(buffer)
    );

    for (uint8_t i = 0; i < RELAY_COUNT; i++)
    {
      memcpy(
        &buffer[i * NAME_SLOT_LEN],
        relayNames[i],
        NAME_SLOT_LEN
      );
    }

    Wire.write(
      buffer,
      sizeof(buffer)
    );
  }
}

// ============================================================
// TRAITEMENT DES COMMANDES I2C
// ============================================================

void processI2C()
{
  if (!commandPending)
    return;

  uint8_t buffer[128];
  uint8_t len;

  // Copie protégée du buffer reçu
  noInterrupts();

  len = rxLength;

  if (len > sizeof(buffer))
    len = sizeof(buffer);

  memcpy(
    buffer,
    (const void *)rxBuffer,
    len
  );

  commandPending = false;

  interrupts();

  if (len == 0)
    return;

  uint8_t command = buffer[0];

  // ==========================================================
  // 0x01 = SET RELAY
  //
  // [0] = 0x01
  // [1] = numéro relais 0..7
  // [2] = état 0/1
  // ==========================================================

  if (command == 0x01)
  {
    if (len >= 3)
    {
      uint8_t relay = buffer[1];
      uint8_t state = buffer[2];

      if (relay < RELAY_COUNT)
      {
        setRelay(
          relay,
          state != 0
        );

        Serial0.print("Relay ");
        Serial0.print(relay + 1);
        Serial0.print(" -> ");

        if (state)
          Serial0.println("ON");
        else
          Serial0.println("OFF");
      }
    }
  }

  // ==========================================================
  // 0x02 = GET STATES
  //
  // La réponse est envoyée dans onRequestI2C()
  // ==========================================================

  else if (command == 0x02)
  {
    Serial0.println("I2C: GET STATES");
  }

  // ==========================================================
  // 0x03 = SET NAME
  //
  // [0] = 0x03
  // [1] = numéro relais
  // [2] = longueur
  // [3...] = caractères
  // ==========================================================

  else if (command == 0x03)
  {
    if (len >= 3)
    {
      uint8_t relay = buffer[1];
      uint8_t nameLength = buffer[2];

      if (relay < RELAY_COUNT)
      {
        if (nameLength > MAX_NAME_LEN)
          nameLength = MAX_NAME_LEN;

        if (nameLength > len - 3)
          nameLength = len - 3;

        char name[NAME_SLOT_LEN];

        memset(
          name,
          0,
          sizeof(name)
        );

        memcpy(
          name,
          &buffer[3],
          nameLength
        );

        name[MAX_NAME_LEN] = '\0';

        saveName(
          relay,
          name
        );

        Serial0.print("Relay ");
        Serial0.print(relay + 1);
        Serial0.print(" name = ");
        Serial0.println(name);
      }
    }
  }

  // ==========================================================
  // 0x04 = GET NAMES
  //
  // La réponse est envoyée dans onRequestI2C()
  // ==========================================================

  else if (command == 0x04)
  {
    Serial0.println("I2C: GET NAMES");
  }

  // ==========================================================
  // COMMANDE INCONNUE
  // ==========================================================

  else
  {
    Serial0.print("I2C: Unknown command 0x");

    if (command < 0x10)
      Serial0.print("0");

    Serial0.println(
      command,
      HEX
    );
  }
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  // ----------------------------------------------------------
  // UART0
  // ----------------------------------------------------------

  Serial0.begin(115200);

  delay(500);

  Serial0.println();
  Serial0.println("========================================");
  Serial0.println(" ESP32-S3 N16R8 - RELAY CONTROLLER");
  Serial0.println("========================================");

  // ----------------------------------------------------------
  // GPIO RELAIS
  // ----------------------------------------------------------

  Serial0.println("Initializing relays...");

  for (uint8_t i = 0; i < RELAY_COUNT; i++)
  {
    pinMode(
      relayPins[i],
      OUTPUT
    );

    // Relais OFF
    digitalWrite(
      relayPins[i],
      HIGH
    );
  }

  allRelaysOff();

  Serial0.println("All relays OFF");

  // ----------------------------------------------------------
  // CHARGEMENT DES NOMS
  // ----------------------------------------------------------

  loadNames();

  Serial0.println("Relay names loaded:");

  for (uint8_t i = 0; i < RELAY_COUNT; i++)
  {
    Serial0.print("  Relay ");
    Serial0.print(i + 1);
    Serial0.print(": ");
    Serial0.println(relayNames[i]);
  }

  // ----------------------------------------------------------
  // I2C
  // ----------------------------------------------------------

  Serial0.println();
  Serial0.println("Initializing I2C...");

  Wire.begin(
    I2C_ADDRESS,
    I2C_SDA,
    I2C_SCL,
    I2C_SPEED
  );

  Wire.onReceive(onReceiveI2C);
  Wire.onRequest(onRequestI2C);

  Serial0.print("I2C address : 0x");
  Serial0.println(
    I2C_ADDRESS,
    HEX
  );

  Serial0.print("I2C SDA     : GPIO");
  Serial0.println(I2C_SDA);

  Serial0.print("I2C SCL     : GPIO");
  Serial0.println(I2C_SCL);

  Serial0.print("I2C speed   : ");
  Serial0.print(I2C_SPEED);
  Serial0.println(" Hz");

  // ----------------------------------------------------------
  // FIN DU BOOT
  // ----------------------------------------------------------

  Serial0.println();
  Serial0.println("========================================");
  Serial0.println(" READY");
  Serial0.println("========================================");
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
  processI2C();

  delay(2);
}
