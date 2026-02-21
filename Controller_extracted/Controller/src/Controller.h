//
// Created by Oscar Tesniere on 11/02/2026.
//

#ifndef THEFORGE2026_CONTROLLER_H
#define THEFORGE2026_CONTROLLER_H

#include <Arduino.h>
#include <WiFiS3.h>

class Controller {
public:
    Controller(const char* ssid, const char* password);

    // Start AP + HTTP server (optional debug flag for prints)
    bool beginAP(bool debug = false);
    void update();

    // Optional generic message callback
    void registerCallback(void (*callback)(const String&));

    // Optional: called whenever smoothed motor outputs change
    // (Still available even if L298N is configured internally)
    void registerDriveCallback(void (*callback)(int8_t left, int8_t right));

    // Smoothed motor outputs (-100..100)
    int8_t speedLeft() const;
    int8_t speedRight() const;

    uint8_t getThrottle() const;  // returns 0–100

    void setFailsafeTimeoutMs(uint16_t ms);

    // Register a button shown on the UI; callback called on press
    bool registerButton(const char* label, void (*cb)());
    void clearButtons();

    // -------- L298N integration (optional) --------
    // Call this before beginAP() to let the library drive motors automatically.
    void configureL298N(
        uint8_t ena, uint8_t in1, uint8_t in2,
        uint8_t enb, uint8_t in3, uint8_t in4
    );

    // Optional tuning for motor debug printing
    void setMotorDebugPrintIntervalMs(uint16_t ms);

  void enableStatusLED(uint8_t pin = LED_BUILTIN);

void setMotorMinPWM(uint8_t pwm);

private:

enum LedState {
    LED_BOOTING,
    LED_AP_READY,
    LED_CLIENT_CONNECTED,
    LED_FAILSAFE,
    LED_ERROR
};
    void printWiFiStatus() const;

    void handleClient(WiFiClient& client);
    String readRequestLine(WiFiClient& client);

    void sendHttpOk(WiFiClient& client, const char* contentType, const String& body);
    void sendHttpNotFound(WiFiClient& client);

    void handleRoot(WiFiClient& client);
    void handleDrive(WiFiClient& client, const String& requestLine);
    void handleBtn(WiFiClient& client, const String& requestLine);
    void handleControlMsg(WiFiClient& client, const String& requestLine);
    void handleHealth(WiFiClient& client);

    static bool extractQueryInt(const String& requestLine, const char* key, int& outValue);
    static int clampInt(int v, int lo, int hi);

    void applySmoothingAndNotify();

    // -------- L298N internals --------
    void motorInitSafeStop();
    void motorApply(int8_t left, int8_t right);
    void setMotorOne(uint8_t en, uint8_t inA, uint8_t inB, int8_t spd);
    static void speedToCmd(int8_t spd, bool &forward, uint8_t &pwm);
    void debugMotors(int8_t left, int8_t right);

    // --- WiFi debug helpers (enabled when beginAP(debug=true)) --- // removed CONST
    void debugWiFiScanForSSID() ;
    bool wifiSSIDExistsNearby() ;

// LED "hold" mechanism (non-blocking)
unsigned long _ledHoldUntilMs = 0;

void setLedStateHold(LedState s, uint16_t holdMs);
void setLedStateForce(LedState s); // bypass hold (for critical transitions)

// ---- LED status ----

void updateStatusLED();
void setLedState(LedState s);

uint8_t _ledPin = 255;
bool _ledEnabled = false;
LedState _ledState = LED_BOOTING;
unsigned long _ledTimer = 0;
bool _ledLevel = false;

private:

    uint8_t _lastThrottle = 100;  // last throttle received from /drive
    uint8_t _motorMinPWM = 0;

    const char* _ssid;
    const char* _password;

    WiFiServer _server{80};
    int _status = WL_IDLE_STATUS;


    void (*_onMessage)(const String&) = nullptr;
    void (*_onDrive)(int8_t left, int8_t right) = nullptr;

    // Network target (set by /drive)
    int8_t _cmdLeft  = 0;
    int8_t _cmdRight = 0;

    // Smoothed output (what you apply to motors)
    int8_t _outLeft  = 0;
    int8_t _outRight = 0;

    // Params for smoothing
    uint8_t _deadband = 6;        // +/-6 => treat as 0
    uint8_t _slewPerUpdate = 8;   // max change per update() call

    // Failsafe
    uint16_t _failsafeTimeoutMs = 1200;
    uint8_t _slewPerUpdateStop = 30;  // faster ramp-down
    unsigned long _lastDriveMs = 0;
    bool _failsafeStopped = false;

    // Button registry
    static constexpr uint8_t MAX_BUTTONS = 8;

    struct ButtonReg {
        String label;
        void (*cb)() = nullptr;
    };

    ButtonReg _buttons[MAX_BUTTONS];
    uint8_t _buttonCount = 0;

    // -------- L298N config --------
    bool _l298nEnabled = false;
    uint8_t _ena = 255, _in1 = 255, _in2 = 255;
    uint8_t _enb = 255, _in3 = 255, _in4 = 255;

    // Debug options (enabled via beginAP(debug=true))
    bool _debug = false;

    // Motor debug throttling
    uint16_t _motorDebugPrintMs = 150;
    int8_t _lastDbgL = 127;
    int8_t _lastDbgR = 127;
    unsigned long _lastDbgPrintMs = 0;
};

#endif // THEFORGE2026_CONTROLLER_H