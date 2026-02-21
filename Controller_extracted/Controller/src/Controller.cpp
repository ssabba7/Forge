//
// Created by Oscar Tesniere on 11/02/2026.
//

#include "Controller.h"

Controller::Controller(const char* ssid, const char* password)
    : _ssid(ssid), _password(password) {}

bool Controller::registerButton(const char* label, void (*cb)()) {
    if (_buttonCount >= MAX_BUTTONS) return false;
    _buttons[_buttonCount].label = label;
    _buttons[_buttonCount].cb = cb;
    _buttonCount++;
    return true;
}

void Controller::clearButtons() {
    _buttonCount = 0;
}

void Controller::registerCallback(void (*callback)(const String&)) {
    _onMessage = callback;
}

void Controller::registerDriveCallback(void (*callback)(int8_t left, int8_t right)) {
    _onDrive = callback;
}

void Controller::setFailsafeTimeoutMs(uint16_t ms) {
    _failsafeTimeoutMs = ms;
}

void Controller::configureL298N(
    uint8_t ena, uint8_t in1, uint8_t in2,
    uint8_t enb, uint8_t in3, uint8_t in4
) {
    _l298nEnabled = true;
    _ena = ena; _in1 = in1; _in2 = in2;
    _enb = enb; _in3 = in3; _in4 = in4;
}

void Controller::setMotorDebugPrintIntervalMs(uint16_t ms) {
    _motorDebugPrintMs = ms;
}

bool Controller::beginAP(bool debug) {

    if (_ledEnabled) setLedStateHold(LED_BOOTING, 1500);
    _debug = debug;

    if (_l298nEnabled) {
        pinMode(_in1, OUTPUT); pinMode(_in2, OUTPUT);
        pinMode(_in3, OUTPUT); pinMode(_in4, OUTPUT);
        pinMode(_ena, OUTPUT); pinMode(_enb, OUTPUT);
        motorInitSafeStop();
    }

    if (wifiSSIDExistsNearby()) { // _debug &&
        Serial.print("[WiFi] NOTE: an AP with SSID already exists nearby: ");
        Serial.println(_ssid);
        if (_ledEnabled) setLedStateHold(LED_ERROR, 2000);
        // If you want to abort instead of just warn, uncomment:
        // return false;
    }

    String fv = WiFi.firmwareVersion();
    if (fv < WIFI_FIRMWARE_LATEST_VERSION) {
        Serial.println("Warning: WiFi firmware may be outdated. Consider upgrading.");
        setLedStateForce(LED_ERROR);
        setLedStateHold(LED_ERROR, 1000);
    }

    Serial.print("Starting AP: ");
    Serial.println(_ssid);

    WiFi.config(IPAddress(10, 0, 0, 2));

    _status = WiFi.beginAP(_ssid, _password);

    if (_status != WL_AP_LISTENING && _status != WL_AP_CONNECTED) {
        Serial.println("Failed to start AP mode");
        setLedStateForce(LED_ERROR);
        return false;
    }
    setLedState(LED_AP_READY);

    delay(2000);
    _server.begin();

    _lastDriveMs = millis();
    _failsafeStopped = false;

    Serial.println("AP mode started");
    printWiFiStatus();
    return true;
}

void Controller::update() {
    // Handle ONE incoming client per loop; keep loop fast
    WiFiClient client = _server.available();
    if (client) {
        client.setTimeout(30);
        handleClient(client);
        delay(1);
        client.stop();
    }

    // Failsafe check
    const unsigned long now = millis();
    if (_failsafeTimeoutMs > 0 && (now - _lastDriveMs) > _failsafeTimeoutMs) {
        _failsafeStopped = true;
        setLedStateHold(LED_FAILSAFE, 1200);
    }

    // Apply smoothing and notify motors (also handles failsafe)
    applySmoothingAndNotify();
    updateStatusLED();   // update the LED status (if enabled)
}

