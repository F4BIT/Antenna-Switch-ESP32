/* WT32-ETH01 + 8 relais + I2C avec ESP32-S3
   I2C slave 0x12. Commandes:
   0x01, relay, 1 = ON ; 0x01, relay, 0 = OFF
   0x02 = demander etats (le master lit 8 octets)
   0x03, index, len, chars... = definir nom
   0x04 = demander noms; le master lit un paquet de 8 noms de 17 octets
   0x05 = demander statut
*/
#include <Arduino.h>
#include <Wire.h>
#include <ETH.h>
#include <WebServer.h>
#include <Preferences.h>

#ifndef ETH_PHY_TYPE
#define ETH_PHY_TYPE ETH_PHY_LAN8720
#define ETH_PHY_ADDR 1
#define ETH_PHY_MDC 23
#define ETH_PHY_MDIO 18
#define ETH_PHY_POWER 16
#define ETH_CLK_MODE ETH_CLOCK_GPIO0_IN
#endif

const uint8_t relayPins[8]={4,5,17,19,21,22,25,26};
const bool RELAY_ACTIVE_LOW=true;
const uint8_t I2C_ADDR=0x12;
const uint8_t I2C_SDA=32, I2C_SCL=33;
const char* HOSTNAME="WT32-ETH01-8RELAIS";

// ---------------- Configuration reseau ----------------
// Mettre a "true" pour une IP fixe, "false" pour DHCP (recu automatiquement).
const bool USE_STATIC_IP = false;

// Ces valeurs ne sont utilisees que si USE_STATIC_IP = true.
// Adaptez-les a votre reseau local.
IPAddress STATIC_IP  (192, 168, 1, 200);
IPAddress STATIC_GW  (192, 168, 1, 1);
IPAddress STATIC_MASK(255, 255, 255, 0);
IPAddress STATIC_DNS1(192, 168, 1, 1);
IPAddress STATIC_DNS2(8, 8, 8, 8);
// --------------------------------------------------------

WebServer server(80); Preferences prefs;
volatile bool ethConnected=false;
volatile bool i2cRequestStates=false, i2cRequestNames=false;
volatile uint8_t rxBuf[64]; volatile uint8_t rxLen=0;
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
bool relayState[8]={0};
char relayNames[8][18]={"Relais 1","Relais 2","Relais 3","Relais 4","Relais 5","Relais 6","Relais 7","Relais 8"};

void outputRelay(uint8_t i,bool on){ digitalWrite(relayPins[i], RELAY_ACTIVE_LOW ? !on:on); }
void allOff(){for(int i=0;i<8;i++){relayState[i]=false;outputRelay(i,false);}}
void setRelay(uint8_t r,bool on){if(r>=8)return; if(on){allOff();relayState[r]=true;outputRelay(r,true);}else{relayState[r]=false;outputRelay(r,false);}}
void saveName(uint8_t i,const char* s){if(i>=8)return; strncpy(relayNames[i],s,17);relayNames[i][17]=0; prefs.putString((String("n")+i).c_str(),relayNames[i]);}
void loadNames(){prefs.begin("relays",false); for(int i=0;i<8;i++){String k=String("n")+i; String s=prefs.getString(k.c_str(),""); if(s.length()){strncpy(relayNames[i],s.c_str(),17);relayNames[i][17]=0;}}}

