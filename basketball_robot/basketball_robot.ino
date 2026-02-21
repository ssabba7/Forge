/*
 * basketball_robot.ino
 * Arduino R4 WiFi — Basketball Robot Controller
 *
 * WiFi AP : BasketballRobot / robot1234
 * URL     : http://192.168.4.1
 *
 * Pin map (L298N #1 — drive):
 *   ENA→D5  IN1→D2  IN2→D3   (left motor,  PWM speed)
 *   ENB→D6  IN3→D7  IN4→D8   (right motor, PWM speed)
 *
 * Pin map (L298N #2 — catapult):
 *   ENA2→D10  IN5→D11  IN6→D12
 */

#include <WiFiS3.h>

// ── Tunable constants ─────────────────────────────────────────────────────────
#define WINDUP_MS       600   // catapult wind-up duration  (ms)
#define RELEASE_MS      200   // reverse-snap release duration (ms)
#define WINDUP_PAUSE_MS  50   // pause between wind and release (ms)

// ── WiFi credentials ──────────────────────────────────────────────────────────
static const char AP_SSID[] = "BasketballRobot";
static const char AP_PASS[] = "robot1234";

// ── Pin definitions ───────────────────────────────────────────────────────────
// PWM-capable pins on Arduino R4 WiFi: 3, 5, 6, 9, 10, 11
// Enable pins MUST be on PWM pins; direction pins can be any digital pin.
//
// L298N #1 — drive
static const uint8_t ENA = 5,  IN1 = 2,  IN2 = 3;   // left  motor  (ENA ~PWM)
static const uint8_t ENB = 6,  IN3 = 7,  IN4 = 8;   // right motor  (ENB ~PWM)
// L298N #2 — catapult
static const uint8_t ENA2 = 10, IN5 = 11, IN6 = 12; // catapult      (ENA2 ~PWM)

// ── State ─────────────────────────────────────────────────────────────────────
WiFiServer server(80);
static unsigned long lastDriveMs = 0;
static const unsigned long WATCHDOG_MS = 400;

// ── Motor helpers ─────────────────────────────────────────────────────────────

// speed: -255 (full reverse) … +255 (full forward)
void setMotor(uint8_t en, uint8_t inA, uint8_t inB, int speed) {
  speed = constrain(speed, -255, 255);
  if (speed > 0) {
    digitalWrite(inA, HIGH); digitalWrite(inB, LOW);
  } else if (speed < 0) {
    digitalWrite(inA, LOW);  digitalWrite(inB, HIGH);
  } else {
    digitalWrite(inA, LOW);  digitalWrite(inB, LOW);
  }
  analogWrite(en, abs(speed));
}

void stopDrive() {
  setMotor(ENA, IN1, IN2, 0);
  setMotor(ENB, IN3, IN4, 0);
}

void stopAll() {
  stopDrive();
  setMotor(ENA2, IN5, IN6, 0);
}

// Blocking shoot sequence — call AFTER closing the HTTP client
void doShoot(int power) {
  int pwm = map(constrain(power, 0, 100), 0, 100, 0, 255);
  setMotor(ENA2, IN5, IN6,  pwm);  delay(WINDUP_MS);       // 1. wind-up
  setMotor(ENA2, IN5, IN6,  0);    delay(WINDUP_PAUSE_MS); // 2. pause
  setMotor(ENA2, IN5, IN6, -255);  delay(RELEASE_MS);      // 3. snap release
  setMotor(ENA2, IN5, IN6,  0);                            // 4. stop
}

// ── HTTP helpers ──────────────────────────────────────────────────────────────

void sendOK(WiFiClient& c, const __FlashStringHelper* body) {
  c.println(F("HTTP/1.1 200 OK"));
  c.println(F("Content-Type: text/plain"));
  c.println(F("Connection: close"));
  c.println();
  c.println(body);
}

// Extract integer query parameter; returns defaultVal if key not found.
int parseParam(const String& line, const char* key, int defaultVal) {
  String k(key);
  k += '=';
  int idx = line.indexOf(k);
  if (idx < 0) return defaultVal;
  idx += k.length();
  int end = line.indexOf('&', idx);
  if (end < 0) end = line.indexOf(' ', idx);
  if (end < 0) end = line.length();
  return line.substring(idx, end).toInt();
}

