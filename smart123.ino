/*
  Smart home - ESP32 + DHT11 temperature / humidity monitor

  - ESP32 creates an OPEN Wi-Fi network called "Smart home"
  - Connect to it, open http://192.168.4.1 in a browser
  - Page shows temperature and humidity, updating every 2 seconds
  - Readings are median-filtered (last 5 good samples) so they stay stable
  - Shows "No signal" when:
      * the DHT11 wire is disconnected / sensor stops answering
      * the ESP32 loses power
      * the phone/PC is not connected to the "Smart home" network
  - Returns to normal automatically when the problem is fixed

  Wiring (3-pin DHT11 module):
      DHT11 VCC  -> ESP32 3V3
      DHT11 GND  -> ESP32 GND
      DHT11 DATA -> ESP32 GPIO 4
  If you use the bare 4-pin DHT11, add a 10k resistor between DATA and 3V3.

  Library needed (Library Manager):
      "DHT sensor library" by Adafruit  (+ "Adafruit Unified Sensor" if asked)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>

// ---------------- Settings ----------------
#define DHTPIN   4
#define DHTTYPE  DHT11

const char* AP_SSID = "Smart home";          // open network, no password

// The Adafruit library caches results for 2000 ms. Polling slightly slower than
// that guarantees every poll is a real sensor read, never a cached (old) value.
const unsigned long READ_INTERVAL = 2200;
const unsigned long STALE_AFTER   = 8000;    // no good reading for this long -> No signal
const uint8_t       FAIL_LIMIT    = 3;       // consecutive bad reads before No signal
const uint8_t       WINDOW        = 5;       // samples used by the median filter

// Calibration: compare with a trusted thermometer/hygrometer, then adjust.
// Example: DHT11 shows 29 C but the real temperature is 27 C -> TEMP_OFFSET = -2.0
const float TEMP_OFFSET = 0.0;
const float HUM_OFFSET  = 0.0;

// ---------------- Globals ----------------
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);

float   tempBuf[WINDOW];
float   humBuf[WINDOW];
uint8_t bufCount = 0;
uint8_t bufIdx   = 0;

float stableTemp = 0;
float stableHum  = 0;

bool          sensorOk   = false;   // false until the first good reading
uint8_t       failCount  = 0;
unsigned long lastGoodMs = 0;
unsigned long lastReadMs = 0;

// ---------------- Web page ----------------
static const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart home</title>
<style>
  /* Theme #368025250: Dark Blue, stacked cards, serif */
  :root{
    --bg:hsl(204,30%,4%);       /* page background */
    --card:hsl(204,27%,7%);     /* card background */
    --text:hsl(204,25%,93%);    /* values and title */
    --sub:hsl(204,15%,67%);     /* labels and status text */
    --ok:hsl(144,78%,62%);      /* card outline, live dot */
    --bad:hsl(2,85%,64%);       /* No signal */
  }
  body{font-family:Georgia,serif;background:var(--bg);color:var(--text);margin:0;
       display:flex;flex-direction:column;align-items:center;padding:24px}
  h1{font-weight:400;margin:0 0 20px}
  .card{box-sizing:border-box;width:100%;max-width:320px;background:var(--card);
        border:1px solid var(--ok);border-radius:24px;padding:22px 28px;margin:8px;
        text-align:center}
  .label{font-size:14px;color:var(--sub);text-transform:uppercase;letter-spacing:1px}
  .value{font-size:48px;margin-top:8px}
  .nosig{border-color:var(--bad)}
  .nosig .value{color:var(--bad)}
  #status{margin-top:16px;font-size:14px;color:var(--sub)}
  #status.nosig-text{color:var(--bad)}
  .dot{display:inline-block;width:10px;height:10px;border-radius:50%;
       background:var(--ok);margin-right:6px}
  .nosig-dot{background:var(--bad)}
</style>
</head>
<body>
<h1>Smart home</h1>
<div class="card" id="tcard"><div class="label">Temperature</div><div class="value" id="t">--</div></div>
<div class="card" id="hcard"><div class="label">Humidity</div><div class="value" id="h">--</div></div>
<div id="status"><span class="dot nosig-dot" id="dot"></span><span id="stext">Connecting...</span></div>
<script>
const t=document.getElementById('t'), h=document.getElementById('h');
const tc=document.getElementById('tcard'), hc=document.getElementById('hcard');
const dot=document.getElementById('dot'), st=document.getElementById('stext');
const box=document.getElementById('status');

