/******************************************************************
 *  ANTENNA CONTROL CENTER
 *  ESP32-ETH01 / LAN8720 
 *  Auteur :  F4BIT Stéphane
 *  Date   :  2026-09
 *  CopyLeft. Sous licence GNU General Public License v3.0
 *  ---------------------------------------------------------------
  I2C #1 : LCD S3 <-> ETH01
    ETH01 = esclave 0x12
    SDA = GPIO32
    SCL = GPIO33
    100 kHz

  I2C #2 : ETH01 <-> ESP32 Relais
    ETH01 = maître
    SDA = GPIO27
    SCL = GPIO14
    100 kHz

  Ethernet :
    LAN8720
    PHY Address = 1
    MDC  = GPIO23
    MDIO = GPIO18
    POWER = GPIO16
    CLOCK = GPIO0
    Hostname = Antenna_Relay_Ctrl

  WebServer :
    Port 80

  Protocole I2C ETH01 <-> LCD :

    0x01, relay, state
      -> commande relais

    0x02
      -> demande état des 8 relais

    0x03, relay, length, text...
      -> changement nom relais

    0x04
      -> demande des 8 noms

    0x05
      -> demande adresse IP Ethernet

  Compatible Arduino ESP32 Core 2.0.17
*/

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <ETH.h>
#include <WebServer.h>


// ============================================================
// CONFIGURATION
// ============================================================

#define LCD_I2C_ADDR  0x12
#define LCD_I2C_SDA   32
#define LCD_I2C_SCL   33

#define RELAY_I2C_ADDR 0x12
#define RELAY_I2C_SDA  27
#define RELAY_I2C_SCL  14

#define MAX_NAME_LEN 14

#define RELAY_COUNT 8

#define LCD_RX_FIFO_SIZE 16

#define RELAY_POLL_INTERVAL 500


// ============================================================
// ETHERNET LAN8720
// ============================================================

#define ETH_PHY_ADDR   1
#define ETH_PHY_POWER  16
#define ETH_PHY_MDC    23
#define ETH_PHY_MDIO   18
#define ETH_CLK_MODE   ETH_CLOCK_GPIO0_IN

const char* HOSTNAME = "Antenna_Relay_Ctrl";


// ============================================================
// OBJETS
// ============================================================

TwoWire I2C_LCD = TwoWire(0);
TwoWire I2C_RELAY = TwoWire(1);

WebServer server(80);

Preferences preferences;


// ============================================================
// RELAIS
// ============================================================

const uint8_t relayPins[RELAY_COUNT] = {
  4, 5, 17, 19, 21, 22, 25, 26
};

bool relayStates[RELAY_COUNT] = {
  false, false, false, false,
  false, false, false, false
};

char relayNames[RELAY_COUNT][MAX_NAME_LEN + 1];


// ============================================================
// ETHERNET
// ============================================================

volatile bool ethConnected = false;

char cachedIP[16] = "0.0.0.0";


// ============================================================
// I2C LCD RX FIFO
// ============================================================

struct LCDCommand {
  uint8_t type;
  uint8_t relay;
  uint8_t value;
  uint8_t length;
  char text[MAX_NAME_LEN + 1];
};

volatile LCDCommand lcdFifo[LCD_RX_FIFO_SIZE];

volatile uint8_t lcdFifoHead = 0;
volatile uint8_t lcdFifoTail = 0;


// ============================================================
// REPONSE I2C LCD
// ============================================================

volatile uint8_t lcdResponseType = 0;


// ============================================================
// COPIE STRUCTURE -> FIFO VOLATILE
// ============================================================

void copyCommandToFifo(
  volatile LCDCommand& destination,
  const LCDCommand& source
) {
  destination.type = source.type;
  destination.relay = source.relay;
  destination.value = source.value;
  destination.length = source.length;

  for (uint8_t i = 0; i <= MAX_NAME_LEN; i++) {
    destination.text[i] = source.text[i];
  }
}


// ============================================================
// COPIE FIFO VOLATILE -> STRUCTURE
// ============================================================

