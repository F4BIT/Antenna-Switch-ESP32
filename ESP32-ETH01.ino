/******************************************************************
 * ANTENNA CONTROL CENTER
 * ESP32-ETH01 / LAN8720
 *
 * Auteur : F4BIT Stéphane
 * Date   : 2026-09
 * Licence: GNU General Public License v3.0
 *
 * Arduino ESP32 Core 2.0.17
 *
 * I2C #1 : LCD S3 <-> ETH01
 * ETH01 esclave 0x12 - SDA 32 - SCL 33
 *
 * I2C #2 : ETH01 <-> ESP32 RELAIS
 * ETH01 maitre - SDA 27 - SCL 14
 *
 * 9 relais commandes par I2C uniquement.
 ******************************************************************/
 
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <ETH.h>
#include <WiFi.h>
#include <WebServer.h>

// ============================================================
// CONFIGURATION
// ============================================================

#define LCD_I2C_ADDR        0x12
#define LCD_I2C_SDA         32
#define LCD_I2C_SCL         33

#define RELAY_I2C_ADDR      0x12
#define RELAY_I2C_SDA       27
#define RELAY_I2C_SCL       14

#define RELAY_COUNT         9
#define MAX_NAME_LEN        14
#define LCD_RX_FIFO_SIZE    16
#define RELAY_POLL_INTERVAL 500

#define ETH_PHY_ADDR        1
#define ETH_PHY_POWER       16
#define ETH_PHY_MDC         23
#define ETH_PHY_MDIO        18
#define ETH_CLK_MODE        ETH_CLOCK_GPIO0_IN

const char* HOSTNAME = "Antenna_Relay_Ctrl";

TwoWire I2C_LCD(0);
TwoWire I2C_RELAY(1);

WebServer server(80);
Preferences preferences;

// ============================================================
// ETATS ET NOMS
// ============================================================

bool relayStates[RELAY_COUNT] = {false};
char relayNames[RELAY_COUNT][MAX_NAME_LEN + 1];

bool relayOnline = false;
unsigned long lastRelayPoll = 0;

volatile bool ethConnected = false;
char cachedIP[16] = "0.0.0.0";

// ============================================================
// FIFO LCD
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
volatile uint8_t lcdResponseType = 0;

portMUX_TYPE lcdMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================
// COPIE FIFO
// ============================================================

void copyCommandToFifo(
  volatile LCDCommand& dst,
  const LCDCommand& src
) {
  dst.type = src.type;
  dst.relay = src.relay;
  dst.value = src.value;
  dst.length = src.length;

  for (uint8_t i = 0; i <= MAX_NAME_LEN; i++) {
    dst.text[i] = src.text[i];
  }
}

void copyCommandFromFifo(
  LCDCommand& dst,
  volatile LCDCommand& src
) {
  dst.type = src.type;
  dst.relay = src.relay;
  dst.value = src.value;
  dst.length = src.length;

  for (uint8_t i = 0; i <= MAX_NAME_LEN; i++) {
    dst.text[i] = src.text[i];
  }
}

// ============================================================
// ETHERNET
// ============================================================

bool ethernetReady() {
  return ETH.linkUp() &&
         ETH.localIP() != IPAddress(0, 0, 0, 0);
}

