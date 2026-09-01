/* ESP32-S3 + TFT_eSPI + tactile FT6336U + WT32 I2C
   Les noms sont modifiables sur l'ecran par appui long (700 ms).
   Les noms sont memorises dans Preferences et envoyes au WT32.
   IMPORTANT: GPIO LCD/backlight/tactile peuvent varier selon la revision.
   Clavier virtuel : chiffres (0-9) + lettres (AZERTY sans accents).
*/
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <TFT_eSPI.h>

TFT_eSPI tft=TFT_eSPI(); Preferences prefs;
const uint8_t WT32=0x12, TOUCH=0x38;
const int I2C_SDA=16,I2C_SCL=15;
char names[8][18]={"Relais 1","Relais 2","Relais 3","Relais 4","Relais 5","Relais 6","Relais 7","Relais 8"};
uint8_t states[8]={0};
struct B{int x,y,w,h;}; B b[8];
unsigned long touchStart=0; int touchIndex=-1; bool wasTouch=false;
unsigned long lastPoll=0;
String editName=""; int editIndex=-1; bool editing=false;
// Chiffres (10) + lettres AZERTY (26) = 36 touches
const char keys[]="0123456789AZERTYUIOPQSDFGHJKLMWXCVBN";
const int NKEYS=36;

bool touchRead(uint16_t&x,uint16_t&y){
   Wire.beginTransmission(TOUCH);
   Wire.write(2);
   if(Wire.endTransmission(false))return false;
   if(Wire.requestFrom(TOUCH,(uint8_t)5)!=5)return false;
   uint8_t n=Wire.read();
   uint8_t xh=Wire.read(),xl=Wire.read(),yh=Wire.read(),yl=Wire.read();
   if(!(n&15))return false;
   x=((xh&15)<<8)|xl;
   y=((yh&15)<<8)|yl;
   if(x>=320)x=319;
   if(y>=480)y=479;
   return true;
}
void makeButtons(){
   for(int i=0;i<8;i++){int row=i/2,col=i%2;
                        b[i]={10+col*155,65+row*91,145,82};
                       }
}
void sendCmd(uint8_t r,bool on){
   Wire.beginTransmission(WT32);
   Wire.write(1);
   Wire.write(r);
   Wire.write(on?1:0);
   Wire.endTransmission();
}
void sendName(uint8_t r){
   uint8_t l=min((int)strlen(names[r]),17);
   Wire.beginTransmission(WT32);
   Wire.write(3);
   Wire.write(r);
   Wire.write(l);
   Wire.write((uint8_t*)names[r],l);
   Wire.endTransmission();
}
void loadNames(){
   prefs.begin("names",false);
   for(int i=0;i<8;i++){String k=String("n")+i,s=prefs.getString(k.c_str(),"");
                        if(s.length()){strncpy(names[i],s.c_str(),17);
                                       names[i][17]=0;
                                      }
                       }
}
void saveName(){
if(editIndex<0)return;
editName.trim();
if(!editName.length())return;
editName.toCharArray(names[editIndex],18);
prefs.putString((String("n")+editIndex).c_str(),names[editIndex]);
sendName(editIndex);
}
void draw(){
  tft.fillScreen(TFT_DARKGREY);
  tft.fillRect(0,0,320,52,TFT_NAVY);
  tft.setTextColor(TFT_WHITE,TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("COMMANDE 8 RELAIS",160,17,2);
  tft.drawString("WT32-ETH01",160,38,2);
  tft.setTextDatum(TL_DATUM);
  for(int i=0;i<8;i++){uint16_t c=states[i]?TFT_GREEN:TFT_DARKGREY;
  tft.fillRoundRect(b[i].x,b[i].y,b[i].w,b[i].h,9,c);
  tft.drawRoundRect(b[i].x,b[i].y,b[i].w,b[i].h,9,TFT_WHITE);
  tft.setTextColor(TFT_WHITE,c);tft.setTextDatum(MC_DATUM);
  String s=names[i];
  if(s.length()>15)s=s.substring(0,15);
  tft.drawString(s, b[i].x+72,b[i].y+22,s.length()>10?1:2);
  tft.drawString(states[i]?"ON":"OFF",b[i].x+72,b[i].y+58,4);
  }
  tft.setTextDatum(TL_DATUM);}
void readStates(){
  Wire.beginTransmission(WT32);
  Wire.write(2);Wire.endTransmission();
  delay(2);
  Wire.requestFrom(WT32,(uint8_t)8);
  for(int i=0;
  i<8;i++)states[i]=Wire.available()?Wire.read():0;
  }
void keyboard(){
  tft.fillScreen(TFT_BLACK);tft.setTextColor(TFT_WHITE,TFT_BLACK);tft.setTextDatum(TL_DATUM);
  tft.drawString("Nouveau nom :",8,8,2);
  tft.fillRoundRect(8,35,304,42,6,TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(editName,160,56,2);
  int W=29,H=43;
  for(int i=0;i<NKEYS;i++){
    int row=i/10,col=i%10;
    int x=5+col*31,y=90+row*47;
    tft.fillRoundRect(x,y,W,H,5,TFT_NAVY);
    tft.setTextColor(TFT_WHITE,TFT_NAVY);
    tft.drawString(String(keys[i]),x+W/2,y+H/2,2);
  }
  // 4 rangees de touches (10+10+10+6) -> derniere rangee finit vers y=278
  tft.fillRoundRect(8,286,145,42,6,TFT_DARKGREY);
  tft.fillRoundRect(167,286,145,42,6,TFT_GREEN);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("EFFACER",80,307,2);
  tft.drawString("VALIDER",239,307,2);
  tft.fillRoundRect(8,338,304,42,6,TFT_NAVY);
  tft.drawString("ESPACE / ANNULER",160,359,2);
  tft.setTextDatum(TL_DATUM);
}
void editTouch(uint16_t x,uint16_t y){
  if(y>=90&&y<278){
    int row=(y-90)/47,col=(x-5)/31;
    if(col>=0&&col<10){
      int k=row*10+col;
      if(k<NKEYS){editName+=keys[k];keyboard();}
    }
    return;
  }
  if(y>=286&&y<328){
    if(x<155){ if(editName.length())editName.remove(editName.length()-1); }
    else { saveName();editing=false;draw();return; }
    keyboard();
    return;
  }
  if(y>=338&&y<380){editing=false;draw();return;}
}
void setup(){
   Serial.begin(115200);
   Wire.begin(I2C_SDA,I2C_SCL,100000);
   loadNames();
   tft.init();
   tft.setRotation(0);
   makeButtons();
   for(int i=0;
      i<8;i++)sendName(i);
   readStates();draw();
}
void loop(){
  if(editing){
    uint16_t x,y;
     bool t=touchRead(x,y);
    if(t&&!wasTouch)editTouch(x,y);
    wasTouch=t;
     delay(20);
     return;
  }
  uint16_t x,y;
   bool t=touchRead(x,y);
  if(t&&!wasTouch){
    for(int i=0;i<8;i++)
      if(x>=b[i].x&&x<b[i].x+b[i].w&&y>=b[i].y&&y<b[i].y+b[i].h){touchIndex=i;touchStart=millis();break;
                                                                }
  }
  if(!t&&wasTouch&&touchIndex>=0){
    unsigned long d=millis()-touchStart;
    if(d>=700){editIndex=touchIndex;
               editName=names[editIndex];
               editing=true;keyboard();
              }
    else{sendCmd(touchIndex,!states[touchIndex]);
         delay(20);
         readStates();
         draw();
        }
    touchIndex=-1;
  }
  wasTouch=t;
  if(millis()-lastPoll>500){
    lastPoll=millis();
    uint8_t old[8];memcpy(old,states,8);readStates();
    for(int i=0;i<8;i++) 
       if(old[i]!=states[i]){draw();
                             break;
                            }
  }
  delay(10);
}
