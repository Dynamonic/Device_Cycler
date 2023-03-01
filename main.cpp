#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_I2CDevice.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WifiEspNow.h>
#include <WiFi.h>


//CREATED BY: Dylan McKillip
//FOR: Automating Cartridge CAP/DECAP Functions to Cycle Test Automated Sample Tube Devices
#define VERSION 1.4 // number in hundreds place means experimental version
#define DEV_NUM 8   //CHANGE FOR EACH DEVICE
//VERSION HISTORY:
// 1.0 -> Made cycler cycle cartridge eject/load sequences
// 1.1 -> Made Cycler cycle decap/recap continuously with no need to specify a stop count
// 1.2 -> Fixed timing issues with Serial.Available()
// 1.3 -> Added cycles per hour functionality -> NEEDS TEST
// 1.4 -> Added ability to monitor status of machines from desk
//

//PLANNED UPDATES:
// ... -> fix bug with DEV 8 (V51) and timed cycles


//SERIAL2 PINS FOR ESP32 DEV BOARD
#define RXD2 16
#define TXD2 17

//PINS FOR I2C
#define SCL 22
#define SDA 21

//PINS FOR INTERRUPT BUTTONS
#define DISP_ON 19
#define DISP_OFF 18

//OLED
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 32 // OLED display height, in pixels
#define OLED_RESET 4

//VAR USED FOR DEBUGGING SET TO false FOR PRODUCTION
bool Debug = false;

//VARS FOR CYCLES PER HOUR:
bool timed_cycles = true;
int cycle_rate = 15; //cycles per hour (note: Device max speed is roughly 54 cycles/hr)
int cycle_millis = 3600000/cycle_rate; //(millis between cycles) = (milliseconds per hour)/(cycles per hour)
int current_millis = millis();
int end_millis = current_millis+cycle_millis;
bool recap_success = false;

void update_screen(String status_msg, int cycles);

//Display settings
void IRAM_ATTR disp_on_isr();
void IRAM_ATTR disp_off_isr();
bool prevDisp = false;
bool disp = true;

//ESPNOW Definitions (USED FOR IXC MONITORS)
enum dev_state {
  UNKNOWN,
  RUNNING,
  FAILED
};
dev_state send_state = RUNNING;
static uint8_t PEERS[][6] = {
  {0xA8,0x03,0x2A,0x6B,0x70,0x35},
  {0xA8,0x03,0x2A,0x6B,0x70,0x81},
  {0xA8,0x03,0x2A,0x6B,0x7C,0x59}
};
void send_status_to_monitor();
//msg format "DEV:X,STATUS:X,CYCLES:X"

//ENUMERATED TYPE FOR STATE MACHINE
enum state_cases{
    INIT,
    DECAP,
    RECAP,
    EXIT
};
state_cases cart_state;


//VARS USED FOR FSM AND SERIAL COMMUNICATION
int cycle_end = -1; //NUMBER OF CYCLES TO RUN SET TO -1 FOR CONTINUOUS RUNNING
int cyclecount = 0;

//COMMANDS
char decap_tubes = 'h'; //Commands in IXC cheat sheet wrong
char recap_tubes = 'i'; //Command in IXC Cheat sheet wrong
char cr_msg = char(0x0D); //HEX VAL FOR CARRIAGE RETURN
char end_hex = char(0x00);
char strt_msg = char(0x02);
char end_msg = char(0x03);

String msg = "";
String status_msg = "START";
String prev_state = "";
int prev_cycles = 9999;
char character;
String DISP1 = "STATE:  ";
String DISP2 = "CYCLES: ";

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
char decap_message[] = {decap_tubes, cr_msg};
char recap_message[] = {recap_tubes, cr_msg};
String decap = String(decap_tubes);
String recap = String(recap_tubes);

//Timer variables
bool monitor_alarm = false;
//hw_timer_t * timer = nullptr;
void IRAM_ATTR time_isr();