void Controller::applySmoothingAndNotify() {
    auto applyDeadband = [&](int8_t v) -> int8_t {
        if (abs((int)v) < (int)_deadband) return 0;
        return v;
    };

    int8_t targetL = _failsafeStopped ? 0 : applyDeadband(_cmdLeft);
    int8_t targetR = _failsafeStopped ? 0 : applyDeadband(_cmdRight);

    auto stepToward = [&](int8_t cur, int8_t tgt) -> int8_t {
        int d = (int)tgt - (int)cur;

        // Use a bigger step when we are braking toward zero
        int step = (tgt == 0) ? (int)_slewPerUpdateStop : (int)_slewPerUpdate;

        if (d > step) d = step;
        if (d < -step) d = -step;
        return (int8_t)((int)cur + d);
    };

    int8_t newL = stepToward(_outLeft, targetL);
    int8_t newR = stepToward(_outRight, targetR);

    if (newL == _outLeft && newR == _outRight) return;

    _outLeft = newL;
    _outRight = newR;

    // Internal motor driver (if enabled)
    if (_l298nEnabled) {
        motorApply(_outLeft, _outRight);
    }

    // Optional external callback
    if (_onDrive) {
        _onDrive(_outLeft, _outRight);
    }
}

int8_t Controller::speedLeft() const { return _outLeft; }
int8_t Controller::speedRight() const { return _outRight; }
uint8_t Controller::getThrottle() const { return _lastThrottle; }

bool Controller::wifiSSIDExistsNearby() {
    int n = WiFi.scanNetworks();
    if (n < 0) return false;

    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == String(_ssid)) {
            return true;
        }
    }
    return false;
}

void Controller::debugWiFiScanForSSID()  {
    Serial.println("[WiFi] Scanning for nearby networks...");
    int n = WiFi.scanNetworks();
    if (n < 0) {
        Serial.println("[WiFi] scanNetworks() failed");
        return;
    }

    Serial.print("[WiFi] Found ");
    Serial.print(n);
    Serial.println(" networks:");

    bool foundSame = false;

    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        int32_t rssi = WiFi.RSSI(i);

        Serial.print("  - ");
        Serial.print(s);
        Serial.print("  RSSI=");
        Serial.println(rssi);

        if (s == String(_ssid)) foundSame = true;
    }

    if (foundSame) {
        Serial.print("[WiFi] WARNING: SSID already present nearby: ");
        setLedStateForce(LED_ERROR);
        setLedStateHold(LED_ERROR, 2000);
        Serial.println(_ssid);

    } else {
        Serial.print("[WiFi] OK: SSID not seen nearby: ");
        Serial.println(_ssid);
    }
}

void Controller::printWiFiStatus() const {
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());

    IPAddress ip = WiFi.localIP();
    Serial.print("IP Address: ");
    Serial.println(ip);

    Serial.print("To control: http://");
    Serial.print(ip);
    Serial.println("/");
}
void Controller::setMotorMinPWM(uint8_t pwm) {
    _motorMinPWM = pwm;
}

String Controller::readRequestLine(WiFiClient& client) {
    unsigned long start = millis();
    while (client.connected() && !client.available()) {
        if (millis() - start > 30) return "";
        delay(1);
    }
    String line = client.readStringUntil('\n');
    line.trim();
    return line;
}

void Controller::sendHttpOk(WiFiClient& client, const char* contentType, const String& body) {
    client.println("HTTP/1.1 200 OK");
    client.print("Content-Type: ");
    client.println(contentType);
    client.println("Connection: close");
    client.print("Content-Length: ");
    client.println(body.length());
    client.println();
    client.print(body);
}

void Controller::sendHttpNotFound(WiFiClient& client) {
    const String body = "Not Found";
    client.println("HTTP/1.1 404 Not Found");
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println("Connection: close");
    client.print("Content-Length: ");
    client.println(body.length());
    client.println();
    client.print(body);
}

int Controller::clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool Controller::extractQueryInt(const String& requestLine, const char* key, int& outValue) {
    int q = requestLine.indexOf('?');
    if (q < 0) return false;

    int end = requestLine.indexOf(' ', q);
    if (end < 0) return false;

    String query = requestLine.substring(q + 1, end);

    String k = String(key) + "=";
    int pos = query.indexOf(k);
    if (pos < 0) return false;

    int valStart = pos + k.length();
    int amp = query.indexOf('&', valStart);
    String valStr = (amp >= 0) ? query.substring(valStart, amp) : query.substring(valStart);

    valStr.replace("+", " ");
    outValue = valStr.toInt();
    return true;
}

