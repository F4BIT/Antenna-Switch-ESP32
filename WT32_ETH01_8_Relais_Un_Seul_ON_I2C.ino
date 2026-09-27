/******************************************************************
 *  ANTENNA CONTROL CENTER
 *  ESP32-ETH01 / LAN8720 - 8 relais
 *  Auteur :  F4BIT Stéphane
 *  Date   :  2026-09-27
 *  ---------------------------------------------------------------
 *  Fonctionnalités :
 *    - 8 relais à commande active LOW
 *    - Commande locale par I2C
 *    - Serveur Web sur Ethernet
 *    - API REST simple pour commander les relais
 *    - Noms des relais mémorisés en mémoire non volatile
 *    - Un seul relais actif à la fois
 *    - Interface Web responsive PC / tablette / smartphone
 *
 *  Compatibilité :
 *    - ESP32 Arduino Core 2.0.17
 *
 *  Ethernet :
 *    - PHY      : LAN8720
 *    - Adresse  : 1
 *    - MDC      : GPIO 23
 *    - MDIO     : GPIO 18
 *    - POWER    : GPIO 16
 *    - CLK      : GPIO 0
 *
 *  Relais :
 *    R1 : GPIO 4
 *    R2 : GPIO 5
 *    R3 : GPIO 17
 *    R4 : GPIO 19
 *    R5 : GPIO 21
 *    R6 : GPIO 22
 *    R7 : GPIO 25
 *    R8 : GPIO 26
 *
 *  I2C :
 *    - ESP32 en esclave
 *    - Adresse : 0x12
 *    - SDA     : GPIO 32
 *    - SCL     : GPIO 33
 *    - Vitesse : 100 kHz
 *
 *  Protocole I2C :
 *
 *    0x01, relais, état
 *       Commande d'un relais
 *       état = 0 : OFF
 *       état = 1 : ON
 *
 *    0x02
 *       Demande de l'état des 8 relais
 *       Réponse : 8 octets
 *
 *    0x03, relais, longueur, texte...
 *       Modification du nom d'un relais
 *       Longueur maximale : 17 caractères
 *
 *    0x04
 *       Demande des 8 noms
 *       Réponse : 144 octets
 *       8 noms de 18 octets chacun
 *
 *  API Web :
 *
 *    GET /
 *       Interface graphique
 *
 *    GET /api/state
 *       Retourne l'état des 8 relais au format JSON
 *
 *    GET /api/set?relay=0&on=1
 *       Active le relais 1
 *
 *    GET /api/set?relay=0&on=0
 *       Désactive le relais 1
 *
 ********************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <ETH.h>
#include <Wire.h>
#include <WebServer.h>
#include <Preferences.h>

/* ================================================================
 * CONFIGURATION ETHERNET - LAN8720
 * ================================================================ */

#define ETH_PHY_TYPE   ETH_PHY_LAN8720
#define ETH_PHY_ADDR   1
#define ETH_PHY_MDC    23
#define ETH_PHY_MDIO   18
#define ETH_PHY_POWER  16
#define ETH_CLK_MODE   ETH_CLOCK_GPIO0_IN

/* ================================================================
 * CONFIGURATION DES RELAIS
 * ================================================================
 *
 * Les sorties sont actives à l'état LOW.
 * Donc :
 *   ON  -> GPIO LOW
 *   OFF -> GPIO HIGH
 *
 * ================================================================ */

const uint8_t relayPins[8] = {
  4, 5, 17, 19, 21, 22, 25, 26
};

constexpr bool RELAY_ACTIVE_LOW = true;

/* ================================================================
 * CONFIGURATION I2C
 * ================================================================ */

const uint8_t I2C_ADDR = 0x12;
const uint8_t I2C_SDA = 32;
const uint8_t I2C_SCL = 33;

/* ================================================================
 * NOM DE L'ESP32 SUR LE RESEAU
 * ================================================================ */

const char *HOSTNAME = "Antenna_Relay_Ctrl";

/* ================================================================
 * SERVEUR WEB ET MEMOIRE
 * ================================================================ */

WebServer server(80);
Preferences prefs;