void onReceive(int n){
  portENTER_CRITICAL(&mux); rxLen=0; while(Wire.available() && rxLen<64) rxBuf[rxLen++]=Wire.read(); portEXIT_CRITICAL(&mux);
}
void onRequest(){
  if(i2cRequestNames){i2cRequestNames=false; uint8_t out[8*18]; memset(out,0,sizeof(out)); for(int i=0;i<8;i++) strncpy((char*)&out[i*18],relayNames[i],17); Wire.write(out,sizeof(out)); return;}
  if(i2cRequestStates){i2cRequestStates=false; uint8_t out[8]; for(int i=0;i<8;i++)out[i]=relayState[i]; Wire.write(out,8); return;}
  uint8_t st=ethConnected?1:0; Wire.write(&st,1);
}
void processI2C(){uint8_t b[64],n; portENTER_CRITICAL(&mux);n=rxLen;memcpy(b,(const void*)rxBuf,n);rxLen=0;portEXIT_CRITICAL(&mux); if(!n)return;
  if(b[0]==0x01 && n>=3){setRelay(b[1],b[2]!=0);}
  else if(b[0]==0x03 && n>=3){uint8_t r=b[1],l=min((int)b[2],17); if(r<8 && n>=3+l){char s[18];memset(s,0,18);memcpy(s,&b[3],l);saveName(r,s);}}
  else if(b[0]==0x04)i2cRequestNames=true;
  else if(b[0]==0x02)i2cRequestStates=true;
}
void onEvent(arduino_event_id_t event){switch(event){
  case ARDUINO_EVENT_ETH_START:
    ETH.setHostname(HOSTNAME);
    if (USE_STATIC_IP) {
      if (!ETH.config(STATIC_IP, STATIC_GW, STATIC_MASK, STATIC_DNS1, STATIC_DNS2)) {
        Serial.println("Echec de la configuration IP fixe, retour DHCP");
      }
    }
    break;
  case ARDUINO_EVENT_ETH_GOT_IP:
    ethConnected=true;
    Serial.print(USE_STATIC_IP ? "IP fixe: " : "IP (DHCP): ");
    Serial.println(ETH.localIP());
    break;
  case ARDUINO_EVENT_ETH_LOST_IP:
  case ARDUINO_EVENT_ETH_DISCONNECTED:
    ethConnected=false;
    break;
  default:
    break;
}}
String page(){String h=R"rawliteral(<!doctype html><html lang='fr'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>8 relais</title><style>body{font-family:Arial;background:#eee;margin:0;padding:16px}.c{max-width:900px;margin:auto;background:#fff;padding:20px;border-radius:14px}.g{display:grid;grid-template-columns:repeat(4,1fr);gap:12px}.r{padding:15px;border:1px solid #ccc;border-radius:12px;text-align:center}.on{background:#28a745;color:#fff}.off{background:#eee}.b{padding:10px;border:0;border-radius:8px;margin:3px;font-size:16px}input{width:90%;padding:8px} @media(max-width:700px){.g{grid-template-columns:repeat(2,1fr)}} </style></head><body><div class='c'><h1>Commande 8 relais</h1><div id='g' class='g'></div></div><script>
async function refresh(){let d=await fetch('/api').then(r=>r.json());let g=document.getElementById('g');g.innerHTML='';d.forEach((x,i)=>{g.innerHTML+=`<div class="r ${x.on?'on':'off'}"><h2>${esc(x.name)}</h2><div><button class="b" onclick="cmd(${i},1)">ON</button><button class="b" onclick="cmd(${i},0)">OFF</button></div><input id="n${i}" value="${esc(x.name)}"><button class="b" onclick="rename(${i})">Renommer</button></div>`})}function esc(s){return s.replaceAll('&','&amp;').replaceAll('"','&quot;').replaceAll('<','&lt;').replaceAll('>','&gt;')}async function cmd(i,s){await fetch(`/relay?relay=${i}&state=${s}`);refresh()}async function rename(i){let n=document.getElementById('n'+i).value.trim();if(n)await fetch('/name?relay='+i+'&name='+encodeURIComponent(n));refresh()}refresh();setInterval(refresh,1000);</script></body></html>)rawliteral";return h;}
void handleRoot(){server.send(200,"text/html; charset=utf-8",page());}
void handleApi(){String j="[";for(int i=0;i<8;i++){if(i)j+=",";j+="{\"name\":\"";String s=relayNames[i];s.replace("\\","\\\\");s.replace("\"","\\\"");j+=s;j+="\",\"on\":"+(relayState[i]?String("true"):String("false"))+"}";}j+="]";server.send(200,"application/json",j);}
void handleRelay(){if(!server.hasArg("relay")||!server.hasArg("state")){server.send(400,"text/plain","Parametres manquants");return;}int r=server.arg("relay").toInt(),s=server.arg("state").toInt();if(r<0||r>7||s<0||s>1){server.send(400,"text/plain","Parametres invalides");return;}setRelay(r,s);server.send(200,"text/plain","OK");}
void handleName(){if(!server.hasArg("relay")||!server.hasArg("name")){server.send(400,"text/plain","Parametres manquants");return;}int r=server.arg("relay").toInt();if(r<0||r>7){server.send(400,"text/plain","Relais invalide");return;}saveName(r,server.arg("name").c_str());server.send(200,"text/plain","OK");}
void setup(){Serial.begin(115200);for(auto p:relayPins)pinMode(p,OUTPUT);allOff();loadNames();Network.onEvent(onEvent);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
ETH.begin(ETH_PHY_TYPE,ETH_PHY_ADDR,ETH_PHY_MDC,ETH_PHY_MDIO,ETH_PHY_POWER,ETH_CLK_MODE);
#else
ETH.begin(ETH_PHY_ADDR,ETH_PHY_POWER,ETH_PHY_MDC,ETH_PHY_MDIO,ETH_PHY_TYPE,ETH_CLK_MODE);
#endif
Wire.begin(I2C_ADDR,I2C_SDA,I2C_SCL,100000);Wire.onReceive(onReceive);Wire.onRequest(onRequest);server.on("/",HTTP_GET,handleRoot);server.on("/api",HTTP_GET,handleApi);server.on("/relay",HTTP_GET,handleRelay);server.on("/name",HTTP_GET,handleName);server.begin();Serial.println("WT32 pret");}
void loop(){processI2C();server.handleClient();}