void onEvent(WiFiEvent_t event) {
  switch (event) {
    case SYSTEM_EVENT_ETH_START:
      Serial.println("ETH START");
      ETH.setHostname(HOSTNAME);
      break;

    case SYSTEM_EVENT_ETH_CONNECTED:
      Serial.println("ETH CONNECTED");
      break;

    case SYSTEM_EVENT_ETH_GOT_IP: {
      ethConnected = true;

      String ip = ETH.localIP().toString();
      ip.toCharArray(cachedIP, sizeof(cachedIP));

      Serial.println();
      Serial.println("========== ETHERNET ==========");
      Serial.print("Adresse Web : http://");
      Serial.println(ETH.localIP());
      Serial.print("Masque      : ");
      Serial.println(ETH.subnetMask());
      Serial.print("Passerelle  : ");
      Serial.println(ETH.gatewayIP());
      Serial.println("==============================");
      Serial.println();
      break;
    }

    case SYSTEM_EVENT_ETH_DISCONNECTED:
      Serial.println("ETH DISCONNECTED");
      ethConnected = false;
      strncpy(cachedIP, "0.0.0.0", sizeof(cachedIP));
      cachedIP[sizeof(cachedIP) - 1] = '\0';
      break;

    case SYSTEM_EVENT_ETH_STOP:
      Serial.println("ETH STOP");
      ethConnected = false;
      strncpy(cachedIP, "0.0.0.0", sizeof(cachedIP));
      cachedIP[sizeof(cachedIP) - 1] = '\0';
      break;

    default:
      break;
  }
}

// ============================================================
// NOMS DES RELAIS : PREFERENCES
// ============================================================

void loadNames() {
  preferences.begin("relays", false);

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    char key[4];
    snprintf(key, sizeof(key), "n%u", i);

    String defaultName = "Relais " + String(i + 1);
    String name = preferences.getString(key, defaultName);

    name.toCharArray(relayNames[i], sizeof(relayNames[i]));
    relayNames[i][MAX_NAME_LEN] = '\0';
  }

  preferences.end();
}

void saveRelayName(uint8_t relay, const char* name) {
  if (relay >= RELAY_COUNT || name == nullptr) {
    return;
  }

  strncpy(relayNames[relay], name, MAX_NAME_LEN);
  relayNames[relay][MAX_NAME_LEN] = '\0';

  preferences.begin("relays", false);

  char key[4];
  snprintf(key, sizeof(key), "n%u", relay);
  preferences.putString(key, relayNames[relay]);

  preferences.end();
}

// ============================================================
// I2C RELAIS : COMMANDE
// Protocole : 0x01, relais, etat
// ============================================================

bool writeRelayCommand(uint8_t relay, bool state) {
  if (relay >= RELAY_COUNT) {
    return false;
  }

  I2C_RELAY.beginTransmission((uint8_t)RELAY_I2C_ADDR);
  I2C_RELAY.write((uint8_t)0x01);
  I2C_RELAY.write(relay);
  I2C_RELAY.write(state ? (uint8_t)1 : (uint8_t)0);

  return I2C_RELAY.endTransmission() == 0;
}

// ============================================================
// I2C RELAIS : LECTURE DES 9 ETATS
// Protocole : envoyer 0x02, lire 9 octets
// ============================================================

bool readRelayStates() {
  I2C_RELAY.beginTransmission((uint8_t)RELAY_I2C_ADDR);
  I2C_RELAY.write((uint8_t)0x02);

  if (I2C_RELAY.endTransmission(false) != 0) {
    return false;
  }

  uint8_t received = I2C_RELAY.requestFrom(
    (uint8_t)RELAY_I2C_ADDR,
    (uint8_t)RELAY_COUNT
  );

  if (received != RELAY_COUNT) {
    while (I2C_RELAY.available()) {
      I2C_RELAY.read();
    }
    return false;
  }

  bool newStates[RELAY_COUNT];

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (!I2C_RELAY.available()) {
      return false;
    }
    newStates[i] = I2C_RELAY.read() != 0;
  }

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    relayStates[i] = newStates[i];
  }

  return true;
}

// ============================================================
// I2C RELAIS : RENOMMAGE
// Protocole : 0x03, relais, longueur, texte
// ============================================================

bool sendRelayName(uint8_t relay, const char* name) {
  if (relay >= RELAY_COUNT || name == nullptr) {
    return false;
  }

  uint8_t length = strlen(name);
  if (length > MAX_NAME_LEN) {
    length = MAX_NAME_LEN;
  }

  I2C_RELAY.beginTransmission((uint8_t)RELAY_I2C_ADDR);
  I2C_RELAY.write((uint8_t)0x03);
  I2C_RELAY.write(relay);
  I2C_RELAY.write(length);

  for (uint8_t i = 0; i < length; i++) {
    I2C_RELAY.write((uint8_t)name[i]);
  }

  return I2C_RELAY.endTransmission() == 0;
}

