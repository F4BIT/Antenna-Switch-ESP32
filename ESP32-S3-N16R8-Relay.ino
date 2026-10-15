/* ============================================================
 * ANTENNA CONTROL CENTER - ESP32-S3 RELAY V3
 * Carte : ESP32-S3 DevKitC-1-N16R8
 * Arduino ESP32 Core 2.0.17
 *
 * Auteur : F4BIT Stéphane
 * Date   : 2026-09
 * Licence: GNU General Public License v3.0
 *
 * GPIO RELAIS (actifs à LOW)
 * Relais 1 -> GPIO 4
 * Relais 2 -> GPIO 5
 * Relais 3 -> GPIO 6
 * Relais 4 -> GPIO 7
 * Relais 5 -> GPIO 8
 * Relais 6 -> GPIO 9
 * Relais 7 -> GPIO 10
 * Relais 8 -> GPIO 11
 * Relais 9 -> GPIO 14
 *
 * I2C ESCLAVE
 * Adresse : 0x12
 * SDA     : GPIO 12
 * SCL     : GPIO 13
 * Vitesse : 100 kHz
 *
 * COMMANDES I2C
 * 0x01 : commander un relais
 * 0x02 : lire les états des 9 relais
 * 0x03 : modifier le nom d'un relais
 * 0x04 : lire les noms des relais
 * ============================================================/

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <string.h>

// ============================================================
// CONFIGURATION
// ============================================================

#define I2C_ADDRESS   0x12
#define I2C_SDA       12
#define I2C_SCL       13
#define I2C_SPEED     100000

#define RELAY_COUNT   9
#define MAX_NAME_LEN  14
#define NAME_SLOT_LEN (MAX_NAME_LEN + 1)

#define RX_BUFFER_SIZE 128

const uint8_t relayPins[RELAY_COUNT] = {
  4, 5, 6, 7, 8, 9, 10, 11, 14
};

// ============================================================
// VARIABLES
// ============================================================

Preferences preferences;

char relayNames[RELAY_COUNT][NAME_SLOT_LEN];

volatile bool commandPending = false;
volatile uint8_t rxLength = 0;
volatile uint8_t rxBuffer[RX_BUFFER_SIZE];
volatile uint8_t lastCommand = 0;

// ============================================================
// RELAIS
// ============================================================

void setRelay(uint8_t relay, bool state)
{
  if (relay >= RELAY_COUNT)
    return;

  // Relais actifs à LOW :
  // ON  = LOW
  // OFF = HIGH

  digitalWrite(relayPins[relay], state ? LOW : HIGH);
}

bool getRelay(uint8_t relay)
{
  if (relay >= RELAY_COUNT)
    return false;

  return digitalRead(relayPins[relay]) == LOW;
}

void allRelaysOff()
{
  for (uint8_t i = 0; i < RELAY_COUNT; i++)
  {
    digitalWrite(relayPins[i], HIGH);
  }
}

// ============================================================
// NOMS DES RELAIS - PREFERENCES
// ============================================================

void loadNames()
{
  preferences.begin("relay", true);

  for (uint8_t i = 0; i < RELAY_COUNT; i++)
  {
    String key = "name" + String(i);
    String defaultName = "RELAY" + String(i + 1);

    String name = preferences.getString(
      key.c_str(),
      defaultName.c_str()
    );

    name.toCharArray(relayNames[i], NAME_SLOT_LEN);
    relayNames[i][MAX_NAME_LEN] = '\0';
  }

  preferences.end();
}

void saveName(uint8_t relay, const char *name)
{
  if (relay >= RELAY_COUNT || name == nullptr)
    return;

  char safeName[NAME_SLOT_LEN];

  strncpy(safeName, name, MAX_NAME_LEN);
  safeName[MAX_NAME_LEN] = '\0';

  preferences.begin("relay", false);

  String key = "name" + String(relay);
  preferences.putString(key.c_str(), safeName);

  preferences.end();

  strncpy(relayNames[relay], safeName, MAX_NAME_LEN);
  relayNames[relay][MAX_NAME_LEN] = '\0';
}

// ============================================================
// CALLBACK I2C : RECEPTION
// ============================================================

void onReceiveI2C(int count)
{
  uint8_t len = 0;

  while (Wire.available() && len < RX_BUFFER_SIZE)
  {
    rxBuffer[len++] = (uint8_t)Wire.read();
  }

  // Les commandes sont traitées ensuite dans loop().
  if (len > 0)
  {
    lastCommand = rxBuffer[0];
    rxLength = len;
    commandPending = true;
  }
}

// ============================================================
// CALLBACK I2C : ENVOI
// ============================================================

void onRequestI2C()
{
  uint8_t command = lastCommand;

  // 0x02 : états des 9 relais, un octet par relais.
  if (command == 0x02)
  {
    uint8_t states[RELAY_COUNT];

    for (uint8_t i = 0; i < RELAY_COUNT; i++)
    {
      states[i] = getRelay(i) ? 1 : 0;
    }

    Wire.write(states, sizeof(states));
  }

  // 0x04 : noms fixes de 15 octets par relais.
  else if (command == 0x04)
  {
    uint8_t buffer[RELAY_COUNT * NAME_SLOT_LEN] = {0};

    for (uint8_t i = 0; i < RELAY_COUNT; i++)
    {
      memcpy(
        &buffer[i * NAME_SLOT_LEN],
        relayNames[i],
        NAME_SLOT_LEN
      );
    }

    Wire.write(buffer, sizeof(buffer));
  }
}

// ============================================================
// TRAITEMENT DES COMMANDES I2C
// ============================================================

void processI2C()
{
  if (!commandPending)
    return;

  uint8_t buffer[RX_BUFFER_SIZE];
  uint8_t len = 0;

  noInterrupts();

  len = rxLength;

  if (len > RX_BUFFER_SIZE)
    len = RX_BUFFER_SIZE;

  for (uint8_t i = 0; i < len; i++)
  {
    buffer[i] = rxBuffer[i];
  }

  commandPending = false;
  rxLength = 0;

  interrupts();

  if (len == 0)
    return;

  uint8_t command = buffer[0];

  // ----------------------------------------------------------
  // 0x01 : SET RELAY
  // [0] commande
  // [1] index relais 0..8
  // [2] état : 0 = OFF, 1 = ON
  // ----------------------------------------------------------

  if (command == 0x01)
  {
    if (len < 3)
      return;

    uint8_t relay = buffer[1];
    bool state = (buffer[2] != 0);

    if (relay >= RELAY_COUNT)
      return;

    setRelay(relay, state);

    Serial0.print("Relay ");
    Serial0.print(relay + 1);
    Serial0.print(" -> ");
    Serial0.println(state ? "ON" : "OFF");
  }

  // ----------------------------------------------------------
  // 0x02 : GET STATES
  // La réponse est envoyée dans onRequestI2C().
  // ----------------------------------------------------------

  else if (command == 0x02)
  {
    Serial0.println("I2C: GET STATES (9 relays)");
  }

  // ----------------------------------------------------------
  // 0x03 : SET NAME
  // [0] commande
  // [1] index relais 0..8
  // [2] longueur du nom
  // [3...] caractères du nom
  // ----------------------------------------------------------

  else if (command == 0x03)
  {
    if (len < 3)
      return;

    uint8_t relay = buffer[1];
    uint8_t nameLength = buffer[2];

    if (relay >= RELAY_COUNT)
      return;

    if (nameLength > MAX_NAME_LEN)
      nameLength = MAX_NAME_LEN;

    uint8_t availableLength = len - 3;

    if (nameLength > availableLength)
      nameLength = availableLength;

    char name[NAME_SLOT_LEN] = {0};

    memcpy(name, &buffer[3], nameLength);
    name[nameLength] = '\0';

    saveName(relay, name);

    Serial0.print("Relay ");
    Serial0.print(relay + 1);
    Serial0.print(" name = ");
    Serial0.println(name);
  }

  // ----------------------------------------------------------
  // 0x04 : GET NAMES
  // La réponse est envoyée dans onRequestI2C().
  // ----------------------------------------------------------

  else if (command == 0x04)
  {
    Serial0.println("I2C: GET NAMES");
  }

  else
  {
    Serial0.print("I2C: Unknown command 0x");

    if (command < 0x10)
      Serial0.print('0');

    Serial0.println(command, HEX);
  }
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial0.begin(115200);
  delay(500);

  Serial0.println();
  Serial0.println("========================================");
  Serial0.println(" ESP32-S3 N16R8 - 9 RELAY CONTROLLER");
  Serial0.println("========================================");

  // Initialisation des sorties relais.
  Serial0.println("Initializing relays...");

  for (uint8_t i = 0; i < RELAY_COUNT; i++)
  {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH);
  }

  allRelaysOff();
  Serial0.println("All 9 relays OFF");

  // Chargement des noms sauvegardés.
  loadNames();

  Serial0.println("Relay names loaded:");

  for (uint8_t i = 0; i < RELAY_COUNT; i++)
  {
    Serial0.print("  Relay ");
    Serial0.print(i + 1);
    Serial0.print(": ");
    Serial0.println(relayNames[i]);
  }

  // Initialisation I2C en mode esclave.
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
  Serial0.println(I2C_ADDRESS, HEX);

  Serial0.print("I2C SDA     : GPIO");
  Serial0.println(I2C_SDA);

  Serial0.print("I2C SCL     : GPIO");
  Serial0.println(I2C_SCL);

  Serial0.print("I2C speed   : ");
  Serial0.print(I2C_SPEED);
  Serial0.println(" Hz");

  Serial0.println();
  Serial0.println("========================================");
  Serial0.println(" READY - 9 RELAYS");
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
