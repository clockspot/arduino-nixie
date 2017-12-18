// Code for Arduino Nano in RLB Designs IN-12/17 clock v5.0
// featuring timekeeping by DS1307 RTC and six digits multiplexed 3x2 via two SN74141 driver chips
// Originally written by Robin Birtles and Chris Gerekos based on http://arduinix.com/Main/Code/ANX-6Tube-Clock-Crossfade.txt
// Refactored and expanded by Luke McKenzie (luke@theclockspot.com)

// TODO: Rotary encoders with velocity - test
// TODO: Alarm - display, set, sound, snooze, 24h silence
// TODO: Timer - display, set, run, sound, silence
// TODO: Cathode anti-poisoning
// TODO: implement other setup options

#include <EEPROM.h>
#include <Wire.h>
#include <RTClib.h>
RTC_DS1307 rtc;
#include <ooPinChangeInt.h>
#include <AdaEncoder.h>

////////// Configuration consts //////////

// These are the RLB board connections to Arduino analog input pins.
// S1/PL13 = Reset
// S2/PL5 = A1
// S3/PL6 = A0
// S4/PL7 = A6
// S5/PL8 = A3
// S6/PL9 = A2
// S7/PL14 = A7
// A6-A7 are analog-only pins that aren't as responsive and require a physical pullup resistor (1K to +5V).

// What input is associated with each control?
const byte mainSel = A2; //main select button - must be equipped
const byte mainAdjUp = A1; //main up/down buttons or rotary encoder - must be equipped
const byte mainAdjDn = A0;
const byte altSel = 0; //alt select button - if unequipped, set to 0
const byte altAdjUp = 0; //A6; //alt up/down buttons or rotary encoder - if unequipped, set to 0
const byte altAdjDn = 0; //A3;

// What type of adj controls are equipped?
// 1 = momentary buttons. 2 = quadrature rotary encoder.
// Currently using AdaEncoder library that uses pin change interrupts, not useful on A6/A7!
const byte mainAdjType = 2;
AdaEncoder mainRot = AdaEncoder('a',mainAdjUp,mainAdjDn);
const byte altAdjType = 0; //if unquipped, set to 0
//AdaEncoder altRot = AdaEncoder('b',altAdjUp,altAdjDn);

// In normal running mode, what do the controls do?
// 255 = nothing, 254 = cycle through functions TODO, 0-3 = go to specific function (see fn)
const byte mainSelFn = 1;
const byte mainAdjFn = 254;
// const byte altSelFn = 255; //3; //timer //if equipped
// const byte altAdjFn = 255; //if equipped

const byte enableSoftAlarmSwitch = 1;
// 1 = yes. Use if using the integrated beeper or another non-switched device (bell solenoid, etc).
// 0 = no. Use if the connected alarm device has its own switch (e.g. clock radio function switch).
//     Alarm will be permanently on in software.
const byte alarmRadio = 0;
// 0 = no. Alarm output is connected to the onboard piezoelectric beeper or similar signal device.
//     When alarm and timer go off, it will output a beep pattern for alarmDur minutes.
// 1 = yes. Alarm output is connected to a relay to switch other equipment (like a radio).
//     When alarm goes off, output will stay on for alarmDur minutes (120 is common).
//     When timer is running, output will stay on until timer runs down.
const byte alarmDur = 3;

// How long (in ms) are the button hold durations?
const word btnShortHold = 1000; //for setting the displayed feataure
const word btnLongHold = 3000; //for for entering SETUP menu
const byte velThreshold = 100; //ms
// When an adj up/down input (btn or rot) follows another in less than this time, value will change more (10 vs 1).
// Recommend ~100 for rotaries. If you want to use this feature with buttons, extend to ~400.


////////// Global consts and vars used in multiple sections //////////