/* ================================================================
 * VARIABLES D'ETAT
 * ================================================================ */

volatile bool ethConnected = false;

/*
 * Ces indicateurs permettent de demander une réponse I2C
 * depuis la fonction onRequest().
 */
volatile bool i2cRequestStates = false;
volatile bool i2cRequestNames = false;

/*
 * Buffer de réception I2C.
 */
volatile uint8_t rxBuf[64];
volatile uint8_t rxLen = 0;

/*
 * Mutex utilisé pour protéger le buffer I2C.
 */
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * Etat des 8 relais :
 *
 * false = OFF
 * true  = ON
 */
bool relayState[8];

/*
 * Noms des relais.
 *
 * 17 caractères utiles maximum
 * + 1 caractère '\0'
 */
char relayNames[8][18] = {
  "Relais 1",
  "Relais 2",
  "Relais 3",
  "Relais 4",
  "Relais 5",
  "Relais 6",
  "Relais 7",
  "Relais 8"
};

/* ================================================================
 * COMMANDE PHYSIQUE D'UN RELAIS
 * ================================================================ */

void outputRelay(uint8_t i, bool on) {
  /*
   * Protection contre un index invalide.
   */
  if (i >= 8) {
    return;
  }

  /*
   * Les relais sont actifs LOW.
   */
  digitalWrite(
    relayPins[i],
    RELAY_ACTIVE_LOW ? !on : on
  );
}

/* ================================================================
 * ARRET DE TOUS LES RELAIS
 * ================================================================ */

void allOff() {
  for (uint8_t i = 0; i < 8; ++i) {
    relayState[i] = false;
    outputRelay(i, false);
  }
}

/* ================================================================
 * COMMANDE D'UN RELAIS
 * ================================================================
 *
 * Mode exclusif :
 * lorsqu'un relais est activé, tous les autres sont d'abord
 * désactivés.
 *
 * Exemple :
 *   setRelay(2, true)
 *
 * -> Relais 3 ON
 * -> Tous les autres OFF
 *
 * ================================================================ */

void setRelay(uint8_t r, bool on) {
  if (r >= 8) {
    return;
  }

  if (on) {
    allOff();

    relayState[r] = true;

    outputRelay(
      r,
      true
    );
  } else {
    relayState[r] = false;

    outputRelay(
      r,
      false
    );
  }
}

/* ================================================================
 * SAUVEGARDE D'UN NOM DE RELAIS
 * ================================================================ */

void saveName(uint8_t i, const char *s) {
  if (i >= 8) {
    return;
  }

  /*
   * Limitation à 17 caractères.
   */
  strncpy(
    relayNames[i],
    s,
    17
  );

  relayNames[i][17] = '\0';

  /*
   * Chaque relais possède sa propre clé :
   * n0, n1, n2, ... n7
   */
  String key = "n" + String(i);

  prefs.putString(
    key.c_str(),
    relayNames[i]
  );
}

/* ================================================================
 * CHARGEMENT DES NOMS
 * ================================================================ */

void loadNames() {
  /*
   * Ouverture de l'espace de stockage "relays".
   */
  prefs.begin(
    "relays",
    false
  );

  for (uint8_t i = 0; i < 8; ++i) {
    String key = "n" + String(i);

    String stored =
      prefs.getString(
        key.c_str(),
        ""
      );

    /*
     * Si un nom est enregistré, on le récupère.
     */
    if (stored.length() > 0) {
      strncpy(
        relayNames[i],
        stored.c_str(),
        17
      );

      relayNames[i][17] = '\0';
    }
  }
}

/* ================================================================
 * RECEPTION I2C
 * ================================================================
 *
 * Cette fonction est appelée automatiquement par Wire lorsqu'un
 * maître I2C envoie des données.
 *
 * Les données sont simplement copiées dans rxBuf.
 * Le traitement est effectué plus tard dans loop(), afin de ne pas
 * effectuer de traitement lourd dans l'interruption I2C.
 *
 * ================================================================ */

