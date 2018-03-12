// Code for Arduino Nano in RLB Designs IN-12/17 clock v5.0
// featuring timekeeping by DS1307 RTC and six digits multiplexed 3x2 via two SN74141 driver chips
// Originally written by Robin Birtles and Chris Gerekos based on http://arduinix.com/Main/Code/ANX-6Tube-Clock-Crossfade.txt
// Refactored and expanded by Luke McKenzie (luke@theclockspot.com)

// TODO: Alarm - display, set, sound, snooze, 24h silence
// TODO: Timer - display, set, run, sound, silence
// TODO: Cathode anti-poisoning
// TODO: implement other setup options

// notes on variable sizes on atmega
// char = int8_t, -128 to 127 (negatives stored via 2's complement).
// byte = uint8_t, 0 to 255
// int  = int16_t, -32768 to 32767. Watch out for bitshift right operator
// word = uint16_t, 0 to 65535 (or unsigned int)
// long = int32_t, -2147483648 to 2147483647.
// float, but use integer math when you can
// integer constant modifiers: use leading B for binary, 0 for octal, 0x for hex; trailing U for unsigned and/or L for long

#include <EEPROM.h>
#include <DS3231.h>
#include <Wire.h>
#include <ooPinChangeInt.h>
#include <AdaEncoder.h>


////////// Configuration consts //////////

// available clock functions, and unique IDs (between 0 and 200)
const byte fnIsTime = 0;
const byte fnIsDate = 1;
const byte fnIsAlarm = 2;
const byte fnIsTimer = 3;
const byte fnIsDayCount = 4;
const byte fnIsTemp = 5;
const byte fnIsCleaner = 6;
// functions enabled in this clock, in their display order. Only fnIsTime is required
const byte fnsEnabled[] = {fnIsTime, fnIsDate, fnIsTimer, fnIsDayCount, fnIsTemp, fnIsCleaner};

// These are the RLB board connections to Arduino analog input pins.
// S1/PL13 = Reset
// S2/PL5 = A1
// S3/PL6 = A0
// S4/PL7 = A6
// S5/PL8 = A3
// S6/PL9 = A2
// S7/PL14 = A7
// A6-A7 are analog-only pins that aren't quite as responsive and require a physical pullup resistor (1K to +5V), and can't be used with rotary encoders because they don't support pin change interrupts.

// What input is associated with each control?
const byte mainSel = A2; //main select button - must be equipped
const byte mainAdjUp = A1; //main up/down buttons or rotary encoder - must be equipped
const byte mainAdjDn = A0;
const byte altSel = 0; //alt select button - if unequipped, set to 0
const byte altAdjUp = 0; //A6; //alt up/down buttons or rotary encoder - if unequipped, set to 0
const byte altAdjDn = 0; //A3;

// What type of adj controls are equipped?
// 1 = momentary buttons. 2 = quadrature rotary encoder.
const byte mainAdjType = 1;
const byte altAdjType = 0; //if unquipped, set to 0

// In normal running mode, what do the controls do?
// assign -1 = nothing, -2 = cycle through functions, or a specific function per above
// TODO, one of them needs to do nothing for the alarm switch, right?
const char mainSelFn = -2;
const char mainAdjFn = -1;
// const byte altSelFn = -1;
// const byte altAdjFn = -1;

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
const byte alarmDur = 1;

const byte displaySize = 6; //number of tubes

// How long (in ms) are the button hold durations?
const word btnShortHold = 1000; //for setting the displayed feataure
const word btnLongHold = 3000; //for for entering options menu
const byte velThreshold = 100; //ms
// When an adj up/down input (btn or rot) follows another in less than this time, value will change more (10 vs 1).
// Recommend ~100 for rotaries. If you want to use this feature with buttons, extend to ~400.

//EEPROM locations for set values - default values are in initEEPROM()
//const byte alarmTimeLoc = 0; //and 1 (word) in minutes past midnight.
//const byte alarmOnLoc = 2; //byte
const byte dayCountYearLoc = 3; //and 4 (word)
const byte dayCountMonthLoc = 5; //byte
const byte dayCountDateLoc = 6; //byte

//EEPROM locations and default values for options menu
//Option numbers are 1-index, so arrays are padded to be 1-index too, for coding convenience. TODO change this
//Most vals (default, min, max) are 1-byte. In case of two-byte (max-min>255), high byte is loc, low byte is loc+1.
//options are offset to loc 16, to reserve 16 bytes of space for other set values per above
//       Option number: -  1  2  3  4  5  6  7  8  9  10 11   12   13 14 15 16   17   18
const byte optsLoc[] = {0,16,17,18,19,20,21,22,23,24, 25,27,  28,  30,32,33,34,  35,  37}; //EEPROM locs
const word optsDef[] = {0, 2, 1, 0, 0, 0, 0, 0, 0, 0,500, 0,1320, 360, 0, 1, 5, 480,1020};
const word optsMin[] = {0, 1, 1, 0, 0, 0, 0, 0, 0, 0,  1, 0,   0,   0, 0, 0, 0,   0,   0};
const word optsMax[] = {0, 2, 2, 2, 1,50, 1, 6, 4,60,999, 2,1439,1439, 2, 6, 6,1439,1439};


////////// Other global consts and vars used in multiple sections //////////

DS3231 ds3231; //an object to access the ds3231 specifically (temp, etc)
RTClib rtc; //an object to access a snapshot of the ds3231 rtc via now()
DateTime tod; //stores the now() snapshot for several functions to use
byte toddow; //stores the day of week as calculated from tod

// Hardware inputs and value setting
//AdaEncoder mainRot;
//AdaEncoder altRot;
byte btnCur = 0; //Momentary button currently in use - only one allowed at a time
byte btnCurHeld = 0; //Button hold thresholds: 0=none, 1=unused, 2=short, 3=long, 4=set by btnStop()
unsigned long inputLast = 0; //When a button was last pressed
unsigned long inputLast2 = 0; //Second-to-last of above

const byte fnOpts = 201; //fn values from here to 255 correspond to options in the options menu
byte fn = fnIsTime; //currently displayed fn, as above
byte fnSetPg = 0; //whether this function is currently being set, and which option/page it's on
word fnSetVal; //the value currently being set, if any - unsigned int 0-65535
word fnSetValMin; //min possible - unsigned int
word fnSetValMax; //max possible - unsigned int
bool fnSetValVel; //whether it supports velocity setting (if max-min > 30)
word fnSetValDate[3]; //holder for newly set date, so we can set it in 3 stages but set the RTC only once

// Volatile running values
word alarmTime = 0; //alarm time of day TODO move these to alarmTimeLoc and alarmOnLoc
bool alarmOn = 0; //alarm switch
word soundRemain = 0; //alarm/timer sound timeout counter, seconds
word snoozeRemain = 0; //snooze timeout counter, seconds
word timerInitial = 0; //timer original setting, seconds - up to 18 hours (64,800 seconds - fits just inside a word)
word timerRemain = 0; //timer actual counter
byte displayNext[6] = {15,15,15,15,15,15}; //Internal representation of display. Blank to start. Change this to change tubes.


////////// Main code control //////////

void setup(){
  Serial.begin(9600);
  Wire.begin();
  initOutputs();
  initInputs();
  if(!enableSoftAlarmSwitch) alarmOn=1; //TODO test and get rid of
  if(readInput(mainSel)==LOW) initEEPROM();
}

unsigned long pollLast = 0;
void loop(){
  unsigned long now = millis();
  //Things done every 50ms - avoids overpolling(?) and switch bounce(?)
  if(pollLast<now+50) {
    pollLast=now;
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
  //rotary encoder init
  if(mainAdjType==2) AdaEncoder mainRot = AdaEncoder('a',mainAdjUp,mainAdjDn);
  //if(altAdjType==2) AdaEncoder altRot = AdaEncoder('b',altAdjUp,altAdjDn);
}

void checkInputs(){
  // TODO can all this if/else business be defined at load instead of evaluated every sample? OR is it compiled that way?
  //potential issue: if user only means to rotate or push encoder but does both?

  //sample button states
  checkBtn(mainSel); //main select
  if(mainAdjType==1) { checkBtn(mainAdjUp); checkBtn(mainAdjDn); } //main adjust buttons (if equipped)
  //if(altSel!=0) checkBtn(altSel); //alt select (if equipped)
  //if(altAdjType==1) { checkBtn(altAdjUp); checkBtn(altAdjDn); } //alt adjust buttons (if equipped)
  
  //check adaencoder library to see if rot(s) have moved
  if(mainAdjType==2 || altAdjType==2) checkRot();
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

bool stoppingSound = false; //Special stuff (snooze canceling) happens right after a press that silences the sound
void ctrlEvt(byte ctrl, byte evt){
  //Handle control events (from checkBtn or checkRot), based on current fn and set state.
  //evt: 1=press, 2=short hold, 3=long hold, 0=release.
  //We only handle press evts for adj ctrls, as that's the only evt encoders generate.
  //But we can handle short and long holds and releases for the sel ctrls (always buttons).
  //TODO needs alt handling
  
  //Before all else, is it a press while the beeper is sounding? Silence it
  if(soundRemain>0 && evt==1){
    stoppingSound = true;
    soundRemain = 0;
    noTone(10);
    //If we're displaying the clock (as alarm trigger does), start snooze. 0 will have no effect
    if(fn==fnIsTime) snoozeRemain = readEEPROM(optsLoc[9],false)*60;
    return;
  }
  //After pressing to silence, short hold cancels a snooze; ignore other btn evts
  if(stoppingSound){
    stoppingSound = false;
    if(evt==2 && snoozeRemain>0) {
      snoozeRemain = 0;
      tone(10, 3136, 100); //G7
    }
    btnStop();
    return;
  }
  
  if(fn < fnOpts) { //normal fn running/setting (not in options menu)

    if(evt==3 && ctrl==mainSel) { //mainSel long hold: enter options menu
      btnStop();
      fn = fnOpts;
      clearSet(); //don't need updateDisplay() here because this calls updateRTC with force=true
      return;
    }
    
    if(!fnSetPg) { //fn running
      if(evt==2 && ctrl==mainSel) { //mainSel hold: enter setting mode
        switch(fn){
          case fnIsTime: //set mins
            startSet((tod.hour()*60)+tod.minute(),0,1439,1); break;
          case fnIsDate: //set year
            fnSetValDate[1]=tod.month(), fnSetValDate[2]=tod.day(); startSet(tod.year(),0,9999,1); break;
          case fnIsAlarm: //set mins
            //DS3231_get_a1(&buff[0], 59); //TODO write a wrapper function for this
            //startSet(buff,0,1439,1); //alarm: set mins
            break;
          case fnIsTimer: //set mins, up to 18 hours (64,800 seconds - fits just inside a word)
            if(timerRemain>0) { timerRemain = 0; btnStop(); updateDisplay(); break; } //If the timer is running, zero it out.
            startSet(timerInitial/60,0,1080,1); break;
          case fnIsDayCount: //set year like date, but from eeprom like startOpt
            startSet(readEEPROM(dayCountYearLoc,true),2000,9999,1); break;
          case fnIsTemp:
            break; //nothing - or is this where we do the calibration? TODO
          default: break;
        }
        //showSitch();
        return;
      }
      else if((ctrl==mainSel && evt==0) || ((ctrl==mainAdjUp || ctrl==mainAdjDn) && evt==1)) { //sel release or adj press - switch fn, depending on config
        //-1 = nothing, -2 = cycle through functions, other = go to specific function (see fn)
        //we can't handle sel press here because, if attempting to enter setting mode, it would switch the fn first
        bool fnChgd = false;
        if(ctrl==mainSel){
          if(mainSelFn!=-1) { //do a function switch
            fnChgd = true;
            if(mainSelFn==-2) fnScroll(1); //Go to next fn in the cycle
            else fn = mainSelFn;
          } else if(fn==fnIsAlarm) switchAlarm(0); //switch alarm
        }
        else if(ctrl==mainAdjUp || ctrl==mainAdjDn) {
          if(mainAdjFn!=-1) { //do a function switch
            fnChgd = true;
            if(mainAdjFn==-2) fnScroll(ctrl==mainAdjUp?1:-1); //Go to next or previous fn in the cycle
            else fn = mainAdjFn;
          } else if(fn==fnIsAlarm) switchAlarm(ctrl==mainAdjUp?1:-1); //switch alarm
        }
        if(fnChgd){
          switch(fn){
            //static ones
            case fnIsAlarm: case fnIsTemp:
              updateDisplay(); break;
            //"ticking" ones
            default: checkRTC(true); break;
          }
        }
      }
    } //end fn running

    else { //fn setting
      if(evt==1) { //we respond only to press evts during fn setting
        //TODO could we do release/shorthold on mainSel so we can exit without making changes?
        if(ctrl==mainSel) { //mainSel push: go to next option or save and exit setting mode
          btnStop(); //not waiting for mainSelHold, so can stop listening here
          //We will set ds3231 time parts directly
          //con: potential for very rare clock rollover while setting; pro: can set date separate from time
          switch(fn){
            case fnIsTime: //save in RTC
              ds3231.setHour(fnSetVal/60);
              ds3231.setMinute(fnSetVal%60);
              ds3231.setSecond(0);
              clearSet(); break;
            case fnIsDate: //save in RTC
              switch(fnSetPg){
                case 1: //save year, set month
                  fnSetValDate[0]=fnSetVal;
                  startSet(fnSetValDate[1],1,12,2); break; 
                case 2: //save month, set date
                  fnSetValDate[1]=fnSetVal;
                  startSet(fnSetValDate[2],1,daysInMonth(fnSetValDate[0],fnSetValDate[1]),3); break;
                case 3: //write year/month/date to RTC
                  ds3231.setYear(fnSetValDate[0]%100); //TODO: do we store century on our end? Per ds3231 docs, "The century bit (bit 7 of the month register) is toggled when the years register overflows from 99 to 00."
                  ds3231.setMonth(fnSetValDate[1]);
                  ds3231.setDate(fnSetVal);
                  ds3231.setDoW(dayOfWeek(fnSetValDate[0],fnSetValDate[1],fnSetVal)+1); //ds3231 weekday is 1-index
                  clearSet(); break;
                default: break;
              } break;
            case fnIsAlarm:
              break;
            case fnIsTimer: //timer
              timerInitial = fnSetVal*60; //timerRemain is seconds, but timer is set by minute
              timerRemain = timerInitial; //set actual timer going
              clearSet();
              break;
            case fnIsDayCount: //set like date, save in eeprom like finishOpt
              switch(fnSetPg){
                case 1: //save year, set month
                  writeEEPROM(dayCountYearLoc,fnSetVal,true);
                  startSet(readEEPROM(dayCountMonthLoc,false),1,12,2); break;
                case 2: //save month, set date
                  writeEEPROM(dayCountMonthLoc,fnSetVal,false);
                  startSet(readEEPROM(dayCountDateLoc,false),1,daysInMonth(fnSetValDate[0],fnSetValDate[1]),3); break;
                case 3: //save date
                  writeEEPROM(dayCountDateLoc,fnSetVal,false);
                  clearSet(); break;
                default: break;
              } break;
              break;
            case fnIsTemp:
              break;
            default: break;
          } //end switch fn
        } //end mainSel push
        if(ctrl==mainAdjUp) doSet(inputLast-inputLast2<velThreshold ? 10 : 1);
        if(ctrl==mainAdjDn) doSet(inputLast-inputLast2<velThreshold ? -10 : -1);
        //showSitch();
      } //end if evt==1
    } //end fn setting
    
  } //end normal fn running/setting
  
  else { //options menu setting - to/from EEPROM
    
    if(evt==2 && ctrl==mainSel) { //mainSel short hold: exit options menu
      btnStop();
      //if we were setting a value, writes setting val to EEPROM if needed
      if(fnSetPg) writeEEPROM(optsLoc[fnSetPg],fnSetVal,optsMax[fnSetPg]-optsMin[fnSetPg]>255?true:false);
      fn = fnIsTime;
      clearSet();
      return;
    }
    
    if(!fnSetPg){ //viewing option number
      if(ctrl==mainSel && evt==0) { //mainSel release: enter option value setting
        byte n = fn-fnOpts+1; //For a given options menu option (1-index), read from EEPROM and call startSet
        startSet(readEEPROM(optsLoc[n],optsMax[n]-optsMin[n]>255?true:false),optsMin[n],optsMax[n],n);
      }
      if(ctrl==mainAdjUp && evt==1) fnOptScroll(1); //next one up or cycle to beginning
      if(ctrl==mainAdjDn && evt==1) fnOptScroll(-1); //next one down or cycle to end?
      updateDisplay();
    } //end viewing option number

    else { //setting option value
      if(ctrl==mainSel && evt==0) { //mainSel release: save and exit option value setting
        //Writes setting val to EEPROM if needed
        writeEEPROM(optsLoc[fnSetPg],fnSetVal,optsMax[fnSetPg]-optsMin[fnSetPg]>255?true:false);
        clearSet();
      }
      if(ctrl==mainAdjUp && evt==1) doSet(inputLast-inputLast2<velThreshold ? 10 : 1);
      if(ctrl==mainAdjDn && evt==1) doSet(inputLast-inputLast2<velThreshold ? -10 : -1);
      updateDisplay();
    }  //end setting option value
  } //end options menu setting
  
} //end ctrlEvt

void fnScroll(char dir){
  //Switch to the next (1) or previous (-1) fn in fnsEnabled
  byte pos;
  byte posLast = sizeof(fnsEnabled)-1;
  if(dir==1) for(pos=0; pos<=posLast; pos++) if(fnsEnabled[pos]==fn) { fn = (pos==posLast?0:fnsEnabled[pos+1]); break; }
  if(dir==-1) for(pos=posLast; pos>=0; pos--) if(fnsEnabled[pos]==fn) { fn = (pos==0?posLast:fnsEnabled[pos-1]); break; }
}
void fnOptScroll(char dir){
  //Switch to the next options fn between min (fnOpts) and max (fnOpts+sizeof(optsLoc)-1) (inclusive)
  byte posLast = fnOpts+sizeof(optsLoc)-1;
  if(dir==1) fn = (fn==posLast? fnOpts: fn+1);
  if(dir==-1) fn = (fn==fnOpts? posLast: fn-1);
}

void startSet(word n, word m, word x, byte p){ //Enter set state at page p, and start setting a value
  fnSetVal=n; fnSetValMin=m; fnSetValMax=x; fnSetValVel=(x-m>30?1:0); fnSetPg=p;
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
    if(fnSetPg!=0 && ((mainAdjType==1 && (btnCur==mainAdjUp || btnCur==mainAdjDn)) || (altAdjType==1 && (btnCur==altAdjUp || btnCur==altAdjDn))) ){ //if we're setting, and this is an adj input for which the type is button
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
  checkRTC(true); //force an update to tod and updateDisplay()
}

//EEPROM values are exclusively bytes (0-255) or words (unsigned ints, 0-65535)
//If it's a word, high byte is in loc, low byte is in loc+1
void initEEPROM(){
  //Set EEPROM and clock to defaults
  //First prevent the held button from doing anything else
  btnCur = mainSel; btnStop();
  //Set the clock
  ds3231.setYear(18);
  ds3231.setMonth(1);
  ds3231.setDate(1);
  ds3231.setDoW(1); //2018-01-01 is Monday. DS3231 will keep count from here
  ds3231.setHour(0);
  ds3231.setMinute(0);
  ds3231.setSecond(0);
  //Set the default values that aren't part of the options menu
  //writeEEPROM(alarmTimeLoc,420,true); //7am - word
  //writeEEPROM(alarmOnLoc,enableSoftAlarmSwitch==0?1:0,false); //off, or on if no software switch spec'd
  writeEEPROM(dayCountYearLoc,2018,true);
  writeEEPROM(dayCountMonthLoc,1,false);
  writeEEPROM(dayCountDateLoc,1,false);
  //then the options menu defaults
  for(byte i=1; i<=sizeof(optsLoc)-1; i++) writeEEPROM(optsLoc[i],optsDef[i],optsMax[i]-optsMin[i]>255?true:false); //options menu
}
word readEEPROM(int loc, bool isWord){
  if(isWord) {
    word w = (EEPROM.read(loc)<<8)+EEPROM.read(loc+1); return w; //word / unsigned int, 0-65535
  } else {
    byte b = EEPROM.read(loc); return b; //byte, 0-255
  }
}
void writeEEPROM(int loc, int val, bool isWord){
  if(isWord) {
    Serial.print(F("EEPROM write word:"));
    if(EEPROM.read(loc)!=highByte(val)) {
      EEPROM.write(loc,highByte(val));
      Serial.print(F(" loc ")); Serial.print(loc,DEC);
      Serial.print(F(" val ")); Serial.print(highByte(val),DEC);
    } else Serial.print(F(" loc ")); Serial.print(loc,DEC); Serial.print(F(" unchanged (no write)."));
    if(EEPROM.read(loc+1)!=lowByte(val)) {
      EEPROM.write(loc+1,lowByte(val));
      Serial.print(F(" loc ")); Serial.print(loc+1,DEC);
      Serial.print(F(" val ")); Serial.print(lowByte(val),DEC);
    } else Serial.print(F(" loc ")); Serial.print(loc+1,DEC); Serial.print(F(" unchanged (no write)."));
  } else {
    Serial.print(F("EEPROM write byte:")); Serial.print(F(" loc ")); Serial.print(loc,DEC);
    if(EEPROM.read(loc)!=val) { EEPROM.write(loc,val);
      Serial.print(F(" val ")); Serial.print(val,DEC);
    } else Serial.print(F(" unchanged (no write)."));
  }
  Serial.println();
}

byte daysInMonth(word y, byte m){
  if(m==2) return (y%4==0 && (y%100!=0 || y%400==0) ? 29 : 28);
  //https://cmcenroe.me/2014/12/05/days-in-month-formula.html
  else return (28 + ((m + (m/8)) % 2) + (2 % m) + (2 * (1/m)));
}
//The following are for use with the Day Counter feature - find number of days between now and target date
//I don't know how reliable the unixtime() is from RTClib past 2038, plus there might be timezone complications
//so we'll just calculate the number of days since 2000-01-01
long dateToDayCount(word y, byte m, byte d){
  long dc = (y-2000)*365; //365 days for every full year since 2000
  word i; for(i=0; i<y-2000; i++) if(i%4==0 && (i%100!=0 || i%400==0)) dc++; //+1 for every feb 29th in those years
  for(i=1; i<m; i++) dc += daysInMonth(y,i); //add every full month since start of this year
  dc += d-1; //every full day since start of this month
  return dc;
}
byte dayOfWeek(word y, byte m, byte d){
  //DS3231 doesn't really calculate the day of the week, it just keeps a counter.
  //When setting date, we'll calculate per https://en.wikipedia.org/wiki/Zeller%27s_congruence
  byte yb = y%100; //2-digit year
  byte ya = y/100; //century
  //For this formula, Jan and Feb are considered months 11 and 12 of the previous year.
  //So if it's Jan or Feb, add 10 to the month, and set back the year and century if applicable
  if(m<3) { m+=10; if(yb==0) { yb=99; ya-=1; } else yb-=1; }
  else m -= 2; //otherwise subtract 2 from the month
  return (d + ((13*m-1)/5) + yb + (yb/4) + (ya/4) + 5*ya) %7;
}


////////// Clock ticking and timed event triggering //////////
byte rtcSecLast = 61;
void checkRTC(bool force){
  //Checks display timeouts;
  //checks for new time-of-day second -> decrements timers and checks for timed events;
  //updates display for "running" functions.
  
  //Things to do every time this is called: timeouts to reset display. These may force a tick.
  if(pollLast > inputLast){ //don't bother if the last input (which may have called checkRTC) was more recent than poll
    //Option/setting timeout: if we're in the options menu, or we're setting a value
    if(fnSetPg || fn>=fnOpts){
      if(pollLast-inputLast>120000) { fnSetPg = 0; fn = fnIsTime; force=true; } //Time out after 2 mins
    }
    //Temporary-display mode timeout: if we're *not* in a permanent one (time, day counter, or running timer)
    else if(fn!=fnIsTime && fn!=fnIsCleaner && fn!=fnIsDayCount && !(fn==fnIsTimer && (timerRemain>0 || soundRemain>0))){
      if(pollLast>inputLast+5000) { fnSetPg = 0; fn = fnIsTime; force=true; }
    }
  }
  
  //Update things based on RTC
  tod = rtc.now();
  toddow = ds3231.getDoW()-1; //ds3231 weekday is 1-index
  
  if(rtcSecLast != tod.second() || force) { //If it's a new RTC second, or we are forcing it
    
    if(force && rtcSecLast != tod.second()) force=false; //in the odd case it's BOTH, recognize the natural second
    rtcSecLast = tod.second();
    
    if(tod.second()==0) { //at top of minute
      //at 2am, check for DST change
      if(tod.minute()==0 && tod.hour()==2) autoDST();
      //check if we should trigger the alarm - TODO weekday limiter
      if(tod.hour()*60+tod.minute()==alarmTime && alarmOn && false) {
        fnSetPg = 0; fn = fnIsTime; soundRemain = alarmDur*60;
      }
      //checkDigitCycle();
    }
    if(tod.second()==30 && fn==fnIsTime && fnSetPg==0) { //At bottom of minute, on fn time (not setting), maybe show date
      if(readEEPROM(optsLoc[3],false)==2) { fn = fnIsDate; inputLast = pollLast; }
    }
    
    //Things to do every natural second (decrementing real-time counters)
    if(!force) {
      //If timer has time on it, decrement and trigger beeper if we reach zero
      if(timerRemain>0) {
        timerRemain--;
        if(timerRemain<=0) { fnSetPg = 0; fn = fnIsTimer; inputLast = pollLast; soundRemain = alarmDur*60; } //TODO radio mode
      }
      //If beeper has time on it, decrement and sound the beeper for 1/2 second
      if(soundRemain>0) {
        soundRemain--;
        //tone(10, 1568, 500); //G6
        tone(10, 1760, 500); //A6
        //tone(10, 1976, 500); //B6
        //tone(10, 2093, 500); //C7
      }
      //If alarm snooze has time on it, decrement and trigger beeper if we reach zero (and alarm is still on)
      if(snoozeRemain>0) {
        snoozeRemain--;
        if(snoozeRemain<=0 && alarmOn) { fnSetPg = 0; fn = fnIsTime; soundRemain = alarmDur*60; }
      }
    } //end natural second
    
    //Finally, whether natural tick or not, if we're not setting anything, update the display
    if(fnSetPg==0) updateDisplay();
    
  } //end if force or new second
} //end checkRTC()

bool fellBack = false;
void autoDST(){
  //Call daily when clock reaches 2am.
  //If rule matches, will set to 3am in spring, 1am in fall (and set fellBack so it only happens once)
  if(fellBack) { fellBack=false; return; } //If we fell back at last 2am, do nothing.
  if(toddow==0) { //is it sunday? currently all these rules fire on Sundays only
    switch(readEEPROM(optsLoc[7],false)){
      case 1: //second Sunday in March to first Sunday in November (US/CA)
        if(tod.month()==3 && tod.day()>=8 && tod.day()<=14) setDST(1);
        if(tod.month()==11 && tod.day()<=7) setDST(-1);
        break;
      case 2: //last Sunday in March to last Sunday in October (UK/EU)
        if(tod.month()==3 && tod.day()>=25) setDST(1);
        if(tod.month()==10 && tod.day()>=25) setDST(-1);
        break;
      case 3: //first Sunday in April to last Sunday in October (MX)
        if(tod.month()==4 && tod.day()<=7) setDST(1);
        if(tod.month()==10 && tod.day()>=25) setDST(-1);
        break;
      case 4: //last Sunday in September to first Sunday in April (NZ)
        if(tod.month()==9 && tod.day()>=24) setDST(1); //30 days hath September: last Sun will be 24-30
        if(tod.month()==4 && tod.day()<=7) setDST(-1);
        break;
      case 5: //first Sunday in October to first Sunday in April (AU)
        if(tod.month()==10 && tod.day()<=7) setDST(1);
        if(tod.month()==4 && tod.day()<=7) setDST(-1);
        break;
      case 6: //third Sunday in October to third Sunday in February (BZ)
        if(tod.month()==10 && tod.day()>=15 && tod.day()<=21) setDST(1);
        if(tod.month()==2 && tod.day()>=15 && tod.day()<=21) setDST(-1);
        break;
      default: break;
    } //end setting switch
  } //end is it sunday
}
void setDST(char dir){
  if(dir==1) ds3231.setHour(3); //could set relatively if we move away from 2am-only sets
  if(dir==-1 && !fellBack) { ds3231.setHour(1); fellBack=true; }
}

void switchAlarm(char dir){
  if(enableSoftAlarmSwitch){
    if(dir==1) alarmOn=1;
    if(dir==-1) alarmOn=0;
    if(dir==0) alarmOn = !alarmOn;
  }
}

////////// Display data formatting //////////
void updateDisplay(){
  //Run as needed to update display when the value being shown on it has changed
  //This formats the new value and puts it in displayNext[] for cycleDisplay() to pick up
  if(fnSetPg) { //setting value
    // //little tubes:
    // if(fn==fnOpts) editDisplay(fnSetPg, 4, 5, false); //options menu: current option key
    // else //fn setting: blank
    blankDisplay(4, 5);
    //big tubes:
    if(fnSetValMax==1439) { //Time of day (0-1439 mins, 0:00–23:59): show hrs/mins
      editDisplay(fnSetVal/60, 0, 1, EEPROM.read(optsLoc[4])); //hours with leading zero
      editDisplay(fnSetVal%60, 2, 3, true);
    } else if(fnSetValMax==1080) { //Timer duration (0-1080 mins, up to 18:00): show hrs/mins w/minimal leading
      if(fnSetVal>=60) editDisplay(fnSetVal/60, 0, 1, false); else blankDisplay(0,1); //hour only if present, else blank
      editDisplay(fnSetVal%60, 2, 3, (fnSetVal>=60?true:false)); //leading zero only if hour present
      editDisplay(0,4,5,true); //placeholder seconds
    } else editDisplay(fnSetVal, 0, 3, false); //some other type of value
  }
  else if(fn >= fnOpts){ //options menu, but not setting a value
    editDisplay(fn-fnOpts+1,0,1,0); //display option number on hour tubes
    blankDisplay(2,5);
  } else { //fn running
    switch(fn){
      case fnIsTime:
      byte hr; hr = tod.hour();
        if(readEEPROM(optsLoc[1],false)==1) hr = (hr==0?12:(hr>12?hr-12:hr));
        editDisplay(hr, 0, 1, readEEPROM(optsLoc[4],false));
        editDisplay(tod.minute(), 2, 3, true);
        if(EEPROM.read(optsLoc[3])==1) editDisplay(tod.day(), 4, 5, EEPROM.read(optsLoc[4])); //date
        else editDisplay(tod.second(), 4, 5, true); //seconds
        break;
      case fnIsDate:
      editDisplay(EEPROM.read(optsLoc[2])==1?tod.month():tod.day(), 0, 1, EEPROM.read(optsLoc[4]));
        editDisplay(EEPROM.read(optsLoc[2])==1?tod.day():tod.month(), 2, 3, EEPROM.read(optsLoc[4]));
        blankDisplay(4, 4);
        editDisplay(toddow, 5, 5, false); //TODO is this 0=Sunday, 6=Saturday?
        break;
      case fnIsDayCount:
        long targetDayCount; targetDayCount = dateToDayCount(
          readEEPROM(dayCountYearLoc,true),
          readEEPROM(dayCountMonthLoc,false),
          readEEPROM(dayCountDateLoc,false)
        );
        long currentDayCount; currentDayCount = dateToDayCount(tod.year(),tod.month(),tod.day());
        editDisplay(abs(targetDayCount-currentDayCount),0,3,false);
        //TODO for now don't indicate negative. Elsewhere we use leading zeros to represent negative but I don't like how that looks here
        blankDisplay(4,5);
        break;
      case fnIsAlarm: //alarm
        //TODO this isn't a real display
        editDisplay(0,0,1,EEPROM.read(optsLoc[4])); //hrs
        editDisplay(0,2,3,true); //mins
        editDisplay(0,4,5,false); //status
        break;
      case fnIsTimer: //timer - display time left.
        //Relative unit positioning: when t <1h, display min/sec in place of hr/min on 4-tube displays
        byte mspos; mspos = (displaySize<6 && timerRemain<3600? 0 : 2);
        if(timerRemain >= 3600) { //t >=1h: display hr on first two tubes
          editDisplay(timerRemain/3600,0,1,false);
        } else blankDisplay(0,1);
        if(timerRemain >= 60) { //t >=1m: display minute (in relative position). Leading zero if t >=1h.
          editDisplay((timerRemain%3600)/60,mspos,mspos+1,(timerRemain>=3600?true:false));
        } else blankDisplay(mspos,mspos+1);
        //display second (in relative position). Leading zero if t>=1m.
        editDisplay(timerRemain%60,mspos+2,mspos+3,(timerRemain>=60?true:false));
        break;
      case fnIsTemp: //thermometer
        int temp; temp = ds3231.getTemperature()*100;
        editDisplay(abs(temp)/100,1,3,(temp<0?true:false)); //leading zeros if negative
        editDisplay(abs(temp)%100,4,5,true);
        break;
      case fnIsCleaner:
        editDisplay(tod.second(),0,0,true);
        editDisplay(tod.second(),1,1,true);
        editDisplay(tod.second(),2,2,true);
        editDisplay(tod.second(),3,3,true);
        editDisplay(tod.second(),4,4,true);
        editDisplay(tod.second(),5,5,true);
      default: break;
    }//end switch
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
  if(fnSetPg>0) { //but if we're setting, dim for every other 500ms since we started setting
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