void Controller::handleClient(WiFiClient& client) {
    String requestLine = readRequestLine(client);
    if (requestLine.length() == 0) return;

    // Drain headers
    while (client.connected()) {
        String h = client.readStringUntil('\n');
        if (h == "\r" || h.length() == 0) break;
    }

    if (requestLine.startsWith("GET / ") || requestLine.startsWith("GET /?")) {
        handleRoot(client);
        setLedStateHold(LED_CLIENT_CONNECTED, 2000);
        return;
    }

    if (requestLine.startsWith("GET /drive")) {
        handleDrive(client, requestLine);
        return;
    }

    if (requestLine.startsWith("GET /btn?")) {
        handleBtn(client, requestLine);
        return;
    }

    if (requestLine.startsWith("GET /control?msg=")) {
        handleControlMsg(client, requestLine);
        return;
    }

    if (requestLine.startsWith("GET /health ")) {
        handleHealth(client);
        return;
    }

    sendHttpNotFound(client);
}

void Controller::handleHealth(WiFiClient& client) {
    sendHttpOk(client, "text/plain; charset=utf-8", "OK");
}

void Controller::handleControlMsg(WiFiClient& client, const String& requestLine) {
    int start = String("GET /control?msg=").length();
    int end = requestLine.indexOf(' ', start);
    String msg = (end > start) ? requestLine.substring(start, end) : "";
    msg.replace("+", " ");

    if (_onMessage) _onMessage(msg);

    sendHttpOk(client, "text/plain; charset=utf-8", "OK");
}

void Controller::handleBtn(WiFiClient& client, const String& requestLine) {
    int id = -1;
    if (!extractQueryInt(requestLine, "id", id)) {
        sendHttpOk(client, "text/plain; charset=utf-8", "Missing id");
        return;
    }

    if (id < 0 || id >= (int)_buttonCount) {
        sendHttpOk(client, "text/plain; charset=utf-8", "Bad id");
        return;
    }

    if (_buttons[id].cb) _buttons[id].cb();

    if (_onMessage) _onMessage(String("btn:") + _buttons[id].label);

    sendHttpOk(client, "text/plain; charset=utf-8", "OK");
}

void Controller::handleDrive(WiFiClient& client, const String& requestLine) {
    int x = 0;   // -100..100
    int y = 0;   // -100..100
    int t = 100; // 0..100

    extractQueryInt(requestLine, "x", x);
    extractQueryInt(requestLine, "y", y);
    extractQueryInt(requestLine, "t", t);

    x = clampInt(x, -100, 100);
    y = clampInt(y, -100, 100);
    t = clampInt(t, 0, 100);

    _lastThrottle = (uint8_t)constrain(t, 0, 100);

    int left  = clampInt(y + x, -100, 100);
    int right = clampInt(y - x, -100, 100);

    left  = (left * t) / 100;
    right = (right * t) / 100;

    _cmdLeft = (int8_t)left;
    _cmdRight = (int8_t)right;

    _lastDriveMs = millis();
    _failsafeStopped = false;

    setLedStateHold(LED_CLIENT_CONNECTED, 1000);

    // Optional debug prints (beware: will spam if heartbeat is enabled)
    // Serial.print("Drive: L="); Serial.print(_cmdLeft);
    // Serial.print(" R="); Serial.println(_cmdRight);

    sendHttpOk(client, "text/plain; charset=utf-8", "OK");
}

void Controller::enableStatusLED(uint8_t pin) {
    _ledPin = pin;
    _ledEnabled = true;
    pinMode(_ledPin, OUTPUT);
    digitalWrite(_ledPin, LOW);
}

void Controller::setLedState(Controller::LedState s) {
    if (!_ledEnabled) return;

    unsigned long now = millis();
    if (now < _ledHoldUntilMs) return;  // respect hold

    _ledState = s;
    _ledTimer = now;
}

void Controller::setLedStateHold(Controller::LedState s, uint16_t holdMs) {
    if (!_ledEnabled) return;

    unsigned long now = millis();
    _ledState = s;
    _ledTimer = now;
    _ledHoldUntilMs = now + (unsigned long)holdMs;
}

void Controller::setLedStateForce(Controller::LedState s) {
    if (!_ledEnabled) return;

    unsigned long now = millis();
    _ledHoldUntilMs = 0;      // clear hold
    _ledState = s;
    _ledTimer = now;
}