void onReceive(int) {
  portENTER_CRITICAL(&mux);

  rxLen = 0;

  while (
    Wire.available() &&
    rxLen < sizeof(rxBuf)
  ) {
    rxBuf[rxLen++] = Wire.read();
  }

  portEXIT_CRITICAL(&mux);
}

/* ================================================================
 * REPONSE I2C
 * ================================================================
 *
 * Le maître I2C peut demander :
 *
 *   - l'état des relais
 *   - les noms des relais
 *   - l'état Ethernet
 *
 * ================================================================ */

void onRequest() {
  /*
   * Demande de l'état des relais.
   */
  if (i2cRequestStates) {
    i2cRequestStates = false;

    uint8_t out[8];

    for (uint8_t i = 0; i < 8; ++i) {
      out[i] = relayState[i];
    }

    Wire.write(
      out,
      8
    );

    return;
  }

  /*
   * Demande des noms.
   *
   * 8 relais x 18 octets = 144 octets.
   */
  if (i2cRequestNames) {
    i2cRequestNames = false;

    uint8_t out[144] = {0};

    for (uint8_t i = 0; i < 8; ++i) {
      strncpy(
        (char *)&out[i * 18],
        relayNames[i],
        17
      );
    }

    Wire.write(
      out,
      sizeof(out)
    );

    return;
  }

  /*
   * Si aucune demande particulière n'est en attente,
   * on retourne simplement l'état Ethernet.
   *
   * 0 = Ethernet absent
   * 1 = Ethernet connecté avec adresse IP
   */
  uint8_t status =
    ethConnected ? 1 : 0;

  Wire.write(
    &status,
    1
  );
}

/* ================================================================
 * TRAITEMENT DES COMMANDES I2C
 * ================================================================ */

void processI2C() {
  uint8_t buf[64];
  uint8_t n;

  /*
   * Copie protégée du buffer reçu.
   */
  portENTER_CRITICAL(&mux);

  n = rxLen;

  memcpy(
    buf,
    (const void *)rxBuf,
    n
  );

  rxLen = 0;

  portEXIT_CRITICAL(&mux);

  if (!n) {
    return;
  }

  /* --------------------------------------------------------------
   * 0x01 : commande d'un relais
   *
   * Format :
   *   [0] = 0x01
   *   [1] = numéro du relais 0..7
   *   [2] = état 0/1
   * -------------------------------------------------------------- */

  if (
    buf[0] == 0x01 &&
    n >= 3
  ) {
    setRelay(
      buf[1],
      buf[2] != 0
    );
  }

  /* --------------------------------------------------------------
   * 0x02 : demande de l'état des relais
   * -------------------------------------------------------------- */

  else if (
    buf[0] == 0x02
  ) {
    i2cRequestStates = true;
  }

  /* --------------------------------------------------------------
   * 0x03 : modification du nom d'un relais
   *
   * Format :
   *   [0] = 0x03
   *   [1] = relais
   *   [2] = longueur
   *   [3...] = texte
   * -------------------------------------------------------------- */

  else if (
    buf[0] == 0x03 &&
    n >= 4
  ) {
    uint8_t index = buf[1];

    uint8_t len =
      min(
        (int)buf[2],
        17
      );

    if (
      index < 8 &&
      n >= len + 3
    ) {
      char txt[18] = {0};

      memcpy(
        txt,
        &buf[3],
        len
      );

      saveName(
        index,
        txt
      );
    }
  }

  /* --------------------------------------------------------------
   * 0x04 : demande des noms
   * -------------------------------------------------------------- */

  else if (
    buf[0] == 0x04
  ) {
    i2cRequestNames = true;
  }
}

/* ================================================================
 * EVENEMENTS ETHERNET
 * ================================================================ */