void copyCommandFromFifo(
  LCDCommand& destination,
  volatile LCDCommand& source
) {
  destination.type = source.type;
  destination.relay = source.relay;
  destination.value = source.value;
  destination.length = source.length;

  for (uint8_t i = 0; i <= MAX_NAME_LEN; i++) {
    destination.text[i] = source.text[i];
  }
}


// ============================================================
// ETHERNET READY
// ============================================================

bool ethernetReady() {

  IPAddress ip = ETH.localIP();

  if (ip == IPAddress(0, 0, 0, 0)) {
    return false;
  }

  return ETH.linkUp();
}


// ============================================================
// ETHERNET EVENTS
// ============================================================

void onEvent(WiFiEvent_t event) {

  switch (event) {

    case SYSTEM_EVENT_ETH_START:

      Serial.println("ETH START");

      ETH.setHostname(HOSTNAME);

      break;


    case SYSTEM_EVENT_ETH_CONNECTED:

      Serial.println("ETH CONNECTED");

      break;


    case SYSTEM_EVENT_ETH_GOT_IP:

      ethConnected = true;

      {
        String ip = ETH.localIP().toString();

        ip.toCharArray(
          cachedIP,
          sizeof(cachedIP)
        );
      }

      Serial.println();
      Serial.println("========== ETHERNET ==========");

      Serial.print("IP      : ");
      Serial.println(ETH.localIP());

      Serial.print("MASK    : ");
      Serial.println(ETH.subnetMask());

      Serial.print("GATEWAY : ");
      Serial.println(ETH.gatewayIP());

      Serial.print("LINK    : ");
      Serial.println(
        ETH.linkUp() ? "UP" : "DOWN"
      );

      Serial.println("==============================");
      Serial.println();

      break;


    case SYSTEM_EVENT_ETH_DISCONNECTED:

      Serial.println("ETH DISCONNECTED");

      ethConnected = false;

      strncpy(
        cachedIP,
        "0.0.0.0",
        sizeof(cachedIP)
      );

      cachedIP[sizeof(cachedIP) - 1] = '\0';

      break;


    case SYSTEM_EVENT_ETH_STOP:

      Serial.println("ETH STOP");

      ethConnected = false;

      strncpy(
        cachedIP,
        "0.0.0.0",
        sizeof(cachedIP)
      );

      cachedIP[sizeof(cachedIP) - 1] = '\0';

      break;


    default:

      break;
  }
}


// ============================================================
// RELAIS LOCAUX
// ============================================================

void applyRelay(uint8_t relay, bool state) {

  if (relay >= RELAY_COUNT) {
    return;
  }

  /*
    Relais actifs LOW.

    Lorsqu'un relais est activé,
    tous les autres sont désactivés.
  */

  if (state) {

    for (uint8_t i = 0; i < RELAY_COUNT; i++) {

      if (i == relay) {

        digitalWrite(
          relayPins[i],
          LOW
        );

        relayStates[i] = true;

      } else {

        digitalWrite(
          relayPins[i],
          HIGH
        );

        relayStates[i] = false;
      }
    }

  } else {

    digitalWrite(
      relayPins[relay],
      HIGH
    );

    relayStates[relay] = false;
  }
}


// ============================================================
// CHARGE LES NOMS
// ============================================================

void loadNames() {

  preferences.begin("relays", false);

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {

    char key[4];

    snprintf(
      key,
      sizeof(key),
      "n%u",
      i
    );

    String defaultName =
      "Relais " + String(i + 1);

    String name =
      preferences.getString(
        key,
        defaultName
      );

    name.toCharArray(
      relayNames[i],
      sizeof(relayNames[i])
    );

    relayNames[i][MAX_NAME_LEN] = '\0';
  }

  preferences.end();
}


// ============================================================
// SAUVEGARDE NOM
// ============================================================