void Controller::updateStatusLED() {
    if (!_ledEnabled) return;

    unsigned long now = millis();

    switch (_ledState) {

        case LED_BOOTING:
            if (now - _ledTimer > 100) {
                _ledTimer = now;
                _ledLevel = !_ledLevel;
                digitalWrite(_ledPin, _ledLevel);
            }
            break;

        case LED_AP_READY:
            if (now - _ledTimer > 500) {
                _ledTimer = now;
                _ledLevel = !_ledLevel;
                digitalWrite(_ledPin, _ledLevel);
            }
            break;

        case LED_CLIENT_CONNECTED:
            digitalWrite(_ledPin, HIGH);
            break;

        case LED_FAILSAFE:
            // double blink pattern
            if (now - _ledTimer > 150) {
                _ledTimer = now;
                _ledLevel = !_ledLevel;
                digitalWrite(_ledPin, _ledLevel);
            }
            break;

        case LED_ERROR:
            if (now - _ledTimer > 70) {
                _ledTimer = now;
                _ledLevel = !_ledLevel;
                digitalWrite(_ledPin, _ledLevel);
            }
            break;
    }
}

void Controller::handleRoot(WiFiClient& client) {
    String buttonsHtml;
    for (uint8_t i = 0; i < _buttonCount; i++) {
        buttonsHtml += "<button class='uBtn' data-id='";
        buttonsHtml += i;
        buttonsHtml += "'>";
        buttonsHtml += _buttons[i].label;
        buttonsHtml += "</button> ";
    }
    if (_buttonCount == 0) {
        buttonsHtml = "<div style='opacity:.7'>No buttons registered</div>";
    }

    String page;
    page.reserve(7500);

    page += "<!doctype html><html><head><meta charset='utf-8'/>";
    page += "<meta name='viewport' content='width=device-width,initial-scale=1'/>";
    page += "<title>Robot Controller</title>";
    page += "<style>";
  page += "#thrRow{margin-top:10px;}";
  page += ".thrHeader{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px;}";
  page += ".thrLabel{font-size:16px;font-weight:600;}";
  page += ".thrValue{font-size:16px;font-variant-numeric:tabular-nums;opacity:.9;}";

// Make the slider easy to drag
  page += "input.thr{width:100%;height:42px;-webkit-appearance:none;appearance:none;background:transparent;touch-action:none;}";

// Track
  page += "input.thr::-webkit-slider-runnable-track{height:12px;border-radius:999px;background:#ddd;border:1px solid #333;}";
  page += "input.thr::-moz-range-track{height:12px;border-radius:999px;background:#ddd;border:1px solid #333;}";

// Thumb (big + easy to grab)
  page += "input.thr::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:34px;height:34px;border-radius:50%;background:#333;border:2px solid #fff;margin-top:-12px;box-shadow:0 2px 6px rgba(0,0,0,.25);}";
  page += "input.thr::-moz-range-thumb{width:34px;height:34px;border-radius:50%;background:#333;border:2px solid #fff;box-shadow:0 2px 6px rgba(0,0,0,.25);}";

// Optional: show active focus without ugly outline
    page += "input.thr:focus{outline:none;}";
    page += "body{font-family:system-ui,Arial;margin:16px;}";
    page += "#wrap{max-width:520px;margin:0 auto;}";
    page += ".row{margin:14px 0;}";
    page += "button{padding:12px 16px;font-size:16px;border-radius:12px;border:1px solid #333;background:#f2f2f2;}";
    page += ".uBtn{margin:6px 8px 6px 0;}";
    page += "#joy{width:260px;height:260px;border:2px solid #333;border-radius:18px;";
    page += "touch-action:none; position:relative; user-select:none; -webkit-user-select:none;}";
    page += "#stick{width:70px;height:70px;border-radius:50%;background:#333;opacity:.85;";
    page += "position:absolute;left:95px;top:95px;}";
    page += "label{display:block;margin-bottom:6px;}";
    page += "input[type=range]{width:100%;}";
    page += "#status{font-family:ui-monospace,Menlo,monospace; white-space:pre;}";
    page += "</style></head><body><div id='wrap'>";
    page += "<h2>Robot Controller</h2>";

    page += "<div class='row' id='buttons'>";
    page += buttonsHtml;
    page += "</div>";

    page += "<div class='row'><div id='joy'><div id='stick'></div></div></div>";

page += "<div class='row' id='thrRow'>";
page += "  <div class='thrHeader'>";
page += "    <div class='thrLabel'>Throttle</div>";
page += "    <div class='thrValue'><span id='tval'>100</span>%</div>";
page += "  </div>";
page += "  <input id='thr' class='thr' type='range' min='0' max='100' value='100' step='1'/>";
page += "</div>";

    // --- JS (STOP priority even if a request is in-flight) + HEARTBEAT resend ---
    page += "<script>";
    page += "let x=0,y=0,t=100;";
    page += "const joy=document.getElementById('joy');";
    page += "const stick=document.getElementById('stick');";
    page += "const thr=document.getElementById('thr');";
    page += "const tval=document.getElementById('tval');";
    page += "const status=document.getElementById('status');";

    page += "function clamp(v,a,b){return Math.max(a,Math.min(b,v));}";
    page += "function setStick(px,py){stick.style.left=(px-35)+'px'; stick.style.top=(py-35)+'px';}";
    page += "function updateStatus(extra=''){status.textContent=`x=${x} y=${y} t=${t}` + (extra?('\\n'+extra):'');}";

    // Dynamic buttons
    page += "document.querySelectorAll('.uBtn').forEach(b=>{";
    page += "  b.addEventListener('click',()=>{";
    page += "    const id=b.getAttribute('data-id');";
    page += "    fetch(`/btn?id=${id}&_=${Date.now()}`, {cache:'no-store'}).catch(()=>{});";
    page += "    updateStatus('btn id=' + id);";
    page += "  });";
    page += "});";

    // --- Drive send logic: 1 in-flight, STOP priority, + heartbeat keepalive ---
    page += "let inFlight=false;";
    page += "let pending=false;";
    page += "let lastSentX=999,lastSentY=999,lastSentT=999;";
    page += "let lastSendMs=0;";
    page += "const HEARTBEAT_MS=200;";

    page += "function sendDriveNow(force=false){";
    page += "  const now=Date.now();";
    page += "  const same = (x===lastSentX && y===lastSentY && t===lastSentT);";
    page += "  if (!force && same && (now - lastSendMs) < HEARTBEAT_MS) return;";
    page += "  const isStop = (x===0 && y===0);";

    // Keep your existing throttle rule, but allow heartbeat sends too
    page += "  if (inFlight && !isStop){ pending=true; return; }";
    page += "  if (!isStop){ inFlight=true; pending=false; }";

    page += "  const url=`/drive?x=${x}&y=${y}&t=${t}&_=${now}`;";
    page += "  lastSendMs=now;";

    page += "  fetch(url,{cache:'no-store', keepalive:true})";
    page += "    .catch(()=>{})";
    page += "    .finally(()=>{";
    page += "      lastSentX=x; lastSentY=y; lastSentT=t;";
    page += "      if (!isStop){";
    page += "        inFlight=false;";
    page += "        if (pending) sendDriveNow(true);";
    page += "      }";
    page += "    });";
    page += "}";

    // Heartbeat: keep sending while held away from center (prevents failsafe)
    page += "setInterval(()=>{";
    page += "  if (x!==0 || y!==0) sendDriveNow(false);";
    page += "}, HEARTBEAT_MS);";

    // Joystick mapping
    page += "function posToXY(clientX,clientY){";
    page += "  const r=joy.getBoundingClientRect();";
    page += "  const cx=clientX - r.left;";
    page += "  const cy=clientY - r.top;";
    page += "  const dx=cx - r.width/2;";
    page += "  const dy=cy - r.height/2;";
    page += "  const max=r.width/2 - 35;";
    page += "  const ndx=clamp(dx,-max,max);";
    page += "  const ndy=clamp(dy,-max,max);";
    page += "  x=Math.round((ndx/max)*100);";
    page += "  y=Math.round((-ndy/max)*100);";
    page += "  if (Math.abs(x) < 4) x=0;";
    page += "  if (Math.abs(y) < 4) y=0;";
    page += "  setStick(r.width/2 + ndx, r.height/2 + ndy);";
    page += "  updateStatus();";
    page += "  sendDriveNow(true);"; // force immediate send on changes
    page += "}";

    page += "let dragging=false;";
    page += "joy.addEventListener('pointerdown',(e)=>{";
    page += "  dragging=true;";
    page += "  joy.setPointerCapture(e.pointerId);";
    page += "  posToXY(e.clientX,e.clientY);";
    page += "});";
    page += "joy.addEventListener('pointermove',(e)=>{";
    page += "  if(!dragging) return;";
    page += "  posToXY(e.clientX,e.clientY);";
    page += "});";
    page += "joy.addEventListener('pointerup',()=>{";
    page += "  dragging=false;";
    page += "  x=0; y=0;";
    page += "  setStick(130,130);";
    page += "  updateStatus('released');";
    page += "  sendDriveNow(true);"; // force STOP send
    page += "});";
    page += "joy.addEventListener('pointercancel',()=>{";
    page += "  dragging=false;";
    page += "  x=0; y=0;";
    page += "  setStick(130,130);";
    page += "  updateStatus('cancel');";
    page += "  sendDriveNow(true);"; // force STOP send
    page += "});";

    // Slider
    page += "thr.addEventListener('input',()=>{";
    page += "  t=parseInt(thr.value,10)||0;";
    page += "  tval.textContent=t;";
    page += "  updateStatus('slider');";
    page += "  sendDriveNow(true);";
    page += "});";

    page += "updateStatus('ready');";
    page += "sendDriveNow(true);";
    page += "</script>";

    page += "</div></body></html>";

    sendHttpOk(client, "text/html; charset=utf-8", page);
}