void onEthEvent(
  WiFiEvent_t event
) {
  switch (event) {

    /*
     * Le PHY Ethernet vient de démarrer.
     */
    case ARDUINO_EVENT_ETH_START:

      Serial.println(
        "ETH START"
      );

      ETH.setHostname(
        HOSTNAME
      );

      break;

    /*
     * Liaison physique Ethernet établie.
     */
    case ARDUINO_EVENT_ETH_CONNECTED:

      Serial.println(
        "ETH CONNECTED"
      );

      break;

    /*
     * DHCP terminé / adresse IP obtenue.
     */
    case ARDUINO_EVENT_ETH_GOT_IP:

      ethConnected = true;

      Serial.print(
        "IP  : "
      );

      Serial.println(
        ETH.localIP()
      );

      Serial.print(
        "MASK: "
      );

      Serial.println(
        ETH.subnetMask()
      );

      Serial.print(
        "GW  : "
      );

      Serial.println(
        ETH.gatewayIP()
      );

      break;

    /*
     * Déconnexion du câble ou perte de liaison.
     */
    case ARDUINO_EVENT_ETH_DISCONNECTED:

      Serial.println(
        "ETH DISCONNECTED"
      );

      ethConnected = false;

      break;

    /*
     * Arrêt du PHY.
     */
    case ARDUINO_EVENT_ETH_STOP:

      Serial.println(
        "ETH STOP"
      );

      ethConnected = false;

      break;

    default:
      break;
  }
}

/* ================================================================
 * API WEB : ETAT DES RELAIS
 * ================================================================
 *
 * Exemple de réponse :
 *
 * {
 *   "Relais 1":false,
 *   "Relais 2":true,
 *   ...
 * }
 *
 * ================================================================ */

void handleApiState() {
  if (!ethConnected) {
    server.send(
      503,
      "application/json",
      "{\"error\":\"no eth\"}"
    );

    return;
  }

  String json = "{";

  for (uint8_t i = 0; i < 8; ++i) {
    json += "\"";
    json += relayNames[i];
    json += "\":";
    json += relayState[i]
      ? "true"
      : "false";

    if (i < 7) {
      json += ",";
    }
  }

  json += "}";

  server.send(
    200,
    "application/json",
    json
  );
}

/* ================================================================
 * API WEB : COMMANDE D'UN RELAIS
 * ================================================================
 *
 * Exemple :
 *
 *   /api/set?relay=0&on=1
 *
 * ================================================================ */

void handleApiSet() {
  if (!ethConnected) {
    server.send(
      503,
      "application/json",
      "{\"error\":\"no eth\"}"
    );

    return;
  }

  if (
    !server.hasArg("relay") ||
    !server.hasArg("on")
  ) {
    server.send(
      400,
      "application/json",
      "{\"error\":\"missing param\"}"
    );

    return;
  }

  uint8_t idx =
    server.arg(
      "relay"
    ).toInt();

  bool on =
    server.arg(
      "on"
    ) != "0";

  if (idx >= 8) {
    server.send(
      404,
      "application/json",
      "{\"error\":\"relay out of range\"}"
    );

    return;
  }

  setRelay(
    idx,
    on
  );

  /*
   * Après la commande, on retourne immédiatement
   * le nouvel état complet des relais.
   */
  handleApiState();
}

/* ================================================================
 * PAGE WEB
 * ================================================================
 *
 * L'interface est générée directement par l'ESP32.
 *
 * Responsive :
 *
 *   > 1050 px : 4 colonnes
 *   801-1050  : 3 colonnes
 *   561-800   : 2 colonnes
 *   <= 560    : 1 colonne
 *
 * L'affichage fonctionne également en mode paysage sur smartphone.
 *
 * ================================================================ */