void saveRelayName(
  uint8_t relay,
  const char* name
) {

  if (relay >= RELAY_COUNT) {
    return;
  }

  strncpy(
    relayNames[relay],
    name,
    MAX_NAME_LEN
  );

  relayNames[relay][MAX_NAME_LEN] = '\0';

  preferences.begin("relays", false);

  char key[4];

  snprintf(
    key,
    sizeof(key),
    "n%u",
    relay
  );

  preferences.putString(
    key,
    relayNames[relay]
  );

  preferences.end();
}


// ============================================================
// I2C LCD - RECEPTION
// ============================================================

void onLCDReceive(int count) {

  if (count <= 0) {
    return;
  }

  uint8_t type = I2C_LCD.read();

  // ----------------------------------------------------------
  // 0x01 = SET RELAY
  // ----------------------------------------------------------

  if (type == 0x01) {

    LCDCommand cmd;

    memset(
      &cmd,
      0,
      sizeof(cmd)
    );

    cmd.type = 0x01;

    if (I2C_LCD.available()) {
      cmd.relay = I2C_LCD.read();
    }

    if (I2C_LCD.available()) {
      cmd.value = I2C_LCD.read();
    }

    uint8_t next =
      (lcdFifoHead + 1) %
      LCD_RX_FIFO_SIZE;

    if (next != lcdFifoTail) {

      copyCommandToFifo(
        lcdFifo[lcdFifoHead],
        cmd
      );

      lcdFifoHead = next;
    }

    while (I2C_LCD.available()) {
      I2C_LCD.read();
    }

    return;
  }


  // ----------------------------------------------------------
  // 0x02 = REQUEST STATES
  // ----------------------------------------------------------

  if (type == 0x02) {

    lcdResponseType = 0x02;

    while (I2C_LCD.available()) {
      I2C_LCD.read();
    }

    return;
  }


  // ----------------------------------------------------------
  // 0x03 = SET NAME
  // ----------------------------------------------------------

  if (type == 0x03) {

    LCDCommand cmd;

    memset(
      &cmd,
      0,
      sizeof(cmd)
    );

    cmd.type = 0x03;

    if (I2C_LCD.available()) {
      cmd.relay = I2C_LCD.read();
    }

    if (I2C_LCD.available()) {
      cmd.length = I2C_LCD.read();
    }

    if (cmd.length > MAX_NAME_LEN) {
      cmd.length = MAX_NAME_LEN;
    }

    uint8_t index = 0;

    while (
      I2C_LCD.available() &&
      index < MAX_NAME_LEN
    ) {

      cmd.text[index++] =
        I2C_LCD.read();
    }

    cmd.text[index] = '\0';

    uint8_t next =
      (lcdFifoHead + 1) %
      LCD_RX_FIFO_SIZE;

    if (next != lcdFifoTail) {

      copyCommandToFifo(
        lcdFifo[lcdFifoHead],
        cmd
      );

      lcdFifoHead = next;
    }

    while (I2C_LCD.available()) {
      I2C_LCD.read();
    }

    return;
  }


  // ----------------------------------------------------------
  // 0x04 = REQUEST NAMES
  // ----------------------------------------------------------

  if (type == 0x04) {

    lcdResponseType = 0x04;

    while (I2C_LCD.available()) {
      I2C_LCD.read();
    }

    return;
  }


  // ----------------------------------------------------------
  // 0x05 = REQUEST IP
  // ----------------------------------------------------------

  if (type == 0x05) {

    lcdResponseType = 0x05;

    while (I2C_LCD.available()) {
      I2C_LCD.read();
    }

    return;
  }


  // ----------------------------------------------------------
  // VIDANGE
  // ----------------------------------------------------------

  while (I2C_LCD.available()) {
    I2C_LCD.read();
  }
}


// ============================================================
// I2C LCD - REPONSE
// ============================================================