// ============================================================
// I2C LCD : RECEPTION DES COMMANDES
//
// 0x01, relais, etat
// 0x02 : demander les états
// 0x03, relais, longueur, texte
// 0x04 : demander les noms
// 0x05 : demander l'adresse IP
// ============================================================

void onLCDReceive(int count) {
  if (count <= 0 || !I2C_LCD.available()) {
    return;
  }

  uint8_t type = I2C_LCD.read();

  if (type == 0x01) {
    LCDCommand cmd = {};
    cmd.type = 0x01;

    if (I2C_LCD.available()) {
      cmd.relay = I2C_LCD.read();
    }

    if (I2C_LCD.available()) {
      cmd.value = I2C_LCD.read();
    }

    portENTER_CRITICAL(&lcdMux);

    uint8_t next = (lcdFifoHead + 1) % LCD_RX_FIFO_SIZE;

    if (next != lcdFifoTail) {
      copyCommandToFifo(lcdFifo[lcdFifoHead], cmd);
      lcdFifoHead = next;
    }

    portEXIT_CRITICAL(&lcdMux);
  }
  else if (type == 0x02 || type == 0x04 || type == 0x05) {
    portENTER_CRITICAL(&lcdMux);
    lcdResponseType = type;
    portEXIT_CRITICAL(&lcdMux);
  }
  else if (type == 0x03) {
    LCDCommand cmd = {};
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

    while (I2C_LCD.available() && index < cmd.length) {
      cmd.text[index++] = (char)I2C_LCD.read();
    }

    cmd.text[index] = '\0';

    portENTER_CRITICAL(&lcdMux);

    uint8_t next = (lcdFifoHead + 1) % LCD_RX_FIFO_SIZE;

    if (next != lcdFifoTail) {
      copyCommandToFifo(lcdFifo[lcdFifoHead], cmd);
      lcdFifoHead = next;
    }

    portEXIT_CRITICAL(&lcdMux);
  }

  while (I2C_LCD.available()) {
    I2C_LCD.read();
  }
}

// ============================================================
// I2C LCD : REPONSE AUX REQUETES
// ============================================================

void onLCDRequest() {
  uint8_t response;

  portENTER_CRITICAL(&lcdMux);
  response = lcdResponseType;
  lcdResponseType = 0;
  portEXIT_CRITICAL(&lcdMux);

  if (response == 0x02) {
    uint8_t states[RELAY_COUNT];

    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
      states[i] = relayStates[i] ? 1 : 0;
    }

    I2C_LCD.write(states, RELAY_COUNT);
  }
  else if (response == 0x04) {
    // 9 noms x 15 octets = 135 octets.
    // Selon le tampon Wire utilisé, cette réponse peut être tronquée.
    uint8_t buffer[RELAY_COUNT * (MAX_NAME_LEN + 1)] = {};
    uint16_t index = 0;

    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
      for (uint8_t j = 0; j <= MAX_NAME_LEN; j++) {
        buffer[index++] = relayNames[i][j];
      }
    }

    I2C_LCD.write(buffer, sizeof(buffer));
  }
  else if (response == 0x05) {
    I2C_LCD.write((const uint8_t*)cachedIP, sizeof(cachedIP));
  }
}

// ============================================================
// TRAITEMENT DES COMMANDES LCD
// ============================================================