// -------------------- L298N implementation --------------------

void Controller::motorInitSafeStop() {
    // Ensure stopped at boot (BRAKE)
    digitalWrite(_in1, HIGH);
    digitalWrite(_in2, HIGH);
    analogWrite(_ena, 0);

    digitalWrite(_in3, HIGH);
    digitalWrite(_in4, HIGH);
    analogWrite(_enb, 0);
}

void Controller::speedToCmd(int8_t spd, bool &forward, uint8_t &pwm) {
    int s = spd; // -100..100
    if (s >= 0) {
        forward = true;
    } else {
        forward = false;
        s = -s;
    }
    s = constrain(s, 0, 100);
    pwm = (uint8_t)map(s, 0, 100, 0, 255);
}

void Controller::setMotorOne(uint8_t en, uint8_t inA, uint8_t inB, int8_t spd) {
    int s = spd;
    if (s > 0) {
        digitalWrite(inA, HIGH);
        digitalWrite(inB, LOW);
    } else if (s < 0) {
        digitalWrite(inA, LOW);
        digitalWrite(inB, HIGH);
        s = -s;
    } else {
        // BRAKE (stops faster than coast)
        digitalWrite(inA, HIGH);
        digitalWrite(inB, HIGH);
        analogWrite(en, 0);
        return;
    }

    int pwm = map(constrain(s, 0, 100), 0, 100, 0, 255);
  // to prevent motor whining when starting from rest, we can enforce a minimum PWM threshold (tune as needed)
    if (pwm > 0 && pwm < _motorMinPWM) pwm = _motorMinPWM;
    analogWrite(en, pwm);
}

void Controller::debugMotors(int8_t left, int8_t right) {
    if (!_debug) return;

    const unsigned long now = millis();
    const bool changed = (left != _lastDbgL) || (right != _lastDbgR);
    const bool timeOk  = (now - _lastDbgPrintMs) >= _motorDebugPrintMs;

    if (!changed && !timeOk) return;

    bool lfwd, rfwd;
    uint8_t lpwm, rpwm;
    speedToCmd(left, lfwd, lpwm);
    speedToCmd(right, rfwd, rpwm);

    Serial.print("[MOTOR] L=");
    Serial.print(left);
    Serial.print(lfwd ? " FWD " : " REV ");
    Serial.print("PWM=");
    Serial.print(lpwm);

    Serial.print(" | R=");
    Serial.print(right);
    Serial.print(rfwd ? " FWD " : " REV ");
    Serial.print("PWM=");
    Serial.println(rpwm);

    _lastDbgL = left;
    _lastDbgR = right;
    _lastDbgPrintMs = now;
}

void Controller::motorApply(int8_t left, int8_t right) {
    debugMotors(left, right);
    setMotorOne(_ena, _in1, _in2, left);
    setMotorOne(_enb, _in3, _in4, right);
}