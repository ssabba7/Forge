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
// Catapult runs continuously until STOP is pressed.
// CHARGE = slow wind-up speed.  SHOOT = fast release speed.
// Both spin in CATAPULT_FLIP direction.  Adjust PWM values to taste.
#define CATAPULT_SHOOT_PWM   255   // fast shoot speed   (0–255)
// Charge speed is set by the in-page slider (0–255 mapped from 0–100%)
#define CATAPULT_SHOOT_MS    100   // snap duration in ms — increase for more rotation

// Motor direction — set to -1 to flip a motor that runs the wrong way
#define LEFT_FLIP      -1    // 1 = normal,   -1 = reversed
#define RIGHT_FLIP     -1    // 1 = normal,   -1 = reversed
#define CATAPULT_FLIP  -1    // 1 = normal,   -1 = reversed

// Power trim — left motor is physically stronger, so boost the right to match.
// 100 = no trim.  Increase until W drives straight (try 110, 120, 130...).
#define RIGHT_TRIM  80

// Deadband — minimum PWM needed for a motor to actually start spinning.
// Any non-zero command below this is clamped UP to this value so both motors
// overcome stiction at the same time.  Raise if the right motor still starts
// late; lower if low-speed control feels too jumpy.
#define MIN_PWM  60

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
// Deadband is NOT applied here — call applyDeadband() before this for drive motors.
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

// Floor any non-zero drive speed to MIN_PWM so both motors overcome stiction together.
int applyDeadband(int speed) {
  if (speed == 0) return 0;
  return (abs(speed) < MIN_PWM) ? (speed > 0 ? MIN_PWM : -MIN_PWM) : speed;
}

void stopDrive() {
  setMotor(ENA, IN1, IN2, 0);
  setMotor(ENB, IN3, IN4, 0);
}