void processLCDCommands() {
  while (true) {
    LCDCommand cmd;

    portENTER_CRITICAL(&lcdMux);

    if (lcdFifoTail == lcdFifoHead) {
      portEXIT_CRITICAL(&lcdMux);
      break;
    }

    copyCommandFromFifo(cmd, lcdFifo[lcdFifoTail]);
    lcdFifoTail = (lcdFifoTail + 1) % LCD_RX_FIFO_SIZE;

    portEXIT_CRITICAL(&lcdMux);

    if (cmd.type == 0x01 && cmd.relay < RELAY_COUNT) {
      bool ok = writeRelayCommand(cmd.relay, cmd.value != 0);

      if (ok && readRelayStates()) {
        relayOnline = true;
      } else {
        relayOnline = false;
        Serial.println("ERREUR I2C CARTE RELAIS");
      }
    }
    else if (cmd.type == 0x03 && cmd.relay < RELAY_COUNT) {
      saveRelayName(cmd.relay, cmd.text);

      bool ok = sendRelayName(cmd.relay, relayNames[cmd.relay]);

      Serial.printf(
        "LCD -> NOM %u = %s\n",
        cmd.relay + 1,
        relayNames[cmd.relay]
      );

      relayOnline = ok;
    }
  }
}

// ============================================================
// POLLING PERIODIQUE
// ============================================================

void pollRelay() {
  if (millis() - lastRelayPoll < RELAY_POLL_INTERVAL) {
    return;
  }

  lastRelayPoll = millis();
  relayOnline = readRelayStates();
}

// ============================================================
// PAGE HTML
// Design Antenna Control Center
// 3 relais maximum par ligne sur grand écran
// ============================================================

const char html[] PROGMEM = R"ANTENNA_HTML(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
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
  font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
  color:var(--text);
  background:
    radial-gradient(circle at top left,rgba(56,189,248,.14),transparent 32%),
    radial-gradient(circle at top right,rgba(34,197,94,.10),transparent 32%),
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
  transition:transform .2s ease,filter .2s ease;
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

h1{
  margin:0;
  font-size:clamp(25px,4vw,38px);
  font-weight:800;
  letter-spacing:.5px;
}

/* MAXIMUM 3 RELAIS PAR LIGNE */
.grid{
  display:grid;
  grid-template-columns:repeat(3,minmax(0,1fr));
  gap:18px;
}