void handleRoot() {
  /*
   * Si Ethernet n'est pas disponible, inutile de générer
   * toute l'interface.
   */
  if (!ethConnected) {
    server.send(
      503,
      "text/html",
      "<!DOCTYPE html>"
      "<html lang='fr'>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<body style='font-family:system-ui;padding:40px;background:#0f172a;color:white'>"
      "<h1>Ethernet non connecté</h1>"
      "</body>"
      "</html>"
    );

    return;
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#0b1120">
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

html{
  width:100%;
  min-height:100%;
  overflow-x:hidden;
}

body{
  margin:0;
  width:100%;
  min-height:100vh;
  font-family:
    Inter,
    ui-sans-serif,
    system-ui,
    -apple-system,
    BlinkMacSystemFont,
    "Segoe UI",
    sans-serif;
  color:var(--text);
  background:
    radial-gradient(
      circle at 10% 0%,
      rgba(56,189,248,.14),
      transparent 32%
    ),
    radial-gradient(
      circle at 90% 10%,
      rgba(34,197,94,.10),
      transparent 28%
    ),
    var(--bg);
}

.container{
  width:100%;
  max-width:1200px;
  margin:0 auto;
  padding:
    clamp(16px,3vw,32px)
    clamp(12px,3vw,24px)
    40px;
}

/* --------------------------------------------------------------
 * En-tête
 * -------------------------------------------------------------- */

header{
  position:relative;
  width:100%;
  min-height:58px;
  display:flex;
  justify-content:center;
  align-items:center;
  margin-bottom:clamp(20px,4vw,32px);
}

header > div:first-child{
  width:100%;
  text-align:center;
  padding:0 190px;
}

h1{
  margin:0;
  font-size:clamp(23px,4vw,36px);
  line-height:1.15;
  letter-spacing:-.03em;
  overflow-wrap:anywhere;
}

/* --------------------------------------------------------------
 * Etat Ethernet
 * -------------------------------------------------------------- */

.connection{
  position:absolute;
  right:0;
  top:50%;
  transform:translateY(-50%);
  display:flex;
  align-items:center;
  gap:9px;
  max-width:190px;
  padding:9px 12px;
  border:1px solid var(--border);
  border-radius:999px;
  background:rgba(17,24,39,.82);
  font-size:13px;
  color:#dbeafe;
  backdrop-filter:blur(12px);
}

.dot{
  flex:0 0 auto;
  width:9px;
  height:9px;
  border-radius:50%;
  background:var(--accent2);
  box-shadow:
    0 0 14px
    rgba(34,197,94,.8);
}

/* --------------------------------------------------------------
 * Grille des relais
 * -------------------------------------------------------------- */

.relay-grid{
  width:100%;
  display:grid;
  grid-template-columns:
    repeat(4,minmax(0,1fr));
  gap:clamp(12px,2vw,18px);
}

/* --------------------------------------------------------------
 * Carte relais
 * -------------------------------------------------------------- */

.relay{
  position:relative;
  overflow:hidden;
  min-width:0;
  min-height:190px;
  padding:clamp(15px,2vw,20px);
  display:flex;
  flex-direction:column;
  justify-content:space-between;
  border:2px solid var(--danger);
  border-radius:20px;
  background:
    linear-gradient(
      145deg,
      rgba(23,32,51,.96),
      rgba(17,24,39,.88)
    );
  box-shadow:var(--shadow);
  transition:
    transform .18s ease,
    border-color .18s ease,
    box-shadow .18s ease,
    background .18s ease;
}

.relay:hover{
  transform:translateY(-3px);
  border-color:rgba(244,63,94,.75);
}

/* --------------------------------------------------------------
 * Relais actif
 * -------------------------------------------------------------- */

.relay.active{
  border-color:#22c55e;
  background:
    linear-gradient(
      145deg,
      rgba(22,101,52,.65),
      rgba(17,24,39,.92)
    );
  box-shadow:
    0 18px 45px
    rgba(34,197,94,.09);
}

.relay.active::before{
  content:"";
  position:absolute;
  inset:0;
  background:
    radial-gradient(
      circle at 85% 10%,
      rgba(34,197,94,.14),
      transparent 38%
    );
  pointer-events:none;
}

/* --------------------------------------------------------------
 * Partie supérieure de la carte
 * -------------------------------------------------------------- */

.relay-top{
  display:flex;
  justify-content:space-between;
  align-items:flex-start;
  gap:8px;
}

.number{
  width:36px;
  height:36px;
  flex:0 0 36px;
  display:grid;
  place-items:center;
  border-radius:11px;
  background:#1e293b;
  color:#cbd5e1;
  font-weight:700;
  font-size:13px;
}

/* --------------------------------------------------------------
 * Indication ACTIF / INACTIF
 * -------------------------------------------------------------- */

.status{
  max-width:100%;
  padding:6px 9px;
  border-radius:999px;
  font-size:11px;
  font-weight:800;
  letter-spacing:.05em;
  background:
    rgba(244,63,94,.11);
  color:#fda4af;
}

.relay.active .status{
  background:
    rgba(34,197,94,.12);
  color:#86efac;
}

/* --------------------------------------------------------------
 * Nom du relais
 * -------------------------------------------------------------- */

.relay-name{
  width:100%;
  margin-top:20px;
  font-size:clamp(15px,1.8vw,18px);
  font-weight:700;
  line-height:1.3;
  text-align:center;
  overflow-wrap:anywhere;
  word-break:break-word;
}

.relay-state{
  min-height:5px;
  margin-top:5px;
  color:var(--muted);
  font-size:13px;
  text-align:center;
}

/* --------------------------------------------------------------
 * Bouton
 * -------------------------------------------------------------- */

.toggle{
  width:100%;
  min-height:48px;
  margin-top:16px;
  padding:11px 14px;
  border:2px solid var(--danger);
  border-radius:12px;
  cursor:pointer;
  color:white;
  background:#263449;
  font-size:14px;
  font-weight:700;
  touch-action:manipulation;
  -webkit-tap-highlight-color:transparent;
  transition:
    transform .12s ease,
    background .18s ease,
    border-color .18s ease,
    box-shadow .18s ease;
}

.relay:not(.active) .toggle{
  border-color:#f43f5e;
  box-shadow:
    0 0 12px
    rgba(244,63,94,.15);
}

.relay.active .toggle{
  background:
    linear-gradient(
      135deg,
      #16a34a,
      #22c55e
    );
  border-color:#22c55e;
  box-shadow:
    0 0 12px
    rgba(34,197,94,.20);
}

.toggle:hover{
  background:#334155;
}

.relay.active .toggle:hover{
  background:
    linear-gradient(
      135deg,
      #16a34a,
      #22c55e
    );
}

.toggle:active{
  transform:scale(.97);
}

.toggle:disabled{
  opacity:.65;
  cursor:wait;
}

/* --------------------------------------------------------------
 * Pied de page
 * -------------------------------------------------------------- */

.footer{
  margin-top:24px;
  padding:0 10px;
  text-align:center;
  color:#64748b;
  font-size:12px;
  line-height:1.5;
}

/* --------------------------------------------------------------
 * Grand écran
 * 4 relais par ligne
 * -------------------------------------------------------------- */

@media(min-width:1051px){

  .relay-grid{
    grid-template-columns:
      repeat(4,minmax(0,1fr));
  }

}

/* --------------------------------------------------------------
 * Ordinateur portable
 * 3 relais par ligne
 * -------------------------------------------------------------- */

@media(min-width:801px) and (max-width:1050px){

  .relay-grid{
    grid-template-columns:
      repeat(3,minmax(0,1fr));
  }

  header > div:first-child{
    padding:0 175px;
  }

  .connection{
    max-width:175px;
  }

}

/* --------------------------------------------------------------
 * Tablette
 * 2 relais par ligne
 * -------------------------------------------------------------- */

@media(min-width:561px) and (max-width:800px){

  .relay-grid{
    grid-template-columns:
      repeat(2,minmax(0,1fr));
  }

  header{
    min-height:55px;
  }

  header > div:first-child{
    padding:0 160px;
  }

  .connection{
    max-width:155px;
    font-size:12px;
  }

}

/* --------------------------------------------------------------
 * Smartphone
 * 1 relais par ligne
 * -------------------------------------------------------------- */

@media(max-width:560px){

  .container{
    padding:
      14px 10px 30px;
  }

  header{
    min-height:0;
    padding-bottom:58px;
    margin-bottom:20px;
  }

  header > div:first-child{
    padding:0;
  }

  h1{
    font-size:
      clamp(22px,7vw,28px);
  }

  .connection{
    position:absolute;
    top:auto;
    bottom:0;
    right:50%;
    transform:translateX(50%);
    width:max-content;
    max-width:calc(100% - 20px);
    padding:8px 12px;
    font-size:12px;
  }

  .relay-grid{
    grid-template-columns:1fr;
    gap:12px;
  }

  .relay{
    min-height:175px;
    border-radius:18px;
  }

  .relay-name{
    font-size:17px;
  }

  .toggle{
    min-height:50px;
    font-size:15px;
  }

}

/* --------------------------------------------------------------
 * Très petit smartphone
 * -------------------------------------------------------------- */

@media(max-width:360px){

  .container{
    padding-left:8px;
    padding-right:8px;
  }

  h1{
    font-size:21px;
  }

  .relay{
    padding:14px;
  }

  .connection{
    font-size:11px;
  }

  .toggle{
    min-height:48px;
  }

}

/* --------------------------------------------------------------
 * Smartphone en mode paysage
 * -------------------------------------------------------------- */

@media(max-width:800px) and (orientation:landscape){

  .relay-grid{
    grid-template-columns:
      repeat(2,minmax(0,1fr));
  }

  .relay{
    min-height:165px;
  }

}

</style>
</head>

<body>

<div class="container">

<header>

  <div>
    <h1>Antenna Control Center</h1>
  </div>

  <div class="connection">

    <span class="dot"></span>

    <div>

      <div>
        Ethernet connecté
      </div>

      <div
        style="
          font-size:11px;
          color:#94a3b8;
          margin-top:2px;
        ">

        IP : )rawliteral";

  html += ETH.localIP().toString();

  html += R"rawliteral(

      </div>

    </div>

  </div>

</header>

<section
  class="relay-grid"
  id="list">

)rawliteral";

  /*
   * Génération des 8 cartes relais.
   */
  for (uint8_t i = 0; i < 8; ++i) {

    html += "<article class='relay";

    if (relayState[i]) {
      html += " active";
    }

    html += "' id='relay";
    html += String(i);
    html += "'>";

    html += "<div>";

    html += "<div class='relay-top'>";

    html += "<div class='number'>";
    html += String(i + 1);
    html += "</div>";

    html += "<div class='status' id='status";
    html += String(i);
    html += "'>";

    html += relayState[i]
      ? "ACTIF"
      : "INACTIF";

    html += "</div>";

    html += "</div>";

    html += "<div class='relay-name'>";
    html += relayNames[i];
    html += "</div>";

    html += "<div class='relay-state' id='state";
    html += String(i);
    html += "'></div>";

    html += "</div>";

    html += "<button class='toggle' onclick='toggle(";
    html += String(i);
    html += ")' id='btn";
    html += String(i);
    html += "'>";

    html += relayState[i]
      ? "Désactiver"
      : "Activer";

    html += "</button>";

    html += "</article>";
  }

  html += R"rawliteral(

