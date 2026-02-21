#include "Controller.h"

Controller controller("RobotAP", "12345678");

void setup() {
  Serial.begin(115200);

  controller.configureL298N(9, 7, 6, 10, 5, 4);
  controller.setMotorMinPWM(110);
  controller.setFailsafeTimeoutMs(300);
  controller.enableStatusLED(LED_BUILTIN);

  controller.beginAP(true);


}

void loop() {
  controller.update();
}