.card{
  min-height:190px;
  border-radius:20px;
  border:2px solid var(--danger);
  background:linear-gradient(145deg,#151c2b,#0e1523);
  box-shadow:var(--shadow);
  display:flex;
  align-items:center;
  justify-content:center;
  cursor:pointer;
  transition:transform .15s ease,border-color .15s ease,
    background .15s ease,box-shadow .15s ease;
  user-select:none;
  outline:none;
}

.card:hover{
  transform:translateY(-3px);
}

.card:focus-visible{
  box-shadow:0 0 0 3px rgba(56,189,248,.35),var(--shadow);
}

.card.active{
  border-color:var(--accent2);
  background:linear-gradient(145deg,rgba(34,197,94,.20),rgba(20,83,45,.35));
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

/* TABLETTE : 2 RELAIS PAR LIGNE */
@media(max-width:800px){
  .grid{
    grid-template-columns:repeat(2,minmax(0,1fr));
  }

  .card{
    min-height:175px;
    border-radius:18px;
  }
}

/* TELEPHONE : 1 RELAIS PAR LIGNE */
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

/* TELEPHONE EN PAYSAGE */
@media(max-width:800px) and (orientation:landscape){
  .grid{
    grid-template-columns:repeat(2,minmax(0,1fr));
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
    <svg class="antenna-logo" viewBox="0 0 120 120"
      xmlns="http://www.w3.org/2000/svg"
      role="img" aria-label="Antenne radio">

      <path d="M28 25 C7 44 7 76 28 95"
        fill="none" stroke="currentColor"
        stroke-width="7" stroke-linecap="round"/>

      <path d="M43 39 C31 50 31 70 43 81"
        fill="none" stroke="currentColor"
        stroke-width="7" stroke-linecap="round"/>

      <path d="M92 25 C113 44 113 76 92 95"
        fill="none" stroke="currentColor"
        stroke-width="7" stroke-linecap="round"/>

      <path d="M77 39 C89 50 89 70 77 81"
        fill="none" stroke="currentColor"
        stroke-width="7" stroke-linecap="round"/>

      <circle cx="60" cy="48" r="8" fill="currentColor"/>

      <path d="M60 57 L42 105 M60 57 L78 105"
        fill="none" stroke="currentColor"
        stroke-width="8" stroke-linecap="round"
        stroke-linejoin="round"/>

      <path d="M49 82 H71"
        fill="none" stroke="currentColor"
        stroke-width="7" stroke-linecap="round"/>
    </svg>

    <div class="header-title">
      <h1>Antenna Control Center</h1>
    </div>
  </div>
</header>

<main id="grid" class="grid"></main>

<footer>
  2026 - F4BIT@CopyLeft.<br>
  Sous licence GNU General Public License v3.0
</footer>

</div>

<script>
function render(data){
  const grid=document.getElementById("grid");
  grid.innerHTML="";

  const names=Object.keys(data);

  names.forEach(function(name,index){
    const active=!!data[name];

    const card=document.createElement("div");
    card.className="card"+(active?" active":"");
    card.setAttribute("role","button");
    card.setAttribute("tabindex","0");

    const title=document.createElement("div");
    title.className="name";
    title.textContent=name;

    card.appendChild(title);

    card.addEventListener("click",function(){
      toggle(index,active);
    });

    card.addEventListener("keydown",function(event){
      if(event.key==="Enter"||event.key===" "){
        event.preventDefault();
        toggle(index,active);
      }
    });

    grid.appendChild(card);
  });
}

function toggle(idx,currentState){
  const newState=currentState?0:1;

  fetch("/api/set?relay="+idx+"&on="+newState)
    .then(function(response){
      if(!response.ok){
        throw new Error("HTTP "+response.status);
      }
      return response.text();
    })
    .then(function(){
      refresh();
    })
    .catch(function(error){
      console.error(error);
      refresh();
    });
}

function refresh(){
  fetch("/api/state",{cache:"no-store"})
    .then(function(response){
      if(!response.ok){
        throw new Error("HTTP "+response.status);
      }
      return response.json();
    })
    .then(function(data){
      render(data);
    })
    .catch(function(error){
      console.error(error);
    });
}

refresh();
setInterval(refresh,5000);
</script>

</body>
</html>
)ANTENNA_HTML";

// ============================================================
// WEB : PAGE PRINCIPALE
// ============================================================

void handleRoot() {
  if (!ethernetReady()) {
    server.send(503, "text/plain", "Ethernet non disponible");
    return;
  }

  server.send_P(200, "text/html; charset=utf-8", html);
}

// ============================================================
// WEB : ETAT DES RELAIS
// JSON attendu par le JavaScript :
// {"Relais 1":false,"Relais 2":true,...}
// ============================================================

String jsonEscape(const char* input) {
  String output;
  output.reserve(32);

  while (*input) {
    char c = *input++;

    if (c == '"' || c == '\\') {
      output += '\\';
      output += c;
    }
    else if ((uint8_t)c < 0x20) {
      output += ' ';
    }
    else {
      output += c;
    }
  }

  return output;
}

void handleState() {
  if (!ethernetReady()) {
    server.send(
      503,
      "application/json",
      "{\"error\":\"Ethernet non disponible\"}"
    );
    return;
  }

  relayOnline = readRelayStates();

  String json = "{";

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (i > 0) {
      json += ",";
    }

    json += "\"";
    json += jsonEscape(relayNames[i]);
    json += "\":";
    json += relayStates[i] ? "true" : "false";
  }

  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// ============================================================
// WEB : COMMANDE D'UN RELAIS
// ============================================================

void handleSet() {
  if (!ethernetReady()) {
    server.send(503, "text/plain", "Ethernet non disponible");
    return;
  }

  if (!server.hasArg("relay") || !server.hasArg("on")) {
    server.send(400, "text/plain", "Parametres manquants");
    return;
  }

  int relay = server.arg("relay").toInt();
  int on = server.arg("on").toInt();

  if (relay < 0 || relay >= RELAY_COUNT ||
      (on != 0 && on != 1)) {
    server.send(400, "text/plain", "Parametres invalides");
    return;
  }

  if (!writeRelayCommand((uint8_t)relay, on == 1)) {
    relayOnline = false;
    server.send(500, "text/plain", "Erreur I2C relais");
    return;
  }

  delay(20);

  if (!readRelayStates()) {
    relayOnline = false;
    server.send(
      500,
      "text/plain",
      "Commande envoyee mais lecture I2C impossible"
    );
    return;
  }

  relayOnline = true;
  server.send(200, "text/plain", "OK");
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" ANTENNA CONTROL CENTER - ETH01");
  Serial.println(" VERSION 9 RELAIS");
  Serial.println("========================================");
  Serial.println("Mode relais : I2C uniquement");

  loadNames();

  // Bus I2C #2 : ETH01 maitre vers carte relais
  I2C_RELAY.begin(
    RELAY_I2C_SDA,
    RELAY_I2C_SCL,
    100000
  );

  Serial.println("I2C RELAY : OK");

  // Bus I2C #1 : ETH01 esclave vers LCD S3
  I2C_LCD.begin(
    LCD_I2C_ADDR,
    LCD_I2C_SDA,
    LCD_I2C_SCL,
    100000
  );

  I2C_LCD.onReceive(onLCDReceive);
  I2C_LCD.onRequest(onLCDRequest);

  Serial.println("I2C LCD   : OK");

  // Ethernet LAN8720
  WiFi.onEvent(onEvent);

  Serial.println("Initialisation Ethernet LAN8720...");

  bool ethOK = ETH.begin(
    ETH_PHY_ADDR,
    ETH_PHY_POWER,
    ETH_PHY_MDC,
    ETH_PHY_MDIO,
    ETH_PHY_LAN8720,
    ETH_CLK_MODE
  );

  if (!ethOK) {
    Serial.println("ERREUR : initialisation Ethernet");
  }

  Serial.println("Attente adresse IP...");

  unsigned long startWait = millis();

  while (!ethernetReady() &&
         millis() - startWait < 10000) {
    delay(100);
  }

  if (ethernetReady()) {
    String ip = ETH.localIP().toString();
    ip.toCharArray(cachedIP, sizeof(cachedIP));

    Serial.println("ETH01 pret");
    Serial.print("Adresse Web : http://");
    Serial.println(ETH.localIP());
  } else {
    Serial.println("ETH01 pret");
    Serial.println("Adresse IP en attente du DHCP");
  }

  // Serveur HTTP
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/set", HTTP_GET, handleSet);

  server.onNotFound([]() {
    server.send(404, "text/plain", "Page introuvable");
  });

  server.begin();

  Serial.println("WebServer : port 80");

  // Synchronisation des noms avec la carte relais
  delay(300);

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if (!sendRelayName(i, relayNames[i])) {
      Serial.printf(
        "Attention : envoi nom relais %u impossible\n",
        i + 1
      );
    }

    delay(5);
  }

  // Lecture initiale des états
  if (readRelayStates()) {
    relayOnline = true;
    Serial.println("Carte relais : ONLINE");
  } else {
    relayOnline = false;
    Serial.println("Carte relais : OFFLINE");
  }

  Serial.println("========================================");
  Serial.println("SYSTEME PRET");
  Serial.println("========================================");
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  processLCDCommands();

  server.handleClient();

  pollRelay();

  // Mise à jour de l'adresse IP en cache pour le LCD
  static unsigned long lastIPCheck = 0;

  if (millis() - lastIPCheck >= 1000) {
    lastIPCheck = millis();

    if (ethernetReady()) {
      String ip = ETH.localIP().toString();

      if (strncmp(cachedIP, ip.c_str(), sizeof(cachedIP)) != 0) {
        ip.toCharArray(cachedIP, sizeof(cachedIP));
      }
    } else if (strcmp(cachedIP, "0.0.0.0") != 0) {
      strncpy(cachedIP, "0.0.0.0", sizeof(cachedIP));
      cachedIP[sizeof(cachedIP) - 1] = '\0';
    }
  }

  delay(1);
}
