#include <arduino.h>
#include "arduino-clock.h"

#include "input.h"

//Needs access to RTC timestamps
#include "rtcDS3231.h"
#include "rtcMillis.h"

#define HOLDSET_SLOW_RATE 125
#define HOLDSET_FAST_RATE 20

//#include "Arduino.h" //not necessary, since these get compiled as part of the main sketch
#ifdef INPUT_UPDN_ROTARY
  #include <Encoder.h> //Paul Stoffregen - install in your Arduino IDE
  Encoder rot(CTRL_R1,CTRL_R2);  //TODO may need to reverse
#endif
#ifdef INPUT_IMU
  #include <Arduino_LSM6DS3.h>
  //If we don't already have inputs defined for Sel/Alt/Up/Dn, use some bogus ones
  #ifndef CTRL_SEL
    #define CTRL_SEL 100
  #endif
  #ifndef CTRL_ALT
    #define CTRL_ALT 101
  #endif
  #ifndef CTRL_UP
    #define CTRL_UP 102
  #endif
  #ifndef CTRL_DN
    #define CTRL_DN 103
  #endif
  //IMU "debouncing"
  int imuRoll = 0; //the state we're reporting (-1, 0, 1)
  int imuRollLast = 0; //when we saw it change
  int imuPitch = 0; //the state we're reporting (-1, 0, 1)
  int imuPitchLast = 0; //when we saw it change
  //int imuLastRead = 0; //for debug
  void readIMU(){
    float x, y, z;
    IMU.readAcceleration(x,y,z);
    int imuState;
    
    //Assumes Arduino is oriented with components facing back of clock, and USB port facing up. TODO add support for other orientations

    //Roll
    if((unsigned long)(millis()-imuRollLast)>=IMU_DEBOUNCING){ //don't check within a period from the last change
           if(y<=-0.5) imuState = 1;
      else if(y>= 0.5) imuState = -1;
      else if(y>-0.3 && y<0.3) imuState = 0;
      else imuState = imuRoll; //if it's borderline, treat it as "same"
      if(imuRoll != imuState){ imuRoll = imuState; imuRollLast = millis(); } //TODO maybe add audible feedback
    }

    //Pitch
    if((unsigned long)(millis()-imuPitchLast)>=IMU_DEBOUNCING){ //don't check within a period from the last change
           if(z<=-0.5) imuState = 1;
      else if(z>= 0.5) imuState = -1;
      else if(z>-0.3 && z<0.3) imuState = 0;
      else imuState = imuPitch; //if it's borderline, treat it as "same"
      if(imuPitch != imuState){ imuPitch = imuState; imuPitchLast = millis(); }
    }
    
  }
#endif

byte inputCur = 0; //Momentary button (or IMU position) currently in use - only one allowed at a time
byte inputCurHeld = 0; //Button hold thresholds: 0=none, 1=unused, 2=short, 3=long, 4=verylong, 5=superlong, 10=set by inputStop()

unsigned long inputLast = 0; //When an input last took place, millis()
int inputLastTODMins = 0; //When an input last took place, time of day. Used in paginated functions so they all reflect the time of day when the input happened.

bool initInputs(){
  //TODO are there no "loose" pins left floating after this? per https://electronics.stackexchange.com/q/37696/151805
  #ifdef INPUT_BUTTONS
    pinMode(CTRL_SEL, INPUT_PULLUP);
    if(CTRL_ALT>0){ //preprocessor directives don't seem to work for this when e.g. "A7"
      pinMode(CTRL_ALT, INPUT_PULLUP);
    }
    #ifdef INPUT_UPDN_BUTTONS
      pinMode(CTRL_UP, INPUT_PULLUP);
      pinMode(CTRL_DN, INPUT_PULLUP);
    #endif
  #endif
  #ifdef INPUT_UPDN_ROTARY
    //rotary needs no init here
  #endif
  #ifdef INPUT_IMU
    //if(!IMU.begin()){ Serial.println(F("Failed to initialize IMU!")); while(1); }
    IMU.begin();
    //Serial.println(F("IMU initialized"));
  #endif
  //Check to see if CTRL_SEL is held at init - facilitates version number display and EEPROM hard init
  delay(100); //prevents the below from firing in the event there's a capacitor stabilizing the input, which can read low falsely
  if(readBtn(CTRL_SEL)){ inputCur = CTRL_SEL; return true; }
  else return false;
}