void stopAll() {
  stopDrive();
  setMotor(ENA2, IN5, IN6, 0);
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
    // Mode toggle
    "#modeBar{display:flex;width:100%;max-width:480px;border-radius:10px;"
      "overflow:hidden;border:2px solid #0f3460}"
    "#modeBar button{flex:1;padding:14px;font-size:1rem;font-weight:bold;"
      "letter-spacing:1px;border:none;cursor:pointer;"
      "background:#0f3460;color:#555;transition:background .15s,color .15s}"
    "#modeBar button.active{background:#e94560;color:#fff}"
    // Panel
    ".panel{display:flex;gap:20px;width:100%;max-width:480px;"
      "flex-wrap:wrap;justify-content:center;align-items:center}"
    // WASD D-pad
    ".dpad{display:grid;grid-template-columns:repeat(3,68px);"
      "grid-template-rows:repeat(2,68px);gap:6px}"
    ".dk{width:68px;height:68px;font-size:1.5rem;font-weight:bold;"
      "border:2px solid #0f3460;border-radius:10px;background:#0f3460;"
      "color:#aaa;cursor:pointer;user-select:none;-webkit-user-select:none;"
      "touch-action:none;transition:background .08s,color .08s,transform .08s}"
    ".dk.on{background:#e94560;color:#fff;border-color:#e94560;transform:scale(.91)}"
    ".dk:disabled{opacity:.25;cursor:default}"
    "#bw{grid-column:2;grid-row:1}"
    "#ba{grid-column:1;grid-row:2}"
    "#bs{grid-column:2;grid-row:2}"
    "#bd{grid-column:3;grid-row:2}"
    // Sliders
    ".ctrl{display:flex;flex-direction:column;gap:14px;"
      "justify-content:center;min-width:160px}"
    "label{font-size:.82rem;color:#aaa;display:block;margin-bottom:2px}"
    "input[type=range]{width:100%;accent-color:#e94560}"
    ".val{float:right;color:#e94560}"
    // Action buttons
    ".actions{display:flex;flex-direction:column;gap:8px;"
      "width:100%;max-width:480px}"
    ".actions button{padding:16px;font-size:1.05rem;border:none;"
      "border-radius:8px;cursor:pointer;font-weight:bold;letter-spacing:1px}"
    // Shoot mode panel — CHARGE + SHOOT side by side
    ".shoot-row{display:flex;gap:8px}"
    ".shoot-row button{flex:1;padding:20px;font-size:1.1rem;border:none;"
      "border-radius:8px;cursor:pointer;font-weight:bold;letter-spacing:1px}"
    "#btnCharge{background:#0f3460;color:#4fc3f7;border:2px solid #4fc3f7}"
    "#btnShoot{background:#e94560;color:#fff}"
    "#btnStop{background:#2a2a2a;color:#ccc}"
    "#status{font-size:.75rem;color:#666}"
    "</style></head><body>"
    "<h1>BASKETBALL ROBOT</h1>"
    // Mode toggle bar
    "<div id='modeBar'>"
      "<button id='btnDrive' class='active' onclick='setMode(\"drive\")'>DRIVE</button>"
      "<button id='btnShootMode'          onclick='setMode(\"shoot\")'>SHOOT</button>"
    "</div>"
    // Drive panel: WASD buttons (full speed, no throttle)
    "<div id='drivePanel' class='panel'>"
      "<div class='dpad'>"
        "<button class='dk' id='bw'>W</button>"
        "<button class='dk' id='ba'>A</button>"
        "<button class='dk' id='bs'>S</button>"
        "<button class='dk' id='bd'>D</button>"
      "</div>"
    "</div>"
    // Shoot panel: charge power slider + charge/shoot buttons (hidden initially)
    "<div id='shootPanel' style='display:none;flex-direction:column;"
      "gap:12px;width:100%;max-width:480px'>"
      "<div class='ctrl'>"
        "<label>Charge Power <span id='cpv' class='val'>50%</span></label>"
        "<input type='range' id='chargePwr' min='0' max='100' value='50'>"
      "</div>"
      "<div class='shoot-row'>"
        "<button id='btnCharge'>CHARGE</button>"
        "<button id='btnShoot'>SHOOT</button>"
      "</div>"
    "</div>"
    "<div class='actions'>"
      "<button id='btnStop'>STOP ALL</button>"
    "</div>"
    "<div id='status'>Drive mode</div>"
  ));

  // ── JavaScript ────────────────────────────────────────────────────────────
  c.print(F(
    "<script>"
    "const keys={w:0,a:0,s:0,d:0};"
    "let lx=0,ly=0,mode='drive';"

    // Switch between drive and shoot modes
    "function setMode(m){"
      "mode=m;"
      "const drv=m==='drive';"
      "document.getElementById('btnDrive').classList.toggle('active',drv);"
      "document.getElementById('btnShootMode').classList.toggle('active',!drv);"
      "document.getElementById('drivePanel').style.display=drv?'':'none';"
      "document.getElementById('shootPanel').style.display=drv?'none':'flex';"
      // Release held WASD keys and stop motors when switching to shoot
      "if(!drv){'wasd'.split('').forEach(k=>setKey(k,0));req('/stop');}"
      "st.textContent=drv?'Drive mode':'Shoot mode';"
    "}"

    // Recompute axes from held keys
    "function updateAxes(){"
      "ly=(keys.w-keys.s)*100;"
      "lx=(keys.d-keys.a)*100;"
    "}"

    // Set key state + visual (ignored if drive disabled)
    "function setKey(k,v){"
      "if(!(k in keys))return;"
      "keys[k]=v;"
      "updateAxes();"
      "const el=document.getElementById('b'+k);"
      "if(el)el.classList.toggle('on',v===1);"
    "}"

    // Physical keyboard — only active in drive mode
    "document.addEventListener('keydown',e=>{"
      "if(mode!=='drive')return;"
      "if(['w','a','s','d'].includes(e.key.toLowerCase())){"
        "e.preventDefault();"
        "setKey(e.key.toLowerCase(),1);"
      "}"
    "});"
    "document.addEventListener('keyup',e=>setKey(e.key.toLowerCase(),0));"

    // On-screen WASD buttons
    "function bind(id,k){"
      "const b=document.getElementById(id);"
      "b.addEventListener('mousedown',e=>{if(mode==='drive'){setKey(k,1);e.preventDefault();}});"
      "b.addEventListener('mouseup',  ()=>setKey(k,0));"
      "b.addEventListener('mouseleave',()=>setKey(k,0));"
      "b.addEventListener('touchstart',e=>{if(mode==='drive'){setKey(k,1);e.preventDefault();}},"
        "{passive:false});"
      "b.addEventListener('touchend',  ()=>setKey(k,0));"
      "b.addEventListener('touchcancel',()=>setKey(k,0));"
    "}"
    "['w','a','s','d'].forEach(k=>bind('b'+k,k));"

    "const st=document.getElementById('status'),"
      "chargePwr=document.getElementById('chargePwr');"
    "chargePwr.oninput=()=>document.getElementById('cpv').textContent=chargePwr.value+'%';"

    // HTTP helper — 300 ms timeout
    "function req(url){"
      "fetch(url,{signal:AbortSignal.timeout(300)})"
        ".then(()=>st.textContent='\u2713 '+url.split('?')[0])"
        ".catch(()=>st.textContent='timeout');"
    "}"

    // Drive poll — only fires in drive mode, always full throttle
    "setInterval(()=>{"
      "if(mode==='drive')"
        "req('/drive?lx='+lx+'&ly='+ly+'&throttle=100');"
    "},100);"

    // Charge toggle — click to start, click again to stop
    "let charging=false;"
    "document.getElementById('btnCharge').onclick=function(){"
      "charging=!charging;"
      "this.textContent=charging?'CHARGING...':'CHARGE';"
      "this.style.background=charging?'#4fc3f7':'#0f3460';"
      "this.style.color=charging?'#000':'#4fc3f7';"
      "req(charging?'/charge?power='+chargePwr.value:'/stop');"
    "};"

    // Shoot — always same direction as charge; resets charge button state
    "document.getElementById('btnShoot').onclick=function(){"
      "charging=false;"
      "const cb=document.getElementById('btnCharge');"
      "cb.textContent='CHARGE';cb.style.background='#0f3460';cb.style.color='#4fc3f7';"
      "req('/shoot');"
    "};"

    "document.getElementById('btnStop').onclick=function(){"
      "charging=false;"
      "const cb=document.getElementById('btnCharge');"
      "cb.textContent='CHARGE';cb.style.background='#0f3460';cb.style.color='#4fc3f7';"
      "req('/stop');"
    "};"

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

    // Joystick → differential drive mixing
    int leftSpd  = constrain((ly + lx) * thr / 100, -245, 245);
    int rightSpd = constrain((ly - lx) * thr / 100, -245, 245);

    // Apply per-motor direction flip and right-motor power trim
    leftSpd  = constrain(leftSpd  * LEFT_FLIP,                    -255, 255);
    rightSpd = constrain(rightSpd * RIGHT_FLIP * RIGHT_TRIM / 100, -255, 255);

    // Apply deadband only to drive motors (not catapult)
    leftSpd  = applyDeadband(leftSpd);
    rightSpd = applyDeadband(rightSpd);

    setMotor(ENA, IN1, IN2, leftSpd);
    setMotor(ENB, IN3, IN4, rightSpd);
    lastDriveMs = millis();

    // Log only when actually moving (avoids flooding serial at idle)
    static int prevLx = 0, prevLy = 0;
    if ((lx != prevLx || ly != prevLy) && (lx != 0 || ly != 0)) {
      Serial.print(F("[DRIVE] lx=")); Serial.print(lx);
      Serial.print(F(" ly=")); Serial.print(ly);
      Serial.print(F(" thr=")); Serial.print(thr);
      Serial.print(F(" L=")); Serial.print(leftSpd);
      Serial.print(F(" R=")); Serial.println(rightSpd);
    }
    prevLx = lx; prevLy = ly;

    sendOK(client, F("ok"));
  }
  else if (line.indexOf("/charge") >= 0) {
    // ── Continuous charge at slider-controlled speed ───────────────────────
    int power = parseParam(line, "power", 50);
    int pwm   = map(constrain(power, 0, 100), 0, 100, 0, 255); // max = 40% of 255
    Serial.print(F("[CHARGE] power=")); Serial.print(power);
    Serial.print(F(" pwm=")); Serial.println(pwm);
    setMotor(ENA2, IN5, IN6, pwm * CATAPULT_FLIP);
    sendOK(client, F("charging"));
  }
  else if (line.indexOf("/shoot") >= 0) {
    // ── Snap burst: full speed for CATAPULT_SHOOT_MS then stop ────────────
    Serial.print(F("[SHOOT] snap ")); Serial.print(CATAPULT_SHOOT_MS); Serial.println(F("ms"));
    sendOK(client, F("shooting"));
    client.flush();
    client.stop();
    setMotor(ENA2, IN5, IN6, CATAPULT_SHOOT_PWM * CATAPULT_FLIP);
    delay(CATAPULT_SHOOT_MS);
    setMotor(ENA2, IN5, IN6, 0);
    lastDriveMs = millis();
    return;
  }
  else if (line.indexOf("/stop") >= 0) {
    // ── Emergency stop ────────────────────────────────────────────────────
    Serial.println(F("[STOP] all motors"));
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
