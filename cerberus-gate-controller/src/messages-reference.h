#include <Arduino.h>

// clang-format off
/***
Author: David Hannaford & Ian Butterworth  Date: 25 September 2017  Version: 6
Modified: Peter Harrison                   Date:  7 June      2022  Version: 7.0

Arduino and Visual Basic: Test code for mouse timing system
Timing performed by Arduino and data passed to VisualBasic PC software
Overall supervision performed by VisualBasic and passed to the Arduino

Messages in the format <message_type,value>CrLf

For example, the supervisor might sendRun
<98,0>
to start a new mouse

Any characters following the closing bracket are ignored by the supervisor

Implemented message types:
      ENCODED  MESSAGE TYPE      DIRECTION      TX FREQENCY     COMMENTS
   
   Status Messages to supervisor
      0        MSG_Watchdog      Arduino to PC  1000 msec       Sent every second with an incrementing value to check connection is active

      4        MSG_CURRENT_STATE Arduino to PC                  Value of timer state  Sent whenever state changes
                                                                  0  CALIBRATE   calibrate gates,
                                                                  1  WAITING     looking for mouse in start cell,
                                                                  2  ARMED       Mouse seen in start cell,
                                                                  3  STARTING    Run started (but not cleared start gate yet)
                                                                  4  RUNNING     Run in progress,
                                                                  5  GOAL        Run to centre completed (finish gate triggered)
                                                                  6  NEW_MOUSE   New Mouse
   Timing Messages to supervisor
      12       MSG_C1SplitTime   Arduino to PC  Event Driven    Time in milliseconds for the current mouse on its current run
                                                                (only sent as zero to start host counter)
      
      13       MSG_C1RunTime     Arduino to PC  Event Driven    Time in milliseconds for a run that has just completed
                                                                (definitive time used to calculate score time - sent twice)

      30       MSG_CourseTimeMs  Arduino to PC  Event Driven    Time in milliseconds that the current mouse has been active
                                                                in the maze (only sent as zero to reset host counter)




      98       MSG_NewMouse      PC to Arduino  Event Driven    A new mouse has been selected in the host application
                                                                (value argument will always be passed as 0)

      99       MSG_SetMode       PC to Arduino  Event Driven    Controls the Arduino mode
                                                                Valid values:
                                                                TIMER       (normal timing mode),
                                                                CALIBRATION (start returning calibration data)


   Not currently used
      71       MSG_STrigger      Arduino to PC  Event Driven    New value of Start Gate trigger (Valid values: 1, 0)
      72       MSG_FTrigger      Arduino to PC  Event Driven    New value of Finish Gate trigger (Valid values: 1, 0)
      73       MSG_CTrigger      Arduino to PC  Event Driven    New value of Mouse in Start Cell trigger (Valid values: 1,0)

   The following are only sent back from the gate controller for display as calibration values
      81       MSG_SGLevel       Arduino to PC  100 msec        Intensity level being received by Start Gate phototransistor
      82       MSG_SGPot         Arduino to PC  100 msec        Value read from Start Gate potentiometer
      83       MSG_FGLevel       Arduino to PC  100 msec        Intensity level being received by Finish Gate phototransistor
      84       MSG_FGPot         Arduino to PC  100 msec        Value read from Finish Gate potentiometer
      85       MSG_SCLevel       Arduino to PC  100 msec        Intensity level being received by Mouse in Start Cell phototransistor
      86       MSG_SCPot         Arduino to PC  100 msec        Value read from Mouse in Start Cell potentiometer
***/

// Message Valid Values
const int MSG_Watchdog       =  0;
const int MSG_CURRENT_STATE  =  4;
const int MSG_C1SplitTime    = 12;
const int MSG_C1RunTime      = 13;
const int MSG_CourseTimeMs   = 30;

const int MSG_NewMouse       = 98;
const int MSG_SetMode        = 99;

const int MSG_STrigger       = 71;
const int MSG_FTrigger       = 72;
const int MSG_CTrigger       = 73;

const int MSG_SGLevel        = 81;
const int MSG_SGPot          = 82;
const int MSG_FGLevel        = 83;
const int MSG_FGPot          = 84;
const int MSG_SCLevel        = 85;
const int MSG_SCPot          = 86;



extern char last_char;
// clang-format on

inline void send_message(int type, unsigned long value, const __FlashStringHelper *comment = nullptr) {
  Serial.print('<');
  Serial.print(type);
  Serial.print(',');
  Serial.print(value);
  Serial.print('>');
  if (type == MSG_CURRENT_STATE) {
    Serial.print(' ');
    Serial.print(last_char);
    last_char = '#';
  }
  Serial.print(comment);
  Serial.println();
}

void send_run_time(unsigned long time) {
  // Sent twice deliberately -- a RATS-side requirement (belt-and-braces
  // against a dropped message on their end), not a bug here. Probably not
  // strictly necessary, but harmless, so left as-is rather than second-
  // guessing the host system's own author.
  send_message(MSG_C1RunTime, time, F(" RUN TIME"));
  delay(20);
  send_message(MSG_C1RunTime, time, F(" RUN TIME"));
}