void onLCDRequest() {

  uint8_t response =
    lcdResponseType;

  lcdResponseType = 0;


  // ----------------------------------------------------------
  // ETATS RELAIS
  // ----------------------------------------------------------

  if (response == 0x02) {

    uint8_t states[RELAY_COUNT];

    for (uint8_t i = 0; i < RELAY_COUNT; i++) {

      states[i] =
        relayStates[i] ? 1 : 0;
    }

    I2C_LCD.write(
      states,
      RELAY_COUNT
    );

    return;
  }


  // ----------------------------------------------------------
  // NOMS RELAIS
  // ----------------------------------------------------------

  if (response == 0x04) {

    /*
      8 noms x 15 octets
      = 120 octets

      MAX_NAME_LEN = 14
      + '\0'
    */

    uint8_t buffer[
      RELAY_COUNT *
      (MAX_NAME_LEN + 1)
    ];

    uint16_t index = 0;

    for (uint8_t i = 0; i < RELAY_COUNT; i++) {

      for (
        uint8_t j = 0;
        j <= MAX_NAME_LEN;
        j++
      ) {

        buffer[index++] =
          relayNames[i][j];
      }
    }

    I2C_LCD.write(
      buffer,
      sizeof(buffer)
    );

    return;
  }


  // ----------------------------------------------------------
  // ADRESSE IP
  // ----------------------------------------------------------

  if (response == 0x05) {

    /*
      Toujours exactement 16 octets.

      Exemple :
      "192.168.1.100"
      suivi de '\0' puis zéros.
    */

    I2C_LCD.write(
      (const uint8_t*)cachedIP,
      sizeof(cachedIP)
    );

    return;
  }
}


// ============================================================
// TRAITEMENT FIFO LCD
// ============================================================

void processLCDCommands() {

  while (lcdFifoTail != lcdFifoHead) {

    LCDCommand cmd;

    copyCommandFromFifo(
      cmd,
      lcdFifo[lcdFifoTail]
    );

    lcdFifoTail =
      (lcdFifoTail + 1) %
      LCD_RX_FIFO_SIZE;


    // --------------------------------------------------------
    // SET RELAY
    // --------------------------------------------------------

    if (cmd.type == 0x01) {

      if (cmd.relay < RELAY_COUNT) {

        applyRelay(
          cmd.relay,
          cmd.value != 0
        );

        Serial.print("LCD -> RELAIS ");
        Serial.print(cmd.relay);
        Serial.print(" = ");
        Serial.println(
          cmd.value ? "ON" : "OFF"
        );
      }
    }


    // --------------------------------------------------------
    // SET NAME
    // --------------------------------------------------------

    else if (cmd.type == 0x03) {

      if (cmd.relay < RELAY_COUNT) {

        saveRelayName(
          cmd.relay,
          cmd.text
        );

        Serial.print("LCD -> NOM ");
        Serial.print(cmd.relay);
        Serial.print(" = ");
        Serial.println(
          relayNames[cmd.relay]
        );
      }
    }
  }
}


// ============================================================
// RELAIS ESP32 DISTANT
// ============================================================

bool writeRelayCommand(
  uint8_t relay,
  bool state
) {

  if (relay >= RELAY_COUNT) {
    return false;
  }

  I2C_RELAY.beginTransmission(
    RELAY_I2C_ADDR
  );

  I2C_RELAY.write(0x01);
  I2C_RELAY.write(relay);
  I2C_RELAY.write(state ? 1 : 0);

  uint8_t error =
    I2C_RELAY.endTransmission();

  return error == 0;
}


// ============================================================
// LECTURE ETATS RELAIS
// ============================================================

bool readRelayStates() {

  I2C_RELAY.beginTransmission(
    RELAY_I2C_ADDR
  );

  I2C_RELAY.write(0x02);

  uint8_t error =
    I2C_RELAY.endTransmission(
      false
    );

  if (error != 0) {
    return false;
  }

  uint8_t received =
    I2C_RELAY.requestFrom(
      RELAY_I2C_ADDR,
      (uint8_t)RELAY_COUNT
    );

  if (received != RELAY_COUNT) {

    while (I2C_RELAY.available()) {
      I2C_RELAY.read();
    }

    return false;
  }

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {

    relayStates[i] =
      I2C_RELAY.read() != 0;
  }

  return true;
}


// ============================================================
// ENVOI NOM AU MODULE RELAIS
// ============================================================

bool sendRelayName(
  uint8_t relay,
  const char* name
) {

  if (relay >= RELAY_COUNT) {
    return false;
  }

  uint8_t length =
    strlen(name);

  if (length > MAX_NAME_LEN) {
    length = MAX_NAME_LEN;
  }

  I2C_RELAY.beginTransmission(
    RELAY_I2C_ADDR
  );

  I2C_RELAY.write(0x03);
  I2C_RELAY.write(relay);
  I2C_RELAY.write(length);

  for (uint8_t i = 0; i < length; i++) {
    I2C_RELAY.write(name[i]);
  }

  uint8_t error =
    I2C_RELAY.endTransmission();

  return error == 0;
}


// ============================================================
// POLLING RELAIS
// ============================================================

unsigned long lastRelayPoll = 0;

bool relayOnline = false;

void pollRelay() {

  if (
    millis() - lastRelayPoll <
    RELAY_POLL_INTERVAL
  ) {
    return;
  }

  lastRelayPoll = millis();

  bool ok =
    readRelayStates();

  relayOnline = ok;
}


// ============================================================
// WEB HTML
// ============================================================

const char html[] PROGMEM = R"ANTENNA_HTML(
<!DOCTYPE html>
<html lang="fr">

<head>

<meta charset="UTF-8">

<meta
  name="viewport"
  content="width=device-width,initial-scale=1"
>

<title>Antenna Control Center</title>

<style>

:root{
  --bg:#0b1120;
  --panel:#111827;
  --border:rgba(255,255,255,.09);
  --text:#f8fafc;
  --muted:#94a3b8;
  --accent2:#22c55e;
  --danger:#f43f5e;
  --shadow:0 20px 50px rgba(0,0,0,.28);
}

*{
  box-sizing:border-box;
}

body{
  margin:0;
  min-height:100vh;
  font-family:
    system-ui,
    -apple-system,
    BlinkMacSystemFont,
    "Segoe UI",
    sans-serif;

  color:var(--text);

  background:
    radial-gradient(
      circle at top left,
      rgba(56,189,248,.14),
      transparent 32%
    ),
    radial-gradient(
      circle at top right,
      rgba(34,197,94,.10),
      transparent 32%
    ),
    var(--bg);
}

.container{
  width:100%;
  max-width:1200px;
  margin:auto;
  padding:28px 20px 20px;
}

header{
  text-align:center;
  margin-bottom:28px;
}

/* =========================================================
   LOGO ANTENNE - SVG INTEGRE
   Aucun fichier image externe n'est necessaire.
   ========================================================= */

.header-brand{
  display:flex;
  align-items:center;
  justify-content:center;
  gap:16px;
}

.antenna-logo{
  width:72px;
  height:72px;
  flex:0 0 72px;
  color:var(--text);
  filter:drop-shadow(0 0 8px rgba(34,197,94,.18));
  transition:transform .2s ease, filter .2s ease;
}

.header-brand:hover .antenna-logo{
  transform:scale(1.05);
  filter:drop-shadow(0 0 12px rgba(34,197,94,.35));
}

.header-title{
  text-align:left;
}

.header-title h1{
  margin:0;
}

.header-subtitle{
  margin:5px 0 0;
  color:var(--muted);
  font-size:14px;
  font-weight:500;
}


h1{
  margin:0;
  font-size:clamp(25px,4vw,38px);
  font-weight:800;
  letter-spacing:.5px;
}

.grid{
  display:grid;
  grid-template-columns:
    repeat(4,minmax(0,1fr));
  gap:18px;
}

.card{
  min-height:190px;
  border-radius:20px;

  border:2px solid var(--danger);

  background:
    linear-gradient(
      145deg,
      #151c2b,
      #0e1523
    );

  box-shadow:var(--shadow);

  display:flex;
  align-items:center;
  justify-content:center;

  cursor:pointer;

  transition:
    transform .15s ease,
    border-color .15s ease,
    background .15s ease,
    box-shadow .15s ease;

  user-select:none;

  outline:none;
}

.card:hover{
  transform:translateY(-3px);
}