void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2); //FOR COMMS WITH IXC
  Serial2.setRxBufferSize(256); //Increase buffer
  //Display Setup
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Address 0x3C for 128x32
    Serial1.println(F("SSD1306 allocation failed"));
    ESP.restart();
  }
  pinMode(DISP_ON, INPUT_PULLUP);
  pinMode(DISP_OFF, INPUT_PULLUP);
  attachInterrupt(DISP_ON, disp_on_isr, FALLING);
  attachInterrupt(DISP_OFF, disp_off_isr, FALLING);
  //ESPNOW Setup
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.disconnect();
  WiFi.softAP("ESPNOW", NULL, 1);
  WiFi.softAPdisconnect(false);
  uint8_t mac[6];
  WiFi.softAPmacAddress(mac);
  bool ok = WifiEspNow.begin();
  if(!ok){
    Serial1.println("ESPNOW failed");
    ESP.restart();
  }
  for(int z=0;z<(sizeof(PEERS)/sizeof(PEERS[0]));z++){
    WifiEspNow.addPeer(PEERS[z], 1);
    if(Debug){Serial1.println("Peer Added.");}
  } 
  //END ESPNOW SETUP

  //TIMER INTERRUPT SETUP
  hw_timer_t * timer = timerBegin(0,8000,true);
  timerAttachInterrupt(timer, time_isr, true);
  timerAlarmWrite(timer, 1200000, true);
  timerAlarmEnable(timer);
  //INITIALIZE FSM AND START DISPLAY
  cart_state = INIT;
  update_screen(status_msg, cyclecount);
}

void loop() {
  //Serial.println("Hello"); //THIS WORKS
  if(Debug && !msg.equals("")){Serial.println(msg);} //USED FOR DEBUG
  //MAIN FINITE STATE MACHINE
  switch(cart_state)
  {
      case INIT:{
        cyclecount = 0;
        //Send MSG to eject cart
        Serial2.print(decap);
        status_msg = "DECAP...";
        cart_state = DECAP;
        if(Debug){
            Serial1.println("IXC Cycle Tester\nVersion: "+String(VERSION, 2));
            Serial1.println("Device #:"+String(DEV_NUM));
            if(timed_cycles){
              Serial1.println("Cyc/Hr: "+String(cycle_rate));
            }
        }
        delay(100);
        send_status_to_monitor();
        break;
      }
      case DECAP: {//DECAP CHECK
        if (msg.indexOf("DecapDONE")>=0){ //LOAD SUCCESS
          cyclecount += 1;
          delay(200);
          //Send command to recieve cartridge status
          Serial2.print(recap);
          status_msg = "RECAP...";
          cart_state = RECAP;
          if(timed_cycles){
            end_millis = millis()+cycle_millis;
          }
        }
        /*A LOAD FAILURE IMMEDIATELY TRIES TO EJECT AND 
        //THROWS NO FAILURE MESSAGE SO SEE IF IT EJECTED 
        //OR THROWS AN EJECT ERROR.
        */
        else if(msg.indexOf("Recap")>=0 || msg.indexOf("ERROR")>=0){ //DECAP FAIL
          status_msg = "DECAP ERR";
          send_state = FAILED;
          cart_state = EXIT;
        }
        break;
      }
      case RECAP:{ 
        //9-13-2021:(having interesting interaction where cycler gets stuck on decap)
        // ^ Probably happening here
        
        if (msg.indexOf("RecapDONE")>=0 || recap_success){ //FLAG SET FOR GOOD RECAP
          recap_success = true;
         }
        else if(msg.indexOf("ERROR")>=0){
          status_msg = "RECAP ERR";
          send_state = FAILED;
          cart_state = EXIT;
        }

        if(!timed_cycles){ //IF RUNNING FULL SPEED
        //9-13-2021: (this part works.)
          if (recap_success){ //EJECT SUCCESS
            //Send command to decap
            recap_success = false;
            delay(200);
            Serial2.print(decap);
            status_msg = "DECAP...";
            cart_state = DECAP;
          }
        }
        else{ //IF USING TIMED CYCLES
        //9-13-2021:(what is wrong with this?)
          current_millis = millis(); //Get the time
          if (recap_success && current_millis>=end_millis){ //DELAY THE CYCLER TO START AGAIN AT THE PROPER TIME
            end_millis = millis()+cycle_millis; //RESET NEXT CYCLE TIME
            recap_success = false; //RESET CYCLE FLAG
            //SEND DECAP COMMAND
            delay(200);
            Serial2.print(decap);
            status_msg = "DECAP...";
            cart_state = DECAP;
          }
        // end (what is wrong with this?)
        }

        break;
      }
      case EXIT:{
        //DOES NOTHING AND STOPS SERIAL2 AND SCREEN UPDATE
        update_screen(status_msg, cyclecount);
        break;
      }
      default:{
        //Serial.print("Error in switch statement");
        break;
      }
  }//END OF FSM
  
  msg = "";
  if (cart_state != EXIT){ //ONLY UPDATES THE SCREEN WHILE STILL RUNNING TEST
    update_screen(status_msg, cyclecount);
  }

  //Cycle Monitor Main Loop
  if(monitor_alarm){
    monitor_alarm = false;
    send_status_to_monitor();
  }

  if (Serial2.available() && cart_state != EXIT){
    bool end = false;
    int timeout = millis()+ 100; // set timeout to 50ms
    while(millis() < timeout){
      if(Serial2.available()){
        character = Serial2.read();
        if(character == end_msg){
          end = true;
          break;
        }
        else{
          msg.concat(character);
          timeout = millis()+100; //reset timeout
        }
      }
    }
    if (end == false){ //if we never see an end char after timeout clear message
      msg = "";
    }
  }


}
//Loop end

