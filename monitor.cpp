#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <WifiEspNow.h>
#include <WiFi.h>


#define VERSION 1.0
//Version History:
//1.0 -> Basic monitor implemented
//

//Planned Updates:
//Display with cycles and Status
//
bool Debug = true;

enum dev_state {
  UNKNOWN,
  RUNNING,
  FAILED
};

const int devices[] = {8, 7, 6, 5, 4, 3, 2, 1};
const int num_pixels = 8;
dev_state dev_running[sizeof(devices)/sizeof(devices[0])];



//uses timing for communication checking
long last_updated[sizeof(devices)];
long current = 0; //current time
long unknown_diff = 3;//Time in minutes until status is set to unkown
long error_diff = 5;//Time in minutes until state set to error
long unknown_mils = unknown_diff*60000;
long error_mils = error_diff*60000;
hw_timer_t * timer = NULL;
volatile int timer_count = 0;

//COLORS
int ERROR[] = {255,0,0,0};
int RUNS[] = {0,255,0,0};
int IDK[] = {0,0,255,0};

//MSG
String msg = ""; //MSG SHOULD BE IN FORMAT "DEV:XX,STATUS:X,CYCLES:XXX" where X's indicate a number or enum


//Helper Definitions
void IRAM_ATTR time_isr();
void check_status(void);
void update_pixels(void);
void update_status_from_msg(const uint8_t mac[WIFIESPNOW_ALEN], const uint8_t* buf, size_t count, void* arg);



Adafruit_NeoPixel pixels(8, 18, NEO_GRBW); //pixels(num_leds, pin_num, NEO_RGBW)

void setup() {
  // put your setup code here, to run once:
  for(int k=0; k<sizeof(devices)/sizeof(devices[0]); k++){
    dev_running[k] = UNKNOWN;
    last_updated[k] = 0;
  }
  pixels.begin();
  pixels.setBrightness(50);
  pixels.show();

  Serial.begin(9600);
  Serial.println("Serial Started:");

  //START ESPNOW
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.disconnect();
  WiFi.softAP("ESPNOW", nullptr, 1);
  WiFi.softAPdisconnect(false);
  // WiFi must be powered on to use ESP-NOW unicast.
  // It could be either AP or STA mode, and does not have to be connected.
  // For best results, ensure both devices are using the same WiFi channel.
  Serial.print("My MAC address: ");
  Serial.println(WiFi.softAPmacAddress());
  
  uint8_t mac[6];
  WiFi.softAPmacAddress(mac);

  bool ok = WifiEspNow.begin();
  if(!ok){
    Serial.println("WifiEspNow.begin() failed");
    ESP.restart();
  }

  WifiEspNow.onReceive(update_status_from_msg, nullptr); //ESPNOW MSG CALLBACK
  //END ESPNOW SETUP

  //Timer interrupt
  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, time_isr, true);
  timerAlarmWrite(timer, 1000000, true);
  timerAlarmEnable(timer);


  check_status();
  update_pixels();
}

void loop() {
  // put your main code here, to run repeatedly:
  if(timer_count>=10){
    timer_count = 0;
    check_status();
    update_pixels();
    if(Debug){
      Serial.println("States:");
      for(int i=0;i<(sizeof(dev_running)/sizeof(dev_running[0]));i++){
        Serial.print(String(dev_running[i])+", ");
      }
      Serial.println("");
    };
  }
  //loop updates roughly 1 per 10 seconds
}

void IRAM_ATTR time_isr(){
  timer_count++;
}


void check_status(){
  current = millis();
  for(int j=0;j<sizeof(devices)/sizeof(devices[0]);j++){
    long last_time = last_updated[j];
    long diff = current-last_time;
    //SETS STATUS TO UNKNOWN AFTER "UNKNOWN_DIFF" time 
    if(diff<error_mils && diff>unknown_mils && dev_running[j]!=UNKNOWN){
      dev_running[j] = UNKNOWN;
    }
    //SETS_STATUS TO ERROR AFTER "ERROR_TIME" time
    else if(diff>error_mils && dev_running[j] != FAILED){
      dev_running[j] = FAILED;
    }
  }
}


void update_pixels(){
  for(int i = 0; i<sizeof(devices)/sizeof(devices[0]);i++){ // THIS CODE ASSUMES 7 DEVICES AND 8 LEDS
    if(dev_running[i]>1){
      pixels.setPixelColor(i, ERROR[0], ERROR[1], ERROR[2], ERROR[3]);
    }
    else if(dev_running[i]<1){
      pixels.setPixelColor(i, IDK[0], IDK[1], IDK[2], IDK[3]);
    }
    else{ //Dev is running
      pixels.setPixelColor(i, RUNS[0],RUNS[1],RUNS[2],RUNS[3]);
    }
  }
  //pixels.setPixelColor(7,0,0,0,255); //Set last pixel to white
  pixels.show();
}

void update_status_from_msg(const uint8_t mac[WIFIESPNOW_ALEN], const uint8_t* buf, 
                            size_t count, void* arg){
  msg = ""; //Message format: "DEV:X,STATUS:X,CYCLES:X"
  for(int i=0;i<static_cast<int>(count);i++){
    msg += static_cast<char>(buf[i]);
  }
  if(Debug){
    Serial.println(msg);
  }
  int dev_start_idx = msg.indexOf("DEV:")+4;
  int dev_end_idx = msg.indexOf(",STATUS:");
  int status_start_idx = dev_end_idx+8;
  int status_end_idx = msg.indexOf(",CYCLES:");
  int cyc_start_idx = status_end_idx+8;
  if(dev_start_idx>-1 && dev_end_idx>-1 
     && status_start_idx>-1 && status_end_idx>-1
     && cyc_start_idx>-1){
    int device = msg.substring(dev_start_idx,dev_end_idx).toInt();
    dev_state status = static_cast<dev_state>(msg.substring(status_start_idx, status_end_idx).toInt());
    //int cycles = msg.substring(cyc_start_idx).toInt(); //UNUSED as of V1.0
    if(Debug){
      Serial.println(String(device)+", "+String(status));
    }
    for(int j=0;j<sizeof(devices)/sizeof(devices[0]);j++){
      if(device == devices[j]){
        dev_running[j] = status;
        Serial.println(String(dev_running[j]));
        last_updated[j] = millis();
      }
    }
  }
}

