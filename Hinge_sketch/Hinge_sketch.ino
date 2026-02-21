/*
 * Hinge_sketch.ino
 * Single DC motor hinge / shooter controller using the Controller library.
 *
 * WIRING (L298N Channel A → Arduino R4 WiFi)
 * ─────────────────────────────────────────────────────────────────────
 *  L298N Terminal   Arduino R4 WiFi Pin   Notes
 *  ─────────────────────────────────────────────────────────────────
 *  ENA              Pin 9                 PWM speed — connect to Arduino
 *  IN1              Pin 7                 Direction A
 *  IN2              Pin 6                 Direction B
 *  OUT1             Motor +               DC motor lead
 *  OUT2             Motor −               DC motor lead
 *  12V / VMotor     External 6V supply +  L298N drops ~2V; motor sees ~4V
 *  GND              Supply GND + Ard GND  Common ground is mandatory
 *
 * DIRECTION LOGIC (Channel A)
 * ─────────────────────────────────────────────────────────────────────
 *  IN1=HIGH, IN2=LOW,  ENA=PWM  →  Clockwise (CW)
 *  IN1=LOW,  IN2=HIGH, ENA=PWM  →  Counter-Clockwise (CCW)
 *  IN1=HIGH, IN2=HIGH, ENA=0    →  Brake (fast stop)
 *
 * USAGE
 * ─────────────────────────────────────────────────────────────────────
 *  1. Connect to AP "HingeAP" (password 12345678)
 *  2. Open http://10.0.0.2
 *  3. Drag throttle slider to set power (0–100%)
 *  4. Tap "Shoot" → hinge spins CW at that power for 800 ms then brakes
 *  5. Tap "Stop"  → immediate brake at any time
 *  6. Lose WiFi   → failsafe brakes after 1 second
 *
 * DESIGN NOTES
 * ─────────────────────────────────────────────────────────────────────
 *  - The joystick is present in the web UI but intentionally ignored here.
 *  - The throttle slider acts as the "power bar" for the Shoot action.
 *  - rotateFor() uses start+duration millis() pattern for rollover safety.
 *  - _minPWM (default 110) prevents stall at low speeds; tune per motor.
 *  - Tune the 800 ms duration in doShoot() for desired throw distance.
 */

#include "Controller.h"

// ─────────────────────────────────────────────────────────────────────
// Hinge class — single DC motor with timed rotation support
// ─────────────────────────────────────────────────────────────────────
class Hinge {
public:
    Hinge(uint8_t ena, uint8_t in1, uint8_t in2)
        : _ena(ena), _in1(in1), _in2(in2) {}

    // Call in setup() — sets pin modes and applies safe brake state
    void begin() {
        pinMode(_ena, OUTPUT);
        pinMode(_in1, OUTPUT);
        pinMode(_in2, OUTPUT);
        stop();  // safe state before WiFi starts
    }

    // Override stiction floor (default 110); call after begin()
    void setMinPWM(uint8_t pwm) { _minPWM = pwm; }

    // Rotate clockwise at speed 1–100
    void rotateCW(uint8_t speed = 100) {
        _timedActive = false;
        applyMotor(true, speed);
    }

    // Rotate counter-clockwise at speed 1–100
    void rotateCCW(uint8_t speed = 100) {
        _timedActive = false;
        applyMotor(false, speed);
    }

    // Brake (IN1=HIGH, IN2=HIGH, ENA=0) — stops faster than coast
    void stop() {
        _timedActive = false;
        digitalWrite(_in1, HIGH);
        digitalWrite(_in2, HIGH);
        analogWrite(_ena, 0);
    }

    // Non-blocking timed rotation; call update() in loop() to auto-stop
    void rotateFor(bool cw, uint8_t speed, uint32_t ms) {
        _timedStartMs = millis();
        _timedDurMs   = ms;
        _timedActive  = true;
        if (cw) rotateCW(speed);
        else    rotateCCW(speed);
        // Re-arm timed flag (rotateCW/CCW clears it first, set again here)
        _timedActive  = true;
    }

    // Call every loop() iteration — handles the rotateFor() timeout
    void update() {
        if (_timedActive && (millis() - _timedStartMs) >= _timedDurMs) {
            stop();
        }
    }

private:
    uint8_t  _ena, _in1, _in2;
    uint8_t  _minPWM      = 110;   // stiction floor — tune to motor

    bool     _timedActive  = false;
    uint32_t _timedStartMs = 0;
    uint32_t _timedDurMs   = 0;

    // Maps speed 0–100 → PWM 0–255 with _minPWM floor.
    // Mirrors Controller::setMotorOne() for consistent behaviour.
    void applyMotor(bool forward, uint8_t speed) {
        if (speed == 0) { stop(); return; }

        if (forward) {
            digitalWrite(_in1, HIGH);
            digitalWrite(_in2, LOW);
        } else {
            digitalWrite(_in1, LOW);
            digitalWrite(_in2, HIGH);
        }

        int pwm = map(constrain((int)speed, 0, 100), 0, 100, 0, 255);
        if (pwm > 0 && pwm < (int)_minPWM) pwm = (int)_minPWM;
        analogWrite(_ena, (uint8_t)pwm);
    }
};

// ─────────────────────────────────────────────────────────────────────
// Global instances
// ─────────────────────────────────────────────────────────────────────
Controller controller("HingeAP", "12345678");
Hinge      hinge(9, 7, 6);   // ENA=9, IN1=7, IN2=6

// ─────────────────────────────────────────────────────────────────────
// Button callbacks
// ─────────────────────────────────────────────────────────────────────

// Rotate CW at current throttle for 800 ms then brake.
// Drag the throttle slider before pressing Shoot to set power.
void doShoot() {
    uint8_t power = controller.getThrottle();  // 0–100 from slider
    hinge.rotateFor(true, power, 800);          // CW, power%, 800 ms
}

// ─────────────────────────────────────────────────────────────────────
// setup / loop
// ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    hinge.begin();
    hinge.setMinPWM(110);

    controller.setFailsafeTimeoutMs(1000);    // brake 1 s after WiFi loss
    controller.enableStatusLED(LED_BUILTIN);

    // Joystick drive is intentionally not configured here;
    // this sketch uses the throttle slider + buttons only.
    controller.registerButton("Shoot", doShoot);
    controller.registerButton("Stop",  []{ hinge.stop(); });

    controller.beginAP(true);
}

void loop() {
    controller.update();  // WiFi, failsafe, button callbacks
    hinge.update();       // handles rotateFor() / Shoot timeout
}
