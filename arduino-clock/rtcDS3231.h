#ifndef RTC_DS3231_H
#define RTC_DS3231_H

//Mutually exclusive with other rtc options

void rtcInit();
void rtcSetTime(byte h, byte m, byte s);
void rtcSetDate(int y, byte m, byte d, byte w);
void rtcSetHour(byte h);

void rtcTakeSnap();

int  rtcGetYear();
byte rtcGetMonth();
byte rtcGetDate();
byte rtcGetWeekday();
int  rtcGetTOD();
byte rtcGetHour();
byte rtcGetMinute();
byte rtcGetSecond();

byte rtcGetTemp();

#endif