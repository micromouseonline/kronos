#include <Arduino.h>

int state = 1;
uint32_t last_pulse;

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    last_pulse = micros();
}

const uint32_t one_second = 998572;
void loop() {
    if (micros() - last_pulse < one_second) {
        return;
    }
    last_pulse += one_second;
    digitalWrite(LED_BUILTIN, 1);
    delay(100);
    digitalWrite(LED_BUILTIN, 0);
}