bool readBtn(byte btn){
  //Reads momentary button and/or IMU position, as equipped
  //Returns true if one or both are "pressed"
  bool btnPressed = false;
  bool imuPressed = false;
  #ifdef INPUT_BUTTONS
    if(btn!=0){ //skip disabled alt
      if(btn==A6 || btn==A7) btnPressed = analogRead(btn)<100; //analog-only pins
      else btnPressed = !(digitalRead(btn)); //false (low) when pressed
    }
  #endif
  #ifdef INPUT_IMU
    switch(btn){
      //Assumes Arduino is oriented with components facing back of clock, and USB port facing up
      //TODO support other orientations
      case CTRL_SEL: imuPressed = imuPitch>0; break; //clock tilted dial up
      case CTRL_ALT: imuPressed = imuPitch<0; break; //clock tilted dial down
      case CTRL_DN:  imuPressed = imuRoll<0; break; //clock tilted left
      case CTRL_UP:  imuPressed = imuRoll>0; break; //clock tilted right
      default: break;
    }
  #endif
  return (btnPressed || imuPressed);
}

unsigned long holdLast;
void checkBtn(byte btn){
  //Polls for changes in momentary buttons (or IMU positioning), LOW = pressed.
  //When a button event has occurred, will call ctrlEvt in main code.
  //Only called by checkInputs() and only for inputs configured as button and/or IMU.
  bool bnow = readBtn(btn);
  unsigned long now = millis();
  //If the button has just been pressed, and no other buttons are in use...
  if(inputCur==0 && bnow) {
    // Serial.print(F("Btn "));
    // Serial.print(btn,DEC);
    // Serial.println(F(" pressed"));
    inputCur = btn; inputCurHeld = 0; inputLast = now; inputLastTODMins = rtcGetTOD();
    //Serial.println(); Serial.println(F("ich now 0 per press"));
    ctrlEvt(btn,1,inputCurHeld); //hey, the button has been pressed
    //Serial.print(F("ich now ")); Serial.print(inputCurHeld,DEC); Serial.println(F(" after press > ctrlEvt"));
  }
  //If the button is being held...
  if(inputCur==btn && bnow) {
    //If the button has passed a hold duration threshold... (ctrlEvt will only act on these for Sel/Alt)
    if((unsigned long)(now-inputLast)>=CTRL_HOLD_SUPERLONG_DUR && inputCurHeld < 5){
      ctrlEvt(btn,5,inputCurHeld); if(inputCurHeld<10) inputCurHeld = 5;
      //Serial.print(F("ich now ")); Serial.print(inputCurHeld,DEC); Serial.println(F(" after 5 hold > ctrlEvt"));
    }
    else if((unsigned long)(now-inputLast)>=CTRL_HOLD_VERYLONG_DUR && inputCurHeld < 4){
      ctrlEvt(btn,4,inputCurHeld); if(inputCurHeld<10) inputCurHeld = 4;
      //Serial.print(F("ich now ")); Serial.print(inputCurHeld,DEC); Serial.println(F(" after 4 hold > ctrlEvt"));
    }
    else if((unsigned long)(now-inputLast)>=CTRL_HOLD_LONG_DUR && inputCurHeld < 3){
      ctrlEvt(btn,3,inputCurHeld); if(inputCurHeld<10) inputCurHeld = 3;
      //Serial.print(F("ich now ")); Serial.print(inputCurHeld,DEC); Serial.println(F(" after 3 hold > ctrlEvt"));
    }
    else if((unsigned long)(now-inputLast)>=CTRL_HOLD_SHORT_DUR && inputCurHeld < 2) {
      //Serial.print(F("ich was ")); Serial.println(inputCurHeld,DEC);
      ctrlEvt(btn,2,inputCurHeld); if(inputCurHeld<10) inputCurHeld = 2;
      //Serial.print(F("ich now ")); Serial.print(inputCurHeld,DEC); Serial.println(F(" after 2 hold > ctrlEvt"));
      holdLast = now; //starts the repeated presses code going
    }
    //While Up/Dn are being held, send repeated presses to ctrlEvt
    #if defined(INPUT_UPDN_BUTTONS) || defined(INPUT_IMU)
      if((btn==CTRL_UP || btn==CTRL_DN) && inputCurHeld >= 2){
        if((unsigned long)(now-holdLast)>=(inputCurHeld>=3?HOLDSET_FAST_RATE:HOLDSET_SLOW_RATE)){ //could make it nonlinear?
          holdLast = now;
          ctrlEvt(btn,1,inputCurHeld);
        }
      }
    #endif
  }
  //If the button has just been released...
  if(inputCur==btn && !bnow) {
    inputCur = 0;
    //Only act if the button hasn't been stopped
    if(inputCurHeld<10) ctrlEvt(btn,0,inputCurHeld); //hey, the button was released after inputCurHeld
    //Serial.print(F("ich now ")); Serial.print(inputCurHeld,DEC); Serial.println(F(" then 0 after release > ctrlEvt"));
    inputCurHeld = 0;
  }
}
void inputStop(){
  //In some cases, when handling btn evt 1/2/3/4/5, we may call this so following events 2/3/4/5/0 won't cause unintended behavior (e.g. after a fn change, or going in or out of set)
  inputCurHeld = 10;
  //Serial.println(F("ich now 10 per inputStop"));
}

bool rotVel = 0; //high velocity setting (x10 rather than x1)
#ifdef INPUT_UPDN_ROTARY
unsigned long rotLastStep = 0; //timestamp of last completed step (detent)
int rotLastVal = 0;
void checkRot(){
  //Changes in rotary encoder. When rotation(s) occur, will call ctrlEvt to simulate btn presses. During setting, ctrlEvt will take rotVel into account.
  int rotCurVal = rot.read();
  if(rotCurVal!=rotLastVal){ //we've sensed a state change
    rotLastVal = rotCurVal;
    if(rotCurVal>=4 || rotCurVal<=-4){ //we've completed a step of 4 states (this library doesn't seem to drop states much, so this is reasonably reliable)
      unsigned long now = millis();
      inputLast = now; inputLastTODMins = rtcGetTOD();
      if((unsigned long)(now-rotLastStep)<=ROT_VEL_START) rotVel = 1; //kick into high velocity setting (x10)
      else if((unsigned long)(now-rotLastStep)>=ROT_VEL_STOP) rotVel = 0; //fall into low velocity setting (x1)
      rotLastStep = now;
      while(rotCurVal>=4) { rotCurVal-=4; ctrlEvt(CTRL_UP,1,inputCurHeld,rotVel); }
      while(rotCurVal<=-4) { rotCurVal+=4; ctrlEvt(CTRL_DN,1,inputCurHeld,rotVel); }
      rot.write(rotCurVal);
    }
  }
} //end checkRot()
#endif

void checkInputs(){
  //TODO potential issue: if user only means to rotate or push encoder but does both?
  #ifdef INPUT_IMU
    readIMU(); //captures IMU state for checkBtn/readBtn to look at
  #endif
  //checkBtn calls readBtn which will read button and/or IMU as equipped
  //We just need to only call checkBtn if one or the other is equipped
  #if defined(INPUT_BUTTONS) || defined(INPUT_IMU)
    checkBtn(CTRL_SEL);
    if(CTRL_ALT>0){ //preprocessor directives don't seem to work for this when e.g. "A7"
      checkBtn(CTRL_ALT);
    }
    #if defined(INPUT_UPDN_BUTTONS) || defined(INPUT_IMU)
      checkBtn(CTRL_UP);
      checkBtn(CTRL_DN);
    #endif
  #endif
  #ifdef INPUT_UPDN_ROTARY
    checkRot();
  #endif
}

void setInputLast(unsigned long increment){
  //Called when other code changes the displayed fn, as though the human user had done it
  //(which in many cases they did, but indirectly, such as via settings page - but also automatic date display)
  //If increment is specified, we just want to move inputLast up a bit
  if(increment) inputLast+=increment;
  //else we want to set both inputLast and the TODMins snapshot to now
  inputLast = millis(); inputLastTODMins = rtcGetTOD();
}
unsigned long getInputLast(){
  return inputLast;
}
int getInputLastTODMins(){
  //Used to ensure paged displays (e.g. calendar) use the same TOD for all pages
  return inputLastTODMins;
}