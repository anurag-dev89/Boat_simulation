/*
  River Waste Collection Boat - ESP32 firmware (simple version, no camera/AI)

  What it does
  - Creates its own Wi-Fi network (AP mode). Connect your phone, open http://192.168.4.1
  - Web page: hold-to-drive buttons, speed slider, conveyor ON/OFF, Auto-collect toggle,
    live status (bin FULL/EMPTY, waste distance, battery).
  - 4 BO motors on one L298N (left pair + right pair), conveyor motors switched by a relay.
  - HC-SR04 at the conveyor intake detects waste; IR sensor on the bin wall detects FULL.

  Safety built in
  - Drive command times out after 1 s -> motors stop.
  - No contact from the phone for 4 s -> everything stops (motors + conveyor).
  - Bin FULL -> conveyor stops, forward driving is blocked (reverse/turn still allowed).
  - Optional low-battery cutoff (off by default).

  Board: "ESP32 Dev Module" in Arduino IDE (ESP32 core by Espressif, v2.x or v3.x).
  Not compiled or tested on hardware by the author of this file - test each part
  step by step as described in wiring_guide.md.
*/

#include <WiFi.h>
#include <WebServer.h>

// ===================== Wi-Fi =====================
const char* AP_SSID = "RiverBoat";
const char* AP_PASS = "boat12345";      // at least 8 characters

// ===================== PINS =====================
#define PIN_L_EN    25   // L298N ENA  (left motors speed, PWM)
#define PIN_L_IN1   26   // L298N IN1
#define PIN_L_IN2   27   // L298N IN2
#define PIN_R_EN    13   // L298N ENB  (right motors speed, PWM)
#define PIN_R_IN3   32   // L298N IN3
#define PIN_R_IN4   33   // L298N IN4
#define PIN_RELAY   18   // relay module IN (active LOW) -> conveyor
#define PIN_TRIG     5   // HC-SR04 TRIG
#define PIN_ECHO    34   // HC-SR04 ECHO (through 1k/2k divider!)
#define PIN_BIN_IR  35   // bin IR sensor OUT
#define PIN_BATT    36   // optional battery divider (10k top / 4.7k bottom)

// ===================== SETTINGS (tune these) =====================
#define LEFT_INVERT          false   // set true if the left side drives backwards
#define RIGHT_INVERT         false   // set true if the right side drives backwards
#define BIN_ACTIVE_LEVEL     LOW     // FC-51 style IR sensors output LOW when they see something
#define RELAY_FLOAT_OFF      true    // true: turn relay OFF by making the pin an input (fixes
                                     // "relay stuck on" with 3.3V logic). false: use HIGH.
#define WASTE_TRIGGER_CM     20      // waste is "detected" when closer than this
#define WASTE_HITS_NEEDED    3       // consecutive close readings needed (avoids ripple false alarms)
#define CONVEYOR_RUN_MS      10000   // auto-collect: how long the conveyor runs
#define AUTO_COOLDOWN_MS     3000    // wait after a collect cycle before the next one
#define MOVE_TIMEOUT_MS      1000    // stop motors if no drive command for this long
#define LINK_TIMEOUT_MS      4000    // stop everything if the phone page is silent this long
#define BIN_CONFIRM_MS       1500    // IR must stay steady this long to change FULL/EMPTY
#define PWM_FREQ             1000
#define PWM_BITS             8
#define DEFAULT_SPEED_PCT    80

// Optional low-battery cutoff (needs the 10k/4.7k divider on PIN_BATT)
#define USE_BATTERY_CUTOFF   false
#define BATT_DIVIDER_RATIO   3.1277  // (10k + 4.7k) / 4.7k
#define BATT_CAL             1.00    // adjust so the page matches your multimeter
#define BATT_CUTOFF_V        6.6     // 2S pack: 3.3 V per cell

// PWM channels (used on ESP32 core v2.x)
#define CH_LEFT   0
#define CH_RIGHT  1

// ===================== STATE =====================
WebServer server(80);

int  speedDuty   = (DEFAULT_SPEED_PCT * 255) / 100;
char lastMove    = 'S';
bool moving      = false;
bool convOn      = false;
bool autoMode    = false;
bool binFull     = false;
bool binRaw      = false;
bool wasteNear   = false;
bool lowBattery  = false;
int  distanceCm  = -1;
int  wasteHits   = 0;
float battVolts  = 0;

unsigned long lastMoveCmd   = 0;
unsigned long lastContact   = 0;
unsigned long binChangeAt   = 0;
unsigned long lastSensorAt  = 0;
unsigned long lastPrintAt   = 0;

enum AutoState { AUTO_IDLE, AUTO_RUNNING };
AutoState autoState = AUTO_IDLE;
unsigned long autoUntil = 0;
unsigned long autoCooldownUntil = 0;

// ===================== WEB PAGE =====================
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>RiverBoat</title>
<style>
body{font-family:sans-serif;background:#10202b;color:#eee;text-align:center;margin:0;padding:12px}
h2{margin:6px 0}
.card{background:#1b3342;border-radius:12px;padding:12px;margin:10px auto;max-width:420px}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
button{padding:18px 0;font-size:18px;border:0;border-radius:10px;background:#2e7da6;color:#fff;touch-action:none;user-select:none;-webkit-user-select:none}
button:active{background:#58b4de}
.stop{background:#c0392b}
.on{background:#27ae60}
.warn{color:#ff6b6b;font-weight:bold}
.ok{color:#7be07b;font-weight:bold}
input[type=range]{width:90%}
.row{display:flex;gap:8px}
.row button{flex:1}
</style></head><body>
<h2>RiverBoat Control</h2>

<div class="card">
  <div>Bin: <span id="bin">--</span></div>
  <div>Waste distance: <span id="dist">--</span> cm <span id="waste"></span></div>
  <div>Battery: <span id="batt">--</span></div>
  <div id="msg" class="warn"></div>
</div>

<div class="card">
  <div class="grid">
    <div></div><button id="bF">&#9650; Fwd</button><div></div>
    <button id="bL">&#9664; Left</button><button class="stop" id="bS">STOP</button><button id="bR">Right &#9654;</button>
    <div></div><button id="bB">&#9660; Back</button><div></div>
  </div>
  <p>Speed: <span id="spv">80</span>%</p>
  <input type="range" id="sp" min="40" max="100" value="80">
</div>

<div class="card">
  <div class="row">
    <button id="bConv">Conveyor: OFF</button>
    <button id="bAuto">Auto-collect: OFF</button>
  </div>
</div>

<script>
var hold=null;
function send(u){fetch(u).catch(function(){});}
function press(c){send('/cmd?c='+c);if(hold)clearInterval(hold);hold=setInterval(function(){send('/cmd?c='+c);},300);}
function release(){if(hold){clearInterval(hold);hold=null;}send('/cmd?c=S');}
function bind(id,c){
  var b=document.getElementById(id);
  b.addEventListener('pointerdown',function(e){e.preventDefault();press(c);});
  ['pointerup','pointerleave','pointercancel'].forEach(function(ev){b.addEventListener(ev,release);});
  b.addEventListener('contextmenu',function(e){e.preventDefault();});
}
bind('bF','F');bind('bB','B');bind('bL','L');bind('bR','R');
document.getElementById('bS').addEventListener('click',function(){send('/cmd?c=S');send('/conv?on=0');send('/auto?on=0');});
var conv=false,auto=false;
document.getElementById('bConv').addEventListener('click',function(){send('/conv?on='+(conv?0:1));});
document.getElementById('bAuto').addEventListener('click',function(){send('/auto?on='+(auto?0:1));});
var sp=document.getElementById('sp');
sp.addEventListener('input',function(){document.getElementById('spv').textContent=sp.value;});
sp.addEventListener('change',function(){send('/speed?v='+sp.value);});
function poll(){
  fetch('/status').then(function(r){return r.json();}).then(function(s){
    var bin=document.getElementById('bin');
    bin.textContent=s.bin; bin.className=(s.bin=='FULL')?'warn':'ok';
    document.getElementById('dist').textContent=(s.dist<0)?'--':s.dist;
    document.getElementById('waste').textContent=s.waste?'(WASTE!)':'';
    document.getElementById('batt').textContent=(s.batt>0)?s.batt.toFixed(2)+' V':'not monitored';
    conv=s.conv;auto=s.auto;
    var bc=document.getElementById('bConv');bc.textContent='Conveyor: '+(conv?'ON':'OFF');bc.className=conv?'on':'';
    var ba=document.getElementById('bAuto');ba.textContent='Auto-collect: '+(auto?'ON':'OFF');ba.className=auto?'on':'';
    var m='';
    if(s.bin=='FULL')m='BIN FULL - empty it. Forward and conveyor blocked.';
    if(s.low)m='LOW BATTERY - boat locked. Switch off.';
    document.getElementById('msg').textContent=m;
  }).catch(function(){document.getElementById('msg').textContent='No connection to boat';});
}
setInterval(poll,500);poll();
</script></body></html>
)rawliteral";

// ===================== PWM HELPERS =====================
void pwmInit(int pin, int ch) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(pin, PWM_FREQ, PWM_BITS);
#else
  ledcSetup(ch, PWM_FREQ, PWM_BITS);
  ledcAttachPin(pin, ch);
#endif
}

void pwmWrite(int pin, int ch, int duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(pin, duty);
#else
  ledcWrite(ch, duty);
#endif
}

// ===================== MOTORS =====================
// speed: -255 (full reverse) ... 0 (stop) ... +255 (full forward)
void driveSide(int enPin, int ch, int a, int b, int spd) {
  spd = constrain(spd, -255, 255);
  if (spd > 0)      { digitalWrite(a, HIGH); digitalWrite(b, LOW); }
  else if (spd < 0) { digitalWrite(a, LOW);  digitalWrite(b, HIGH); }
  else              { digitalWrite(a, LOW);  digitalWrite(b, LOW); }
  pwmWrite(enPin, ch, abs(spd));
}

void applyMotion(int left, int right) {
  if (LEFT_INVERT)  left  = -left;
  if (RIGHT_INVERT) right = -right;
  driveSide(PIN_L_EN, CH_LEFT,  PIN_L_IN1, PIN_L_IN2, left);
  driveSide(PIN_R_EN, CH_RIGHT, PIN_R_IN3, PIN_R_IN4, right);
}

void stopMotors() {
  applyMotion(0, 0);
  moving = false;
  lastMove = 'S';
}

void doMove(char c) {
  if (lowBattery) { stopMotors(); return; }
  if (c == 'F' && binFull) { stopMotors(); return; }   // bin full: no more forward
  int s = speedDuty;
  switch (c) {
    case 'F': applyMotion(s, s);          break;
    case 'B': applyMotion(-s, -s);        break;
    case 'L': applyMotion(-s / 2, s);     break;   // pivot left
    case 'R': applyMotion(s, -s / 2);     break;   // pivot right
    default:  applyMotion(0, 0); c = 'S'; break;
  }
  lastMove = c;
  moving = (c != 'S');
}

// ===================== CONVEYOR (relay, active LOW) =====================
void setConveyor(bool on) {
  if (on && (binFull || lowBattery)) on = false;
  convOn = on;
  if (on) {
    pinMode(PIN_RELAY, OUTPUT);
    digitalWrite(PIN_RELAY, LOW);          // LOW = relay ON
  } else {
    if (RELAY_FLOAT_OFF) {
      pinMode(PIN_RELAY, INPUT);           // module's own pull-up turns it OFF
    } else {
      pinMode(PIN_RELAY, OUTPUT);
      digitalWrite(PIN_RELAY, HIGH);
    }
  }
}

// ===================== SENSORS =====================
int readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long d = pulseIn(PIN_ECHO, HIGH, 25000UL);   // 25 ms timeout (~4 m)
  if (d == 0) return -1;
  return (int)(d * 0.0343f / 2.0f);
}

float readBatteryVolts() {
  long sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_BATT);
  float mv = sum / 8.0f;
  return (mv / 1000.0f) * BATT_DIVIDER_RATIO * BATT_CAL;
}

void updateSensors(unsigned long now) {
  // Bin IR: FULL/EMPTY only changes after the reading is steady for BIN_CONFIRM_MS
  bool raw = (digitalRead(PIN_BIN_IR) == BIN_ACTIVE_LEVEL);
  if (raw != binRaw) { binRaw = raw; binChangeAt = now; }
  if (now - binChangeAt >= BIN_CONFIRM_MS) binFull = binRaw;

  // Ultrasonic every 100 ms
  if (now - lastSensorAt >= 100) {
    lastSensorAt = now;
    distanceCm = readDistanceCm();
    if (distanceCm >= 2 && distanceCm < WASTE_TRIGGER_CM) wasteHits++;
    else wasteHits = 0;
    wasteNear = (wasteHits >= WASTE_HITS_NEEDED);

    if (USE_BATTERY_CUTOFF) {
      battVolts = readBatteryVolts();
      if (battVolts > 3.0 && battVolts < BATT_CUTOFF_V) lowBattery = true;   // latched
    }
  }
}

// ===================== AUTO-COLLECT =====================
void updateAutoCollect(unsigned long now) {
  if (autoState == AUTO_IDLE) {
    if (autoMode && wasteNear && !binFull && !lowBattery && now > autoCooldownUntil) {
      stopMotors();                       // hold still while collecting
      setConveyor(true);
      autoUntil = now + CONVEYOR_RUN_MS;
      autoState = AUTO_RUNNING;
    }
  } else {  // AUTO_RUNNING
    if (!autoMode || binFull || lowBattery || now > autoUntil) {
      setConveyor(false);
      autoState = AUTO_IDLE;
      autoCooldownUntil = now + AUTO_COOLDOWN_MS;
    }
  }
}

// ===================== SAFETY =====================
void updateSafety(unsigned long now) {
  if (moving && (now - lastMoveCmd > MOVE_TIMEOUT_MS)) stopMotors();

  if (now - lastContact > LINK_TIMEOUT_MS) {          // phone gone
    if (moving) stopMotors();
    if (convOn) setConveyor(false);
    autoState = AUTO_IDLE;
    autoMode = false;
  }

  if (binFull) {
    if (convOn) setConveyor(false);
    if (moving && lastMove == 'F') stopMotors();
  }

  if (lowBattery) {
    if (moving) stopMotors();
    if (convOn) setConveyor(false);
  }
}

// ===================== WEB HANDLERS =====================
void touch() { lastContact = millis(); }

void handleRoot() {
  touch();
  server.send_P(200, "text/html", PAGE);
}

void handleCmd() {
  touch();
  String c = server.arg("c");
  char ch = c.length() ? c.charAt(0) : 'S';
  doMove(ch);
  lastMoveCmd = millis();
  server.send(200, "text/plain", "ok");
}

void handleConv() {
  touch();
  bool on = server.arg("on") == "1";
  if (autoState == AUTO_RUNNING) autoState = AUTO_IDLE;   // manual overrides auto cycle
  setConveyor(on);
  server.send(200, "text/plain", convOn ? "ON" : "OFF");
}

void handleAuto() {
  touch();
  autoMode = server.arg("on") == "1";
  if (!autoMode && autoState == AUTO_RUNNING) {
    setConveyor(false);
    autoState = AUTO_IDLE;
  }
  server.send(200, "text/plain", autoMode ? "ON" : "OFF");
}

void handleSpeed() {
  touch();
  int pct = constrain(server.arg("v").toInt(), 0, 100);
  speedDuty = (pct * 255) / 100;
  if (moving) doMove(lastMove);
  server.send(200, "text/plain", "ok");
}

void handleStatus() {
  touch();
  char buf[220];
  snprintf(buf, sizeof(buf),
           "{\"bin\":\"%s\",\"dist\":%d,\"waste\":%s,\"conv\":%s,\"auto\":%s,\"batt\":%.2f,\"low\":%s}",
           binFull ? "FULL" : "EMPTY",
           distanceCm,
           wasteNear ? "true" : "false",
           convOn ? "true" : "false",
           autoMode ? "true" : "false",
           USE_BATTERY_CUTOFF ? battVolts : 0.0f,
           lowBattery ? "true" : "false");
  server.send(200, "application/json", buf);
}

// ===================== SETUP / LOOP =====================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_L_IN1, OUTPUT);
  pinMode(PIN_L_IN2, OUTPUT);
  pinMode(PIN_R_IN3, OUTPUT);
  pinMode(PIN_R_IN4, OUTPUT);
  pwmInit(PIN_L_EN, CH_LEFT);
  pwmInit(PIN_R_EN, CH_RIGHT);
  stopMotors();

  setConveyor(false);                // relay OFF at boot

  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BIN_IR, INPUT);        // GPIO35 has no internal pull-up; the IR module drives it

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("Wi-Fi network: ");
  Serial.println(AP_SSID);
  Serial.print("Open http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/cmd", handleCmd);
  server.on("/conv", handleConv);
  server.on("/auto", handleAuto);
  server.on("/speed", handleSpeed);
  server.on("/status", handleStatus);
  server.begin();

  lastContact = millis();
  binChangeAt = millis();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  updateSensors(now);
  updateAutoCollect(now);
  updateSafety(now);

  // Debug print once per second
  if (now - lastPrintAt >= 1000) {
    lastPrintAt = now;
    Serial.printf("dist=%d cm  waste=%d  bin=%s  conv=%d  auto=%d  move=%c  batt=%.2f\n",
                  distanceCm, wasteNear, binFull ? "FULL" : "EMPTY",
                  convOn, autoMode, lastMove, battVolts);
  }
}