</section>

<div class="footer">
  2026 - F4BIT@CopyLeft.
  Sous licence Creative Commons BY-SA 4.0.
</div>

</div>

<script>

/* ==============================================================
 * MISE A JOUR DE L'INTERFACE
 * ==============================================================
 *
 * Reçoit l'objet JSON envoyé par l'ESP32 et met à jour
 * les cartes sans recharger la page.
 * ============================================================== */

function render(data){

  const names =
    Object.keys(data);

  for(
    let i = 0;
    i < 8;
    i++
  ){

    const state =
      !!data[names[i]];

    const card =
      document.getElementById(
        'relay' + i
      );

    const status =
      document.getElementById(
        'status' + i
      );

    const text =
      document.getElementById(
        'state' + i
      );

    const btn =
      document.getElementById(
        'btn' + i
      );

    /*
     * Protection si un élément HTML n'existe pas.
     */
    if(
      !card ||
      !status ||
      !text ||
      !btn
    ){
      continue;
    }

    /*
     * Ajout / suppression de la classe "active".
     */
    card.classList.toggle(
      'active',
      state
    );

    status.textContent =
      state
      ? 'ACTIF'
      : 'INACTIF';

    text.textContent =
      '';

    btn.textContent =
      state
      ? 'Désactiver'
      : 'Activer';
  }
}

/* ==============================================================
 * COMMANDE D'UN RELAIS
 * ============================================================== */

async function toggle(idx){

  const btn =
    document.getElementById(
      'btn' + idx
    );

  /*
   * On détermine l'état demandé à partir du texte du bouton.
   */
  const currentText =
    btn.textContent;

  const turnOn =
    currentText === 'Activer'
    ? 1
    : 0;

  /*
   * Désactivation temporaire du bouton pour éviter
   * plusieurs commandes simultanées.
   */
  btn.disabled = true;

  btn.textContent =
    'Mise à jour…';

  try{

    const response =
      await fetch(
        '/api/set?relay=' +
        idx +
        '&on=' +
        turnOn,
        {
          cache:'no-store'
        }
      );

    if(
      !response.ok
    ){
      throw new Error(
        'Erreur HTTP ' +
        response.status
      );
    }

    /*
     * Récupération du nouvel état des 8 relais.
     */
    const data =
      await response.json();

    render(data);

  }
  catch(error){

    console.error(
      error
    );

    btn.textContent =
      'Erreur';

    /*
     * Retour au texte précédent après 1,2 seconde.
     */
    setTimeout(
      () => {
        btn.textContent =
          currentText;
      },
      1200
    );

  }
  finally{

    btn.disabled =
      false;
  }
}