// ── HTML control page ─────────────────────────────────────────────────────────
void sendPage(WiFiClient& c) {
  c.println(F("HTTP/1.1 200 OK"));
  c.println(F("Content-Type: text/html"));
  c.println(F("Connection: close"));
  c.println();

  // ── <head> + CSS ─────────────────────────────────────────────────────────
  c.print(F(
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Basketball Robot</title><style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:#1a1a2e;color:#eee;font-family:Arial,sans-serif;"
      "display:flex;flex-direction:column;align-items:center;"
      "padding:16px;gap:16px}"
    "h1{color:#e94560;font-size:1.5rem;letter-spacing:3px;margin-top:8px}"
    ".panel{display:flex;gap:16px;width:100%;max-width:480px;"
      "flex-wrap:wrap;justify-content:center}"
    "#joy-wrap{display:flex;justify-content:center;align-items:center}"
    "canvas{touch-action:none;border-radius:50%;background:#0f3460}"
    ".ctrl{display:flex;flex-direction:column;gap:14px;"
      "justify-content:center;min-width:160px}"
    "label{font-size:.82rem;color:#aaa;display:block;margin-bottom:2px}"
    "input[type=range]{width:100%;accent-color:#e94560}"
    ".val{float:right;color:#e94560}"
    "button{display:block;width:100%;max-width:480px;padding:16px;"
      "font-size:1.05rem;border:none;border-radius:8px;cursor:pointer;"
      "font-weight:bold;letter-spacing:1px}"
    "#btnShoot{background:#e94560;color:#fff}"
    "#btnStop{background:#333;color:#ccc;margin-top:6px}"
    "#status{font-size:.75rem;color:#666}"
    "</style></head><body>"
    "<h1>BASKETBALL ROBOT</h1>"
    "<div class='panel'>"
      "<div id='joy-wrap'>"
        "<canvas id='jsc' width='200' height='200'></canvas>"
      "</div>"
      "<div class='ctrl'>"
        "<div>"
          "<label>Throttle <span id='tv' class='val'>50%</span></label>"
          "<input type='range' id='thr' min='0' max='100' value='50'>"
        "</div>"
        "<div>"
          "<label>Shoot Power <span id='pv' class='val'>50%</span></label>"
          "<input type='range' id='pwr' min='0' max='100' value='50'>"
        "</div>"
      "</div>"
    "</div>"
    "<button id='btnShoot'>SHOOT</button>"
    "<button id='btnStop'>STOP ALL</button>"
    "<div id='status'>Ready</div>"
  ));

  // ── JavaScript ────────────────────────────────────────────────────────────
  c.print(F(
    "<script>"
    // Joystick state
    "const cv=document.getElementById('jsc'),"
      "ctx=cv.getContext('2d'),"
      "R=cv.width/2,KR=38;"
    "let kx=R,ky=R,drag=false,lx=0,ly=0;"

    // Draw joystick
    "function draw(){"
      "ctx.clearRect(0,0,cv.width,cv.height);"
      "ctx.beginPath();ctx.arc(R,R,R-2,0,2*Math.PI);"
      "ctx.strokeStyle='#e94560aa';ctx.lineWidth=2;ctx.stroke();"
      "ctx.beginPath();ctx.arc(R,R,3,0,2*Math.PI);"
      "ctx.fillStyle='#e9456055';ctx.fill();"
      "ctx.beginPath();ctx.arc(kx,ky,KR,0,2*Math.PI);"
      "ctx.fillStyle='#e94560';ctx.fill();"
    "}"

    // Pointer position helper
    "function pos(e){"
      "const r=cv.getBoundingClientRect(),"
        "t=e.touches?e.touches[0]:e;"
      "return[t.clientX-r.left,t.clientY-r.top];"
    "}"

    // Update knob position and axis values
    "function move(x,y){"
      "const dx=x-R,dy=y-R,"
        "d=Math.hypot(dx,dy),m=R-KR;"
      "kx=R+(d>m?dx/d*m:dx);"
      "ky=R+(d>m?dy/d*m:dy);"
      "lx=Math.round((kx-R)/m*100);"
      "ly=Math.round((R-ky)/m*100);"
      "draw();"
    "}"

    // Reset knob to centre
    "function reset(){drag=false;kx=R;ky=R;lx=0;ly=0;draw();}"

    // Mouse events
    "cv.addEventListener('mousedown',e=>{drag=true;move(...pos(e));});"
    "cv.addEventListener('mousemove',e=>{if(drag)move(...pos(e));});"
    "cv.addEventListener('mouseup',reset);"
    "cv.addEventListener('mouseleave',reset);"

    // Touch events
    "cv.addEventListener('touchstart',e=>{drag=true;move(...pos(e));"
      "e.preventDefault();},{passive:false});"
    "cv.addEventListener('touchmove',e=>{if(drag)move(...pos(e));"
      "e.preventDefault();},{passive:false});"
    "cv.addEventListener('touchend',reset);"

    // Slider display
    "const thr=document.getElementById('thr'),"
      "pwr=document.getElementById('pwr'),"
      "st=document.getElementById('status');"
    "thr.oninput=()=>document.getElementById('tv').textContent=thr.value+'%';"
    "pwr.oninput=()=>document.getElementById('pv').textContent=pwr.value+'%';"

    // HTTP helper — 300 ms timeout, updates status
    "function req(url){"
      "fetch(url,{signal:AbortSignal.timeout(300)})"
        ".then(()=>st.textContent='\u2713 '+url.split('?')[0])"
        ".catch(()=>st.textContent='timeout');"
    "}"

    // Drive loop — 100 ms interval
    "setInterval(()=>"
      "req('/drive?lx='+lx+'&ly='+ly+'&throttle='+thr.value)"
    ",100);"

    // Button handlers
    "document.getElementById('btnShoot').onclick="
      "()=>req('/shoot?power='+pwr.value);"
    "document.getElementById('btnStop').onclick=()=>req('/stop');"

    "draw();"
    "</script></body></html>"
  ));
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000);  // wait up to 3 s for USB serial

  // Configure all motor pins as outputs
  const uint8_t mPins[] = {ENA,IN1,IN2,ENB,IN3,IN4,ENA2,IN5,IN6};
  for (uint8_t p : mPins) pinMode(p, OUTPUT);
  stopAll();

  // Start WiFi access point
  Serial.print(F("Starting AP \""));
  Serial.print(AP_SSID);
  Serial.println(F("\" ..."));

  WiFi.beginAP(AP_SSID, AP_PASS);
  delay(2000);

  Serial.print(F("AP started — connect to "));
  Serial.print(AP_SSID);
  Serial.print(F(" / "));
  Serial.println(AP_PASS);
  Serial.print(F("Open browser: http://"));
  Serial.println(WiFi.localIP());

  server.begin();
  lastDriveMs = millis();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  // Drive-motor watchdog: stop wheels if no /drive arrives within 400 ms
  if (millis() - lastDriveMs > WATCHDOG_MS) {
    stopDrive();
    lastDriveMs = millis();  // reset so we don't call stopDrive every iteration
  }

  WiFiClient client = server.available();
  if (!client) return;

  // Read HTTP request (headers only; stop at blank line or 512-byte guard)
  String req = "";
  unsigned long deadline = millis() + 1000;
  while (client.connected() && millis() < deadline) {
    if (client.available()) {
      char ch = client.read();
      req += ch;
      if (req.endsWith("\r\n\r\n")) break;
      if (req.length() > 512) break;
    }
  }

  // Route on first line of the request
  int eol  = req.indexOf('\n');
  String line = (eol >= 0) ? req.substring(0, eol) : req;

  if (line.indexOf("GET / ") >= 0 || line.startsWith("GET / ")) {
    // ── Serve control page ─────────────────────────────────────────────────
    sendPage(client);
  }
  else if (line.indexOf("/drive") >= 0) {
    // ── Update drive motors ────────────────────────────────────────────────
    int lx  = parseParam(line, "lx",       0);
    int ly  = parseParam(line, "ly",       0);
    int thr = parseParam(line, "throttle", 50);

    // Joystick → differential drive mixing (plan formula)
    int leftSpd  = constrain((ly + lx) * thr / 100, -255, 255);
    int rightSpd = constrain((ly - lx) * thr / 100, -255, 255);

    setMotor(ENA, IN1, IN2, leftSpd);
    setMotor(ENB, IN3, IN4, rightSpd);
    lastDriveMs = millis();

    sendOK(client, F("ok"));
  }
  else if (line.indexOf("/shoot") >= 0) {
    // ── Fire catapult (blocking) ───────────────────────────────────────────
    int power = parseParam(line, "power", 50);
    sendOK(client, F("shooting"));
    client.stop();       // close connection before the blocking sequence
    doShoot(power);
    lastDriveMs = millis();  // reset watchdog after blocking sequence
    return;              // client already stopped
  }
  else if (line.indexOf("/stop") >= 0) {
    // ── Emergency stop ────────────────────────────────────────────────────
    stopAll();
    lastDriveMs = millis();
    sendOK(client, F("stopped"));
  }
  else {
    client.println(F("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot found"));
  }

  delay(1);
  client.stop();
}
