#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>

#define DHTPIN 4       // GPIO pin connected to DHT11 Data pin
#define DHTTYPE DHT11  // Sensor type

const char *ssid = "Smart Home";  // open AP, no password

WebServer server(80);
DHT dht(DHTPIN, DHTTYPE);

// ---------- Sensor cache (updated only from loop, never from HTTP handlers) ----------
const unsigned long SAMPLE_INTERVAL_MS = 2000;   // normal period (DHT11 needs >= 1 s)
const unsigned long RETRY_INTERVAL_MS  = 1000;   // retry period after a failed read
const unsigned long STALE_AFTER_MS     = 10000;  // flag data as invalid after 10 s without a good read

float lastT = NAN, lastH = NAN;
unsigned long lastGoodMs = 0;
unsigned long lastSampleMs = 0;
unsigned long nextSampleDelay = SAMPLE_INTERVAL_MS;
bool haveGoodReading = false;
uint8_t failCount = 0;

// ---------- Retro AI terminal UI (stored in flash, not built with String) ----------
const char PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SMART HOME // CORE</title>
<style>
:root{--g:#33ff66;--dim:#17a23f;--amber:#ffb000;--red:#ff4545;--bg:#010804}
*{box-sizing:border-box}
html,body{margin:0;height:100%}
body{
  background:radial-gradient(ellipse at center,#04170a 0%,var(--bg) 75%);
  color:var(--g);font-family:"Courier New",Courier,monospace;
  display:flex;align-items:center;justify-content:center;
  text-shadow:0 0 6px rgba(51,255,102,.55);
  animation:flick 4s infinite;
}
/* CRT scanlines + vignette */
body:before{content:"";position:fixed;inset:0;pointer-events:none;z-index:2;
  background:repeating-linear-gradient(0deg,rgba(0,0,0,.28) 0,rgba(0,0,0,.28) 1px,transparent 1px,transparent 3px)}
body:after{content:"";position:fixed;inset:0;pointer-events:none;z-index:3;
  box-shadow:inset 0 0 120px rgba(0,0,0,.85)}
@keyframes flick{0%,100%{opacity:1}92%{opacity:1}93%{opacity:.93}94%{opacity:1}97%{opacity:.96}}
.term{
  width:min(94vw,480px);padding:18px 20px;border:2px solid var(--g);
  box-shadow:0 0 22px rgba(51,255,102,.3),inset 0 0 30px rgba(51,255,102,.07);
  background:rgba(0,12,4,.8);position:relative;z-index:1;
}
.top{display:flex;justify-content:space-between;font-size:12px;color:var(--dim);
  border-bottom:1px dashed var(--dim);padding-bottom:8px;margin-bottom:12px}
h1{font-size:20px;margin:0 0 4px;letter-spacing:3px;color:var(--g)}
.sub{font-size:12px;color:var(--dim);margin-bottom:14px}
.boot{font-size:12px;line-height:1.6;color:var(--dim);margin-bottom:14px}
.boot b{color:var(--g);font-weight:normal}
.row{margin:14px 0}
.lbl{font-size:12px;letter-spacing:2px;color:var(--dim)}
.val{font-size:34px;color:var(--amber);text-shadow:0 0 8px rgba(255,176,0,.6)}
.bar{font-size:14px;letter-spacing:1px;color:var(--g);word-break:break-all}
.stat{margin-top:16px;padding-top:10px;border-top:1px dashed var(--dim);font-size:13px}
.ok{color:var(--g)} .bad{color:var(--red);text-shadow:0 0 6px rgba(255,69,69,.6)}
.cur{display:inline-block;width:9px;height:15px;background:var(--g);vertical-align:-2px;
  animation:blink 1s steps(1) infinite}
@keyframes blink{50%{opacity:0}}
</style></head>
<body><div class="term">
  <div class="top"><span>NODE: ESP32-AP</span><span id="up">UPLINK: --</span></div>
  <h1>SMART HOME</h1>
  <div class="sub">ENVIRONMENT NEURAL CORE v1.1</div>
  <div class="boot">
    &gt; <b>BOOT SEQUENCE</b> .......... OK<br>
    &gt; <b>SENSOR BUS (DHT11)</b> .... ONLINE<br>
    &gt; <b>TELEMETRY LOOP</b> ........ ACTIVE
  </div>
  <div class="row">
    <div class="lbl">&gt; TEMPERATURE</div>
    <div class="val"><span id="t">--</span> &deg;C</div>
    <div class="bar" id="tb"></div>
  </div>
  <div class="row">
    <div class="lbl">&gt; HUMIDITY</div>
    <div class="val"><span id="h">--</span> %</div>
    <div class="bar" id="hb"></div>
  </div>
  <div class="stat">&gt; STATUS: <span id="s" class="ok">INITIALIZING</span> <span class="cur"></span></div>
</div>
<script>
function bar(v,max){
  var n=Math.round(Math.max(0,Math.min(v,max))/max*20);
  return '['+'\u2588'.repeat(n)+'\u2591'.repeat(20-n)+']';
}
function setStatus(txt,good){
  var s=document.getElementById('s');
  s.textContent=txt;
  s.className=good?'ok':'bad';
}
function poll(){
  fetch('/data',{cache:'no-store'})
    .then(function(r){return r.json();})
    .then(function(d){
      document.getElementById('up').textContent='UPLINK: OK';
      if(d.ok){
        document.getElementById('t').textContent=d.t;
        document.getElementById('h').textContent=d.h;
        document.getElementById('tb').textContent=bar(d.t,50);
        document.getElementById('hb').textContent=bar(d.h,100);
        setStatus('NOMINAL  [DATA AGE '+d.age+'s]',true);
      }else{
        document.getElementById('t').textContent='--';
        document.getElementById('h').textContent='--';
        document.getElementById('tb').textContent='';
        document.getElementById('hb').textContent='';
        setStatus('SENSOR FAULT - NO VALID DATA',false);
      }
    })
    .catch(function(){
      document.getElementById('up').textContent='UPLINK: LOST';
      setStatus('LINK LOST - RETRYING',false);
    });
}
poll();
setInterval(poll,2000);
</script>
</body></html>)rawliteral";

// ---------- Sensor sampling (non-blocking, called from loop) ----------
void sampleSensor() {
  // One forced bus transaction only. readTemperature()/readHumidity() below then
  // return the values cached by this read instead of hitting the sensor again.
  if (!dht.read(true)) {
    failCount++;
    nextSampleDelay = RETRY_INTERVAL_MS;  // retry in 1 s, without blocking the server
    Serial.printf("DHT read failed (%u)\n", failCount);
    return;
  }

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    failCount++;
    nextSampleDelay = RETRY_INTERVAL_MS;
    Serial.printf("DHT returned NaN (%u)\n", failCount);
    return;
  }

  lastH = h;
  lastT = t;
  lastGoodMs = millis();
  haveGoodReading = true;
  failCount = 0;
  nextSampleDelay = SAMPLE_INTERVAL_MS;
  Serial.printf("T=%.0f C  H=%.0f %%\n", t, h);
}

// ---------- HTTP handlers ----------
void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", PAGE);
}

void handleData() {
  bool fresh = haveGoodReading && (millis() - lastGoodMs < STALE_AFTER_MS);
  char buf[96];

  if (fresh) {
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"t\":%.0f,\"h\":%.0f,\"age\":%lu}",
             lastT, lastH, (millis() - lastGoodMs) / 1000UL);
  } else {
    snprintf(buf, sizeof(buf), "{\"ok\":false}");
  }

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

void handleNotFound() {
  server.send(404, "text/plain", "404");
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);

  // Initialize DHT sensor and allow time to warm up
  dht.begin();
  delay(2000);

  // Start open Wi-Fi AP (no password)
  if (!WiFi.softAP(ssid)) {
    Serial.println("softAP failed to start");
    while (true) delay(1000);
  }
  IPAddress IP = WiFi.softAPIP();

  Serial.println("\n--- Access Point Started ---");
  Serial.print("SSID: ");
  Serial.println(ssid);
  Serial.print("IP Address: ");
  Serial.println(IP);

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP Server started");

  sampleSensor();  // prime first reading
  lastSampleMs = millis();
}

void loop() {
  server.handleClient();

  if (millis() - lastSampleMs >= nextSampleDelay) {
    lastSampleMs = millis();
    sampleSensor();
  }
}