function showOk(temp,hum){
  t.textContent=temp.toFixed(1)+' \u00B0C';
  h.textContent=hum.toFixed(0)+' %';
  tc.classList.remove('nosig'); hc.classList.remove('nosig');
  dot.classList.remove('nosig-dot'); box.classList.remove('nosig-text');
  st.textContent='Live';
}
function showNoSignal(){
  t.textContent='No signal'; h.textContent='No signal';
  tc.classList.add('nosig'); hc.classList.add('nosig');
  dot.classList.add('nosig-dot'); box.classList.add('nosig-text');
  st.textContent='No signal';
}

async function update(){
  const ctrl=new AbortController();
  const timer=setTimeout(()=>ctrl.abort(),2500);
  try{
    const r=await fetch('/data',{cache:'no-store',signal:ctrl.signal});
    if(!r.ok) throw new Error('http');
    const d=await r.json();
    if(d.ok) showOk(d.t,d.h); else showNoSignal();
  }catch(e){
    showNoSignal();          // ESP32 off, Wi-Fi lost, or request timed out
  }finally{
    clearTimeout(timer);
    setTimeout(update,2000); // next poll only after this one finished
  }
}
update();
</script>
</body>
</html>
)rawliteral";

// ---------------- Median filter helpers ----------------
float median(const float* src, uint8_t n) {
  float tmp[WINDOW];
  for (uint8_t i = 0; i < n; i++) tmp[i] = src[i];
  for (uint8_t i = 1; i < n; i++) {             // insertion sort
    float key = tmp[i];
    int8_t j = i - 1;
    while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = key;
  }
  if (n % 2) return tmp[n / 2];
  return (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0f;
}

void pushSample(float t, float h) {
  tempBuf[bufIdx] = t;
  humBuf[bufIdx]  = h;
  bufIdx = (bufIdx + 1) % WINDOW;
  if (bufCount < WINDOW) bufCount++;
  stableTemp = median(tempBuf, bufCount);
  stableHum  = median(humBuf, bufCount);
}

// ---------------- Sensor ----------------
void readSensor() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();   // Celsius

  bool valid = !isnan(h) && !isnan(t) &&
               h >= 0 && h <= 100 &&
               t >= -20 && t <= 80;

  if (valid) {
    failCount  = 0;
    lastGoodMs = millis();
    Serial.printf("Raw: %.1f C  %.1f %%\n", t, h);   // what the sensor really reports
    pushSample(t + TEMP_OFFSET, constrain(h + HUM_OFFSET, 0.0f, 100.0f));
    if (!sensorOk) {
      sensorOk = true;
      Serial.println("Sensor OK");
    }
  } else {
    if (failCount < 255) failCount++;
    Serial.printf("DHT read failed (%u/%u)\n", failCount, FAIL_LIMIT);
    if (failCount >= FAIL_LIMIT && sensorOk) {
      sensorOk = false;
      bufCount = 0;                  // drop old samples so recovery starts clean
      bufIdx   = 0;
      Serial.println("Sensor lost -> No signal");
    }
  }
}

// ---------------- HTTP handlers ----------------
void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", PAGE);
}

void handleData() {
  bool ok = sensorOk && (millis() - lastGoodMs < STALE_AFTER);
  String json;
  if (ok) {
    json = "{\"ok\":true,\"t\":" + String(stableTemp, 1) +
           ",\"h\":" + String(stableHum, 0) + "}";
  } else {
    json = "{\"ok\":false}";
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(200);

  dht.begin();

  WiFi.mode(WIFI_AP);
  IPAddress ip(192, 168, 4, 1), gw(192, 168, 4, 1), mask(255, 255, 255, 0);
  WiFi.softAPConfig(ip, gw, mask);

  // open network: no password, channel 1, visible, max 4 clients
  if (!WiFi.softAP(AP_SSID, nullptr, 1, 0, 4)) {
    Serial.println("Failed to start access point, restarting...");
    delay(1000);
    ESP.restart();
  }

  Serial.print("Access point \"");
  Serial.print(AP_SSID);
  Serial.print("\" started. Open http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  server.handleClient();

  if (millis() - lastReadMs >= READ_INTERVAL) {
    lastReadMs = millis();
    readSensor();
  }
}