.card:focus-visible{
  box-shadow:
    0 0 0 3px rgba(56,189,248,.35),
    var(--shadow);
}

.card.active{
  border-color:var(--accent2);

  background:
    linear-gradient(
      145deg,
      rgba(34,197,94,.20),
      rgba(20,83,45,.35)
    );
}

.name{
  width:100%;
  min-height:100%;

  padding:20px;

  display:flex;
  align-items:center;
  justify-content:center;

  text-align:center;

  font-size:20px;
  font-weight:750;
  line-height:1.25;

  overflow-wrap:anywhere;
}

footer{
  margin-top:30px;

  text-align:center;

  color:var(--muted);

  font-size:13px;
}

@media(max-width:1050px){

  .grid{
    grid-template-columns:
      repeat(3,minmax(0,1fr));
  }

}

@media(max-width:800px){

  .grid{
    grid-template-columns:
      repeat(2,minmax(0,1fr));
  }

  .card{
    min-height:175px;
    border-radius:18px;
  }

}

@media(max-width:560px){

  .container{
    padding:20px 14px 16px;
  }

  .grid{
    grid-template-columns:1fr;
    gap:14px;
  }

  .card{
    min-height:175px;
  }

}

@media(
  max-width:800px
) and (
  orientation:landscape
){

  .grid{
    grid-template-columns:
      repeat(2,minmax(0,1fr));
  }

  .card{
    min-height:165px;
  }

}

</style>

</head>

<body>

<div class="container">

<header>

<div class="header-brand">

<svg
  class="antenna-logo"
  viewBox="0 0 120 120"
  xmlns="http://www.w3.org/2000/svg"
  role="img"
  aria-label="Antenne radio"
>

  <!-- Ondes radio gauche -->
  <path
    d="M28 25 C7 44 7 76 28 95"
    fill="none"
    stroke="currentColor"
    stroke-width="7"
    stroke-linecap="round"
  />

  <path
    d="M43 39 C31 50 31 70 43 81"
    fill="none"
    stroke="currentColor"
    stroke-width="7"
    stroke-linecap="round"
  />

  <!-- Ondes radio droite -->
  <path
    d="M92 25 C113 44 113 76 92 95"
    fill="none"
    stroke="currentColor"
    stroke-width="7"
    stroke-linecap="round"
  />

  <path
    d="M77 39 C89 50 89 70 77 81"
    fill="none"
    stroke="currentColor"
    stroke-width="7"
    stroke-linecap="round"
  />

  <!-- Point d'emission -->
  <circle
    cx="60"
    cy="48"
    r="8"
    fill="currentColor"
  />

  <!-- Mât / antenne -->
  <path
    d="M60 57 L42 105 M60 57 L78 105"
    fill="none"
    stroke="currentColor"
    stroke-width="8"
    stroke-linecap="round"
    stroke-linejoin="round"
  />

  <!-- Barre centrale -->
  <path
    d="M49 82 H71"
    fill="none"
    stroke="currentColor"
    stroke-width="7"
    stroke-linecap="round"
  />

</svg>

<div class="header-title">
  <h1>Antenna Control Center</h1>
</div>

</div>

</header>

<main
  id="grid"
  class="grid"
></main>

<footer>
2026 - F4BIT@CopyLeft.
Sous licence GNU General Public License v3.0
</footer>

</div>

<script>

function render(data){

  const grid =
    document.getElementById("grid");

  grid.innerHTML = "";

  const names =
    Object.keys(data);

  names.forEach(
    function(name,index){

      const active =
        !!data[name];

      const card =
        document.createElement("div");

      card.className =
        "card" +
        (active ? " active" : "");

      card.setAttribute(
        "role",
        "button"
      );

      card.setAttribute(
        "tabindex",
        "0"
      );

      const title =
        document.createElement("div");

      title.className =
        "name";

      title.textContent =
        name;

      card.appendChild(title);

      card.addEventListener(
        "click",
        function(){

          toggle(index);

        }
      );

      card.addEventListener(
        "keydown",
        function(event){

          if(
            event.key === "Enter" ||
            event.key === " "
          ){

            event.preventDefault();

            toggle(index);
          }

        }
      );

      grid.appendChild(card);
    }
  );
}


function toggle(idx){

  fetch(
    "/api/set?relay=" +
    idx +
    "&on=1"
  )
  .then(
    function(response){

      if(!response.ok){

        throw new Error(
          "HTTP " + response.status
        );
      }

      return response.text();
    }
  )
  .then(
    function(){

      refresh();
    }
  )
  .catch(
    function(error){

      console.error(error);

      refresh();
    }
  );
}


function refresh(){

  fetch(
    "/api/state",
    {
      cache:"no-store"
    }
  )
  .then(
    function(response){

      if(!response.ok){

        throw new Error(
          "HTTP " + response.status
        );
      }

      return response.json();
    }
  )
  .then(
    function(data){

      render(data);
    }
  )
  .catch(
    function(error){

      console.error(error);
    }
  );
}


refresh();

setInterval(
  refresh,
  5000
);

</script>

</body>

</html>
)ANTENNA_HTML";


// ============================================================
// WEB : PAGE PRINCIPALE
// ============================================================

void handleRoot() {

  if (!ethernetReady()) {

    server.send(
      503,
      "text/plain",
      "Ethernet non disponible"
    );

    return;
  }

  server.send_P(
    200,
    "text/html",
    html
  );
}


// ============================================================
// WEB : ETAT
// ============================================================

void handleState() {

  if (!ethernetReady()) {

    server.send(
      503,
      "application/json",
      "{\"error\":\"Ethernet non disponible\"}"
    );

    return;
  }


  String json = "{";

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {

    if (i > 0) {
      json += ",";
    }

    json += "\"";

    json += relayNames[i];

    json += "\":";

    json +=
      relayStates[i]
      ? "true"
      : "false";
  }

  json += "}";

  server.send(
    200,
    "application/json",
    json
  );
}


// ============================================================
// WEB : SET RELAIS
// ============================================================