//HELPERS

//FUNCTION TO UPDATE WITH STATUS AND NUMBER OF CYCLES
void update_screen(String status, int cycles){
  if(!prev_state.equals(status) || prev_cycles != cycles || prevDisp != disp){

    prev_state = status;
    prev_cycles = cycles;
    prevDisp = disp;
    String topline = DISP1+status;
    String lowerline = DISP2+String(cycles);
    if(disp){
      if(Debug){Serial.println("display on");}
      display.clearDisplay();
      display.setTextSize(1);             // Normal 1:1 pixel scale
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0,0);
      display.println("IXC CAP CYCLER #:"+String(DEV_NUM));
      if(timed_cycles){
        display.println("v:"+String(VERSION,2)+" cyc/hr:"+String(cycle_rate));
      }
      else{
        display.println("v:" + String(VERSION, 2)+" cyc/hr:MAX!");
      }
      display.println(topline);
      display.println(lowerline);
      display.display();
    }
    else{
      display.clearDisplay();
      display.display();
    }
    if(Debug){
      Serial1.println('\n'+topline);
      Serial1.println(lowerline);
    }
  }
}

//FUNCTION FOR SENDING STATUS MESSAGE TO CYCLE MONITOR
void send_status_to_monitor(){
  String msg_to_send = "DEV:"+String(DEV_NUM)+
                      ",STATUS:"+String(static_cast<int>(send_state))+
                      ",CYCLES:"+String(cyclecount);
  uint8_t msg_converted[msg_to_send.length()] = {};
  for(int i=0;i<msg_to_send.length();i++){
    msg_converted[i] = uint8_t(msg_to_send[i]);
  }
  WifiEspNow.send(NULL, msg_converted, sizeof(msg_converted)); //sending to null sends to all peers in list
}

void IRAM_ATTR time_isr(){
  monitor_alarm = true;
}
void IRAM_ATTR disp_on_isr(){
  disp = true;
}
void IRAM_ATTR disp_off_isr(){
  disp = false;
}


  
//OLD VERSION CODE: circa version 1.0/1.1
  while (Serial2.available() && cart_state!=EXIT){
    //THIS NEEDS TO BE READ MSG TYPE STUFF
    character = Serial2.read();
    if (character == end_msg){ //CHECK THIS
      break;
    }
    else{
      msg.concat(character);
    }
    delay(100); 
    //^ Added to make sure that the read is slower than the serial out of the machine so that we see the whole message,
    //     some devices are too slow at outputting serial messages compared to the microcontroller
  }