/* ==============================================================
 * RAFRAICHISSEMENT AUTOMATIQUE
 * ==============================================================
 *
 * L'état des relais est relu toutes les 5 secondes.
 *
 * Cela permet notamment de refléter une commande reçue
 * par I2C sur l'interface Web.
 * ============================================================== */

async function refresh(){

  try{

    const response =
      await fetch(
        '/api/state',
        {
          cache:'no-store'
        }
      );

    if(
      response.ok
    ){

      render(
        await response.json()
      );

    }

  }
  catch(error){

    console.error(
      error
    );

  }
}

/*
 * Première lecture immédiatement après le chargement.
 */
refresh();

/*
 * Puis actualisation automatique toutes les 5 secondes.
 */
setInterval(
  refresh,
  5000
);

</script>

</body>
</html>

)rawliteral";

  server.send(
    200,
    "text/html",
    html
  );
}

/* ================================================================
 * INITIALISATION
 * ================================================================ */

void setup() {

  Serial.begin(
    115200
  );

  Serial.println(
    "\n=== WT32-ETH01 – 8 relais ==="
  );

  /*
   * Configuration des GPIO relais.
   */
  for (
    uint8_t i = 0;
    i < 8;
    ++i
  ) {

    pinMode(
      relayPins[i],
      OUTPUT
    );
  }

  /*
   * Sécurité :
   * tous les relais sont désactivés au démarrage.
   */
  allOff();

  /*
   * Récupération des noms enregistrés.
   */
  loadNames();

  /*
   * Activation des événements Ethernet.
   */
  WiFi.onEvent(
    onEthEvent
  );

  /*
   * Initialisation LAN8720.
   *
   * Cette syntaxe est compatible avec
   * Arduino ESP32 Core 2.0.17.
   */
  ETH.begin(
    ETH_PHY_ADDR,
    ETH_PHY_POWER,
    ETH_PHY_MDC,
    ETH_PHY_MDIO,
    ETH_PHY_TYPE,
    ETH_CLK_MODE
  );

  /*
   * Initialisation I2C en mode esclave.
   */
  Wire.begin(
    I2C_ADDR,
    I2C_SDA,
    I2C_SCL,
    100000
  );

  /*
   * Fonctions appelées par Wire.
   */
  Wire.onReceive(
    onReceive
  );

  Wire.onRequest(
    onRequest
  );

  /*
   * Routes du serveur Web.
   */
  server.on(
    "/",
    handleRoot
  );

  server.on(
    "/api/state",
    handleApiState
  );

  server.on(
    "/api/set",
    handleApiSet
  );

  /*
   * Démarrage du serveur HTTP.
   */
  server.begin();

  Serial.println(
    "Serveur web démarré (port 80)"
  );
}

/* ================================================================
 * BOUCLE PRINCIPALE
 * ================================================================ */

void loop() {

  /*
   * Traitement des éventuelles commandes I2C.
   */
  processI2C();

  /*
   * Traitement des requêtes HTTP.
   */
  server.handleClient();

  /*
   * Petite pause pour laisser du temps aux autres tâches ESP32.
   */
  delay(5);
}