void handleSet() {

  if (!ethernetReady()) {

    server.send(
      503,
      "text/plain",
      "Ethernet non disponible"
    );

    return;
  }


  if (
    !server.hasArg("relay") ||
    !server.hasArg("on")
  ) {

    server.send(
      400,
      "text/plain",
      "Parametres manquants"
    );

    return;
  }


  int relay =
    server.arg("relay").toInt();

  int on =
    server.arg("on").toInt();


  if (
    relay < 0 ||
    relay >= RELAY_COUNT
  ) {

    server.send(
      400,
      "text/plain",
      "Relais invalide"
    );

    return;
  }


  bool state =
    on != 0;


  bool ok =
    writeRelayCommand(
      relay,
      state
    );


  if (!ok) {

    relayOnline = false;

    server.send(
      500,
      "text/plain",
      "Erreur I2C relais"
    );

    return;
  }


  /*
    Mise à jour optimiste du cache.

    Le relais distant applique aussi
    l'exclusivité : un seul relais ON.
  */

  if (state) {

    for (
      uint8_t i = 0;
      i < RELAY_COUNT;
      i++
    ) {

      relayStates[i] =
        (i == relay);
    }

  } else {

    relayStates[relay] = false;
  }


  relayOnline = true;


  server.send(
    200,
    "text/plain",
    "OK"
  );
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(500);

  Serial.println();
  Serial.println();
  Serial.println(
    "========================================"
  );
  Serial.println(
    " ANTENNA CONTROL CENTER - ETH01"
  );
  Serial.println(
    "========================================"
  );


  // ----------------------------------------------------------
  // RELAIS LOCAUX / CACHE
  // ----------------------------------------------------------

  for (
    uint8_t i = 0;
    i < RELAY_COUNT;
    i++
  ) {

    relayStates[i] = false;
  }


  // ----------------------------------------------------------
  // NOMS
  // ----------------------------------------------------------

  loadNames();


  // ----------------------------------------------------------
  // I2C RELAIS
  // ----------------------------------------------------------

  I2C_RELAY.begin(
    RELAY_I2C_SDA,
    RELAY_I2C_SCL,
    100000
  );

  Serial.println(
    "I2C RELAY : OK"
  );


  // ----------------------------------------------------------
  // I2C LCD ESCLAVE
  // ----------------------------------------------------------

  I2C_LCD.begin(
    LCD_I2C_ADDR,
    LCD_I2C_SDA,
    LCD_I2C_SCL,
    100000
  );

  I2C_LCD.onReceive(
    onLCDReceive
  );

  I2C_LCD.onRequest(
    onLCDRequest
  );

  Serial.println(
    "I2C LCD   : OK"
  );


  // ----------------------------------------------------------
  // ETHERNET
  // ----------------------------------------------------------

  WiFi.onEvent(
    onEvent
  );

  ETH.begin(
    ETH_PHY_ADDR,
    ETH_PHY_POWER,
    ETH_PHY_MDC,
    ETH_PHY_MDIO,
    ETH_PHY_LAN8720,
    ETH_CLK_MODE
  );


  // ----------------------------------------------------------
  // ATTENTE ETHERNET
  // ----------------------------------------------------------

  Serial.println(
    "Attente Ethernet..."
  );

  unsigned long startWait =
    millis();

  while (
    !ethernetReady() &&
    millis() - startWait < 10000
  ) {

    delay(100);
  }


  if (ethernetReady()) {

    Serial.println(
      "Ethernet pret."
    );

    Serial.print(
      "Adresse IP : "
    );

    Serial.println(
      ETH.localIP()
    );

  } else {

    Serial.println(
      "Ethernet non disponible."
    );
  }


  // ----------------------------------------------------------
  // WEB SERVER
  // ----------------------------------------------------------

  server.on(
    "/",
    HTTP_GET,
    handleRoot
  );

  server.on(
    "/api/state",
    HTTP_GET,
    handleState
  );

  server.on(
    "/api/set",
    HTTP_GET,
    handleSet
  );

  server.begin();


  Serial.println(
    "WebServer : port 80"
  );


  // ----------------------------------------------------------
  // PREMIERE LECTURE RELAIS
  // ----------------------------------------------------------

  if (readRelayStates()) {

    relayOnline = true;

    Serial.println(
      "Relais : ONLINE"
    );

  } else {

    relayOnline = false;

    Serial.println(
      "Relais : OFFLINE"
    );
  }


  Serial.println(
    "========================================"
  );

  Serial.println(
    "SYSTEME PRET"
  );

  Serial.println(
    "========================================"
  );
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  // ----------------------------------------------------------
  // COMMANDES LCD
  // ----------------------------------------------------------

  processLCDCommands();


  // ----------------------------------------------------------
  // WEB SERVER
  // ----------------------------------------------------------

  server.handleClient();


  // ----------------------------------------------------------
  // POLLING RELAIS
  // ----------------------------------------------------------

  pollRelay();


  // ----------------------------------------------------------
  // MAINTIEN DE L'IP CACHEE
  // ----------------------------------------------------------

  /*
    Si l'Ethernet est opérationnel mais que
    l'adresse IP a changé, on actualise le cache.

    Cette opération reste dans loop(),
    jamais dans le callback I2C.
  */

  static unsigned long lastIPCheck = 0;

  if (
    millis() - lastIPCheck >= 1000
  ) {

    lastIPCheck = millis();

    if (ethernetReady()) {

      IPAddress ip =
        ETH.localIP();

      String ipString =
        ip.toString();

      if (
        strncmp(
          cachedIP,
          ipString.c_str(),
          sizeof(cachedIP)
        ) != 0
      ) {

        ipString.toCharArray(
          cachedIP,
          sizeof(cachedIP)
        );
      }

    } else {

      if (
        strcmp(
          cachedIP,
          "0.0.0.0"
        ) != 0
      ) {

        strncpy(
          cachedIP,
          "0.0.0.0",
          sizeof(cachedIP)
        );

        cachedIP[
          sizeof(cachedIP) - 1
        ] = '\0';
      }
    }
  }


  delay(1);
}