// Hardware inputs
byte btnCur = 0; //Momentary button currently in use - only one allowed at a time
byte btnCurHeld = 0; //Button hold thresholds: 0=none, 1=unused, 2=short, 3=long, 4=set by btnStop()
unsigned long inputLast = 0; //When a button was last pressed / knob was last turned
unsigned long inputLast2 = 0; //Second-to-last of above

// Input handling and value setting
const byte fnCt = 2; //number of functions in the clock
byte fn = 0; //currently displayed function: 0=time, 1=date, 2=alarm, 3=timer, 255=SETUP menu
byte fnSet = 0; //whether this function is currently being set, and which option/page it's on
word fnSetVal; //the value currently being set, if any - unsigned int 0-65535
word fnSetValMin; //min possible - unsigned int
word fnSetValMax; //max possible - unsigned int
bool fnSetValVel; //whether it supports velocity setting (if max-min > 30)
word fnSetValDate[3]; //holder for newly set date, so we can set it in 3 stages but set the RTC only once
const byte alarmTimeLoc = 0; //EEPROM locs 0-1 (2 bytes) in minutes past midnight.
const byte alarmOnLoc = 2; //EEPROM loc 2
unsigned long alarmSoundStart = 0; //also used for timer expiry TODO what happens if they are concurrent?
word snoozeTime = 0; //seconds
word timerTime = 0; //seconds - up to just under 18 hours
const byte optsCt = 18; //number of setup menu options (see readme).
//Option number/fnSet are 1-index, so arrays are padded to be 1-index too, for coding convenience.
//Most vals (default, min, max) are 1-byte. In case of two-byte (max-min>255), high byte is loc, low byte is loc+1.
//         Option number: -  1  2  3  4  5  6  7  8  9  10 11   12   13 14 15 16   17   18
const byte optsLoc[19] = {0, 3, 4, 5, 6, 7, 8, 9,10,11, 12,14,  15,  17,19,20,21,  22,  24}; //EEPROM locs 3-25
const word optsDef[19] = {0, 2, 1, 0, 0, 0, 0, 0, 0, 0,500, 0,1320, 360, 0, 1, 5, 480,1020};
const word optsMin[19] = {0, 1, 1, 0, 0, 0, 0, 0, 0, 0,  1, 0,   0,   0, 0, 0, 0,   0,   0};
const word optsMax[19] = {0, 2, 2, 2, 1,50, 1, 6, 4,60,999, 2,1439,1439, 2, 6, 6,1439,1439};

// Display formatting
byte displayNext[6] = {15,15,15,15,15,15}; //Internal representation of display. Blank to start. Change this to change tubes.


////////// Main code control //////////

void setup(){
  Serial.begin(57600);
  Wire.begin();
  rtc.begin();
  if(!rtc.isrunning()) rtc.adjust(DateTime(2017,1,1,0,0,0)); //TODO test
  initOutputs();
  initInputs();
  initEEPROM(readInput(mainSel)==LOW);
  //debugEEPROM();
  //setCaches();
}

unsigned long pollLast = 0;
void loop(){
  //Things done every 50ms - avoids overpolling(?) and switch bounce(?)
  if(pollLast<millis()+50) {
    pollLast=millis();
    checkRTC(false); //if clock has ticked, decrement timer if running, and updateDisplay
    checkInputs(); //if inputs have changed, this will do things + updateDisplay as needed
    doSetHold(); //if inputs have been held, this will do more things + updateDisplay as needed
  }
  //Things done every loop cycle
  cycleDisplay(); //keeps the display hardware multiplexing cycle going
}


////////// Control inputs //////////
void initInputs(){
  //TODO are there no "loose" pins left floating after this? per https://electronics.stackexchange.com/q/37696/151805
  pinMode(A0, INPUT_PULLUP);
  pinMode(A1, INPUT_PULLUP);
  pinMode(A2, INPUT_PULLUP);
  pinMode(A3, INPUT_PULLUP);
  //4 and 5 used for I2C
  pinMode(A6, INPUT); digitalWrite(A6, HIGH);
  pinMode(A7, INPUT); digitalWrite(A7, HIGH);
}

void checkInputs(){
  // TODO can all this if/else business be defined at load instead of evaluated every sample? OR is it compiled that way?
  //if(millis() >= inputSampleLast+inputSampleDur) { //time for a sample
    //inputSampleLast = millis();
    //potential issue: if user only means to rotate or push encoder but does both?
  
    //sample button states
    checkBtn(mainSel); //main select
    if(mainAdjType==1) { checkBtn(mainAdjUp); checkBtn(mainAdjDn); } //main adjust buttons (if equipped)
    //if(altSel!=0) checkBtn(altSel); //alt select (if equipped)
    //if(altAdjType==1) { checkBtn(altAdjUp); checkBtn(altAdjDn); } //alt adjust buttons (if equipped)
    
    //check adaencoder library to see if rot(s) have moved
    if(mainAdjType==2 || altAdjType==2) checkRot();
    
  //} //end if time for a sample
}

bool readInput(byte pin){
  if(pin==A6 || pin==A7) return analogRead(pin)<100?0:1; //analog-only pins
  else return digitalRead(pin);
}
void checkBtn(byte btn){
  //Changes in momentary buttons, LOW = pressed.
  //When a button event has occurred, will call ctrlEvt
  bool bnow = readInput(btn);
  //If the button has just been pressed, and no other buttons are in use...
  if(btnCur==0 && bnow==LOW) {
    btnCur = btn; btnCurHeld = 0; inputLast2 = inputLast; inputLast = millis();
    ctrlEvt(btn,1); //hey, the button has been pressed
  }
  //If the button is being held...
  if(btnCur==btn && bnow==LOW) {
    if(millis() >= inputLast+btnLongHold && btnCurHeld < 3) {
      btnCurHeld = 3;
      ctrlEvt(btn,3); //hey, the button has been long-held
    }
    else if(millis() >= inputLast+btnShortHold && btnCurHeld < 2) {
      btnCurHeld = 2;
      ctrlEvt(btn,2); //hey, the button has been short-held
    }
  }
  //If the button has just been released...
  if(btnCur==btn && bnow==HIGH) {
    btnCur = 0;
    if(btnCurHeld < 4) ctrlEvt(btn,0); //hey, the button was released
    btnCurHeld = 0;
  }
}
void btnStop(){
  //In some cases, when handling btn evt 1/2/3, we may call this
  //so following events 2/3/0 won't cause unintended behavior
  //(e.g. after a fn change, or going in or out of set)
  btnCurHeld = 4;
}

void checkRot(){
  //Changes in rotary encoders. When rotation(s) occur, will call ctrlEvt to simulate btn presses.
  if(btnCur==0) {
    //TODO does this work with more than one encoder? maybe on separate loops?
    //https://github.com/GreyGnome/AdaEncoder/blob/master/Examples/MyEncoder/MyEncoder.ino
    AdaEncoder *thisEncoder=NULL;
    thisEncoder = AdaEncoder::genie();
    if(thisEncoder!=NULL) {
      inputLast2 = inputLast; inputLast = millis();
      int8_t clicks = thisEncoder->query(); //signed number of clicks it has moved
      byte dir = (clicks<0?0:1);
      clicks = abs(clicks);
      for(byte i=0; i<clicks; i++){ //in case of more than one click
        ctrlEvt((thisEncoder->getID()=='a'?(dir?mainAdjUp:mainAdjDn):(dir?altAdjUp:altAdjDn)),1);
      }
    }
  }
}//end checkRot


////////// Input handling and value setting //////////

void ctrlEvt(byte ctrl, byte evt){  
  //Handle control events (from checkBtn or checkRot), based on current fn and set state.
  //evt: 1=press, 2=short hold, 3=long hold, 0=release.
  //We only handle press evts for adj ctrls, as that's the only evt encoders generate.
  //But we can handle short and long holds and releases for the sel ctrls (always buttons).
  //TODO needs alt handling
  
  if(fn!=255) { //normal fn running/setting (not in setup menu)
    
    if(evt==3 && ctrl==mainSel) { //mainSel long hold: enter SETUP menu
      btnStop(); fn = 255; startOpt(1); return;
    }
    
    DateTime now = rtc.now();

    if(!fnSet) { //fn running
      if(evt==2 && ctrl==mainSel) { //sel hold: enter fnSet
        switch(fn){
          case 0: startSet((now.hour()*60)+now.minute(),0,1439,1); break; //time: set mins
          case 1: fnSetValDate[1]=now.month(), fnSetValDate[2]=now.day(); startSet(now.year(),0,32767,1); break; //date: set year
          case 2: //alarm: set mins
          case 3: //timer: set mins
          default: break;
        }
        return;
      }
      else if((ctrl==mainSel && evt==0) || ((ctrl==mainAdjUp || ctrl==mainAdjDn) && evt==1)) { //sel release or adj press - switch fn, depending on config
        //255 = nothing, 254 = cycle through functions TODO, 0-3 = go to specific function (see fn)
        //we can't handle sel press here because, if attempting to enter fnSet, it would switch the fn first
        bool fnChgd = false;
        if(ctrl==mainSel && mainSelFn!=255) {
          fnChgd = true;
          if(mainSelFn==254) fn = (fn==fnCt-1 ? 0 : fn+1);
          else fn = mainSelFn;
        }
        else if((ctrl==mainAdjUp || ctrl==mainAdjDn) && mainAdjFn!=255) {
          fnChgd = true;
          if(mainAdjFn==254) fn = (ctrl==mainAdjUp?(fn==fnCt-1 ? 0 : fn+1):(fn==0 ? fnCt-1 : fn-1));
          else fn = mainAdjFn;
        }
        if(fnChgd){
          switch(fn){
            case 0: case 1: checkRTC(true); break;
            case 2: //alarm: show
            case 3: //timer: show
            default: break;
          }
        }
      }
    } //end fn running

    else { //fn setting
      if(evt==1) { //we respond only to press evts during fn setting
        if(ctrl==mainSel) { //mainSel push: go to next option or save and exit fnSet
          btnStop(); //not waiting for mainSelHold, so can stop listening here
          switch(fn){
            case 0: //time of day: save in RTC
              rtc.adjust(DateTime(now.year(),now.month(),now.day(),fnSetVal/60,fnSetVal%60,0));
              clearSet(); break;
            case 1: switch(fnSet){ //date: save in RTC - year can be 2-byte int
              case 1: //date: save year, set month
                fnSetValDate[0]=fnSetVal;
                startSet(fnSetValDate[1],1,12,2); break; 
              case 2: //date: save month, set date
                fnSetValDate[1]=fnSetVal;
                startSet(fnSetValDate[2],1,daysInMonth(fnSetValDate[0],fnSetValDate[1]),3); break;
              case 3: //date: save in RTC
                rtc.adjust(DateTime(fnSetValDate[0],fnSetValDate[1],fnSetVal,now.hour(),now.minute(),now.second()));
                //TODO this rounds down the seconds and loses synchronization! find a way to set the date only
                clearSet(); break;
              default: break;
            } break;
            case 2: //alarm
              //EEPROM set TODO
            case 3: //timer
              //TODO
            default: break;
          } //end switch fn
        } //end mainSel push
        if(ctrl==mainAdjUp) doSet(inputLast-inputLast2<velThreshold ? 10 : 1);
        if(ctrl==mainAdjDn) doSet(inputLast-inputLast2<velThreshold ? -10 : -1);
      } //end if evt==1
    } //end fn setting

  } //end normal fn running/setting
  else { //setup menu setting - to/from EEPROM
    
    if(ctrl==mainSel) {
      if(evt==1) { //TODO could consider making it a release, so it doesn't go to the next option before leaving
        finishOpt();
        if(fnSet==optsCt) { //that was the last one – rotate back around
          startOpt(1); return;
        } else {
          startOpt(fnSet+1); return;
        }
      }
      if(evt==2) { //exit setup
        btnStop(); fn = 0; clearSet(); /*debugEEPROM();*/ return; //exit setup
        //setCaches();
      }
    }
    if(ctrl==mainAdjUp && evt==1) doSet(inputLast-inputLast2<velThreshold ? 10 : 1);
    if(ctrl==mainAdjDn && evt==1) doSet(inputLast-inputLast2<velThreshold ? -10 : -1);
    
  } //end setup menu setting
} //end ctrlEvt

void startSet(word n, word m, word x, byte p){ //Enter set state at page p, and start setting a value
  fnSetVal=n; fnSetValMin=m; fnSetValMax=x; fnSetValVel=(x-m>30?1:0); fnSet=p;
  updateDisplay();
}
void doSet(int delta){
  //Does actual setting of fnSetVal, as told to by ctrlEvt or doSetHold. Makes sure it stays within range.
  if(delta>0) if(fnSetValMax-fnSetVal<delta) fnSetVal=fnSetValMax; else fnSetVal=fnSetVal+delta;
  if(delta<0) if(fnSetVal-fnSetValMin<abs(delta)) fnSetVal=fnSetValMin; else fnSetVal=fnSetVal+delta;
  updateDisplay();
}
unsigned long doSetHoldLast;
void doSetHold(){
  //When we're setting via an adj button that's passed a hold point, fire off doSet commands at intervals
  //TODO integrate this with checkInputs?
  if(doSetHoldLast+250<millis()) {
    doSetHoldLast = millis();
    if(fnSet!=0 && ((mainAdjType==1 && (btnCur==mainAdjUp || btnCur==mainAdjDn)) || (altAdjType==1 && (btnCur==altAdjUp || btnCur==altAdjDn))) ){ //if we're setting, and this is an adj input for which the type is button
      bool dir = (btnCur==mainAdjUp || btnCur==altAdjUp ? 1 : 0);
      //If short hold, or long hold but high velocity isn't supported, use low velocity (delta=1)
      if(btnCurHeld==2 || (btnCurHeld==3 && fnSetValVel==false)) doSet(dir?1:-1);
      //else if long hold, use high velocity (delta=10)
      else if(btnCurHeld==3) doSet(dir?10:-10);
    }
  }
}
void clearSet(){ //Exit set state
  startSet(0,0,0,0);
  if(fn==0 || fn==1) checkRTC(true); //force a display update for time and date, as updateDisplay() won't
}

void startOpt(byte n){ //For a given setup menu option (1-index), reads from EEPROM and calls startSet
  startSet(readEEPROM(optsLoc[n],optsMax[n]-optsMin[n]>255?true:false),optsMin[n],optsMax[n],n);
}
void finishOpt(){ //Writes fnSet val to EEPROM if needed
  writeEEPROM(optsLoc[fnSet],fnSetVal,optsMax[fnSet]-optsMin[fnSet]>255?true:false);
}

// void setCaches(){ //Sets a few of the EEPROM settings into global variables for faster access
//   //fadeMax = float(readEEPROM(optsLoc[5],false)); //fade duration - TODO doesn't work
// }

//EEPROM values are exclusively bytes (0-255) or words (unsigned ints, 0-65535)
//If it's a word, high byte is in loc, low byte is in loc+1
void initEEPROM(bool force){
  //Set EEPROM to defaults. Done if force==true
  //or if the first setup menu option has a value of 0 (e.g. first run)
  if(force) { // || EEPROM.read(optsLoc[0])==0
    writeEEPROM(alarmTimeLoc,420,true); //7am - word
    writeEEPROM(alarmOnLoc,enableSoftAlarmSwitch==0?1:0,false); //off, or on if no software switch spec'd
    for(byte i=1; i<=optsCt; i++) writeEEPROM(optsLoc[i],optsDef[i],optsMax[i]-optsMin[i]>255?true:false); //setup menu
  }
}
// void debugEEPROM(){
//   //Writes all the salient clock values from EEPROM to serial
//   Serial.println("////////// EEPROM value dump //////////");
//   Serial.print("alarm time: "); Serial.println(readEEPROM(0,true),DEC);
//   Serial.print("alarm on: "); Serial.println(readEEPROM(2,false),DEC);
//   for(byte i=1; i<=optsCt; i++) {
//     Serial.print("opt "); Serial.print(i,DEC); Serial.print(" is ");
//     Serial.print(readEEPROM(optsLoc[i],optsMax[i]-optsMin[i]>255?true:false));
//     Serial.println(optsMax[i]-optsMin[i]>255?" (word)":" (byte)");
//   }
// }
word readEEPROM(int loc, bool isWord){
  if(isWord) {
    word w = (EEPROM.read(loc)<<8)+EEPROM.read(loc+1); return w; //word / unsigned int, 0-65535
  } else {
    byte b = EEPROM.read(loc); return b; //byte, 0-255
  }
}
void writeEEPROM(int loc, int val, bool isWord){
  if(isWord) {
    Serial.print("EEPROM write word:");
    if(EEPROM.read(loc)!=highByte(val)) {
      EEPROM.write(loc,highByte(val));
      Serial.print(" loc "); Serial.print(loc,DEC);
      Serial.print(" val "); Serial.print(highByte(val),DEC);
    } else Serial.print(" loc "); Serial.print(loc,DEC); Serial.print(" unchanged (no write).");
    if(EEPROM.read(loc+1)!=lowByte(val)) {
      EEPROM.write(loc+1,lowByte(val));
      Serial.print(" loc "); Serial.print(loc+1,DEC);
      Serial.print(" val "); Serial.print(lowByte(val),DEC);
    } else Serial.print(" loc "); Serial.print(loc+1,DEC); Serial.print(" unchanged (no write).");
  } else {
    Serial.print("EEPROM write byte:"); Serial.print(" loc "); Serial.print(loc,DEC);
    if(EEPROM.read(loc)!=val) { EEPROM.write(loc,val);
      Serial.print(" val "); Serial.print(val,DEC);
    } else Serial.print(" unchanged (no write).");
  }
  Serial.println();
}

byte daysInMonth(word y, byte m){
  if(m==2) return (y%4==0 && (y%100!=0 || y%400==0) ? 29 : 28);
  //https://cmcenroe.me/2014/12/05/days-in-month-formula.html
  else return (28 + ((m + (m/8)) % 2) + (2 % m) + (2 * (1/m)));
}


////////// Clock ticking and timed event triggering //////////
byte rtcSecLast = 61;
void checkRTC(bool force){
  //Checks for new time-of-day second; decrements timer; checks for timed events;
  //updates display for running time or date.
  //Check for timeouts based on millis
  if(fnSet && pollLast-inputLast>120000) { fnSet = 0; fn = 0; force=true; } //abandon set
  else if(!fnSet && fn!=0 && !(fn==3 && (timerTime>0 || alarmSoundStart!=0)) && pollLast>inputLast+5000) { fnSet = 0; fn = 0; force=true; } //abandon fn
  //Update things based on RTC
  DateTime now = rtc.now();
  if(rtcSecLast != now.second() || force) {
    rtcSecLast = now.second(); //this was missing! TODO reintroduce
    //trip alarm TODO
    //decrement timer TODO
    //trip minutely date at :30 TODO
    //trip digit cycle TODO
    //finally display live time of day / date
    if(fnSet==0 && fn==0){ //time of day
      byte hr = now.hour();
      if(readEEPROM(optsLoc[1],false)==1) hr = (hr==0?12:(hr>12?hr-12:hr));
      editDisplay(hr, 0, 1, readEEPROM(optsLoc[4],false));
      editDisplay(now.minute(), 2, 3, true);
      if(EEPROM.read(optsLoc[3])==1) editDisplay(now.day(), 4, 5, EEPROM.read(optsLoc[4])); //date
      else editDisplay(now.second(), 4, 5, true); //seconds
    } else if(fnSet==0 && fn==1){ //date
      editDisplay(EEPROM.read(optsLoc[2])==1?now.month():now.day(), 0, 1, EEPROM.read(optsLoc[4]));
      editDisplay(EEPROM.read(optsLoc[2])==1?now.day():now.month(), 2, 3, EEPROM.read(optsLoc[4]));
      blankDisplay(4, 4);
      editDisplay(now.dayOfTheWeek(), 5, 5, false);
    }
  }
}


////////// Display data formatting //////////
void updateDisplay(){
  //Run as needed to update display when the value being shown on it has changed
  //(Ticking time and date are an exception - see checkRTC() )
  //This formats the new value and puts it in displayNext[] for cycleDisplay() to pick up
  if(fnSet) { //setting
    //little tubes:
    if(fn==255) editDisplay(fnSet, 4, 5, false); //setup menu: current option key
    else blankDisplay(4, 5); //fn setting: blank
    //big tubes:
    if(fnSetValMax==1439) { //value is a time of day
      editDisplay(fnSetVal/60, 0, 1, EEPROM.read(optsLoc[4])); //hours with leading zero
      editDisplay(fnSetVal%60, 2, 3, true);
    } else editDisplay(fnSetVal, 0, 3, false); //some other type of value
  }
  else { //fn running
    switch(fn){
      //case 0: //time taken care of by checkRTC()
      //case 1: //date taken care of by checkRTC()
      case 2: //alarm
      case 3: //timer
      break;
    }
  }
} //end updateDisplay()

void editDisplay(word n, byte posStart, byte posEnd, bool leadingZeros){
  //Splits n into digits, sets them into displayNext in places posSt-posEnd (inclusive), with or without leading zeros
  //If there are blank places (on the left of a non-leading-zero number), uses value 15 to blank tube
  //If number has more places than posEnd-posStart, the higher places are truncated off (e.g. 10015 on 4 tubes --> 0015)
  word place;
  for(byte i=0; i<=posEnd-posStart; i++){
    //place = int(pow(10,i)); TODO PROBLEM: int(pow(10,2))==99 and int(pow(10,3))==999. Why??????????
    switch(i){
      case 0: place=1; break;
      case 1: place=10; break;
      case 2: place=100; break;
      case 3: place=1000; break;
      default: break;
    }
    displayNext[posEnd-i] = (i==0&&n==0 ? 0 : (n>=place ? (n/place)%10 : (leadingZeros?0:15)));
  }
} //end editDisplay()
void blankDisplay(byte posStart, byte posEnd){
  for(byte i=posStart; i<=posEnd; i++) displayNext[i]=15;
} //end blankDisplay();


////////// Hardware outputs //////////
//This clock is 2x3 multiplexed: two tubes powered at a time.
//The anode channel determines which two tubes are powered,
//and the two SN74141 cathode driver chips determine which digits are lit.
//4 pins out to each SN74141, representing a binary number with values [1,2,4,8]
byte binOutA[4] = {2,3,4,5};
byte binOutB[4] = {6,7,8,9};
//3 pins out to anode channel switches
byte anodes[3] = {11,12,13};

float fadeMax = 5.0f;
float fadeStep = 1.0f;
int displayLast[6]={11,11,11,11,11,11}; //What is currently being displayed. We slowly fade away from this.
float displayNextFade[6]={0.0f,0.0f,0.0f,0.0f,0.0f,0.0f}; //Fading in displayNext values
float displayLastFade[6]={8.0f,8.0f,8.0f,8.0f,8.0f,8.0f}; //Fading out displayLast values
unsigned long setStartLast = 0; //to control flashing

void initOutputs() {
  for(byte i=0; i<4; i++) { pinMode(binOutA[i],OUTPUT); pinMode(binOutB[i],OUTPUT); }
  for(byte i=0; i<3; i++) { pinMode(anodes[i],OUTPUT); }
  pinMode(10, OUTPUT); //Alarm signal pin
}

void cycleDisplay(){
  bool dim = 0;//(opts[2]>0?true:false); //Under normal circumstances, dim constantly if the time is right
  if(fnSet>0) { //but if we're setting, dim for every other 500ms since we started setting
    if(setStartLast==0) setStartLast = millis();
    dim = 1-(((millis()-setStartLast)/500)%2);
  } else {
    if(setStartLast>0) setStartLast=0;
  }
  
  //Anode channel 0: tubes #2 (min x10) and #5 (sec x1)
  setCathodes(displayLast[2],displayLast[5]); //Via d2b decoder chip, set cathodes to old digits
  digitalWrite(anodes[0], HIGH); //Turn on tubes
  delay(displayLastFade[0]/(dim?4:1)); //Display for fade-out cycles
  setCathodes(displayNext[2],displayNext[5]); //Switch cathodes to new digits
  delay(displayNextFade[0]/(dim?4:1)); //Display for fade-in cycles
  digitalWrite(anodes[0], LOW); //Turn off tubes
  
  if(dim) delay(fadeMax/1.5);
  
  //Anode channel 1: tubes #4 (sec x10) and #1 (hour x1)
  setCathodes(displayLast[4],displayLast[1]);
  digitalWrite(anodes[1], HIGH);
  delay(displayLastFade[1]/(dim?4:1));
  setCathodes(displayNext[4],displayNext[1]);
  delay(displayNextFade[1]/(dim?4:1));
  digitalWrite(anodes[1], LOW);
  
  if(dim) delay(fadeMax/1.5);
  
  //Anode channel 2: tubes #0 (hour x10) and #3 (min x1)
  setCathodes(displayLast[0],displayLast[3]);
  digitalWrite(anodes[2], HIGH);
  delay(displayLastFade[2]/(dim?4:1));
  setCathodes(displayNext[0],displayNext[3]);
  delay(displayNextFade[2]/(dim?4:1));
  digitalWrite(anodes[2], LOW);
  
  if(dim) delay(fadeMax*0.75);
  
  // Loop thru and update all the arrays, and fades.
  for( byte i = 0 ; i < 6 ; i ++ ) {
    if( displayNext[i] != displayLast[i] ) {
      displayNextFade[i] += fadeStep;
      displayLastFade[i] -= fadeStep;
  
      if( displayNextFade[i] >= fadeMax ){
        displayNextFade[i] = 0.0f;
        displayLastFade[i] = fadeMax;
        displayLast[i] = displayNext[i];
      }
    }
  }
} //end cycleDisplay()

void setCathodes(byte decValA, byte decValB){
  bool binVal[4]; //4-bit binary number with values [1,2,4,8]
  decToBin(binVal,decValA); //have binary value of decVal set into binVal
  for(byte i=0; i<4; i++) digitalWrite(binOutA[i],binVal[i]); //set bin inputs of SN74141
  decToBin(binVal,decValB);
  for(byte i=0; i<4; i++) digitalWrite(binOutB[i],binVal[i]); //set bin inputs of SN74141
} //end setCathodes()

void decToBin(bool binVal[], byte i){
  //binVal is a reference (modify in place) of a binary number bool[4] with values [1,2,4,8]
  if(i<0 || i>15) i=15; //default value, turns tubes off
  binVal[3] = int(i/8)%2;
  binVal[2] = int(i/4)%2;
  binVal[1] = int(i/2)%2;
  binVal[0] = i%2;
} //end decToBin()