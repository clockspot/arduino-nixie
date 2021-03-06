#include <arduino.h>
#include "arduino-clock.h"

#ifdef RTC_DS3231 //see arduino-clock.ino Includes section

#include "rtcDS3231.h"
#include <Wire.h> //Arduino - GNU LPGL - for I2C access to DS3231
#include <DS3231.h> //NorthernWidget - The Unlicense - install in your Arduino IDE

//RTC objects
DS3231 ds3231; //an object to access the ds3231 specifically (temp, etc)
RTClib rtc; //an object to access a snapshot of the ds3231 via rtc.now()
DateTime tod; //stores the rtc.now() snapshot for several functions to use
byte todW; //stores the day of week (read separately from ds3231 dow counter)

void rtcInit(){
  Wire.begin();
}
void rtcSetTime(byte h, byte m, byte s){
  ds3231.setHour(h);
  ds3231.setMinute(m);
  ds3231.setSecond(s);
  millisReset();
}
void rtcSetDate(int y, byte m, byte d, byte w){
  ds3231.setYear(y%100); //TODO: should we store century on our end? Per ds3231 docs, "The century bit (bit 7 of the month register) is toggled when the years register overflows from 99 to 00."
  ds3231.setMonth(m);
  ds3231.setDate(d);
  ds3231.setDoW(w+1); //ds3231 weekday is 1-index
}
void rtcSetHour(byte h){ //used for DST forward/backward
  ds3231.setHour(h);
}

void rtcTakeSnap(){
  //rtcGet functions pull from this snapshot - to ensure that code works off the same timestamp
  tod = rtc.now();
  todW = ds3231.getDoW()-1; //ds3231 weekday is 1-index
}
int  rtcGetYear(){ return tod.year(); }
byte rtcGetMonth(){ return tod.month(); }
byte rtcGetDate(){ return tod.day(); }
byte rtcGetWeekday(){ return todW; }
int  rtcGetTOD(){ return tod.hour()*60+tod.minute(); }
byte rtcGetHour(){ return tod.hour(); }
byte rtcGetMinute(){ return tod.minute(); }
byte rtcGetSecond(){ return tod.second(); }

byte rtcGetTemp(){ return ds3231.getTemperature()*100; }

#endif //RTC_DS3231