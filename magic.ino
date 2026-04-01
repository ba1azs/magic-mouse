#include <BleMouse.h>
#include <BLESecurity.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include "esp_sleep.h"
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Preferences.h>
#include <math.h>

BleMouse bleMouse("Magic Mouse", "Apple", 100);
WebServer server(80);
RTC_DS3231 rtc;
Adafruit_MPU6050 mpu;
Preferences prefs;

// ---------------- SETTINGS ----------------

const unsigned long jiggleInterval = 60000;
const unsigned long sleepTimeout = 300000;
const unsigned long hotspotStopDelay = 30000;

const char* apSSID = "Magic Mouse";
const char* apPassword = "";

// I2C pins
const int SDA_PIN = 8;
const int SCL_PIN = 9;

// I2C addresses
const uint8_t MPU_ADDR = 0x69;

// Vibration motor pin
const int VIBRATION_PIN = 4;

// GPIO list for webpage status
const int gpioList[] = {0, 1, 2, 3, 5, 6, 7, 10, 20, 21};
const int gpioCount = sizeof(gpioList) / sizeof(gpioList[0]);

// MPU6050 calibration
const float STABLE_ACCEL_DELTA = 0.08f;
const float STABLE_GYRO_DELTA  = 0.03f;
const int   STABLE_SAMPLES     = 40;
const int   CAL_SAMPLES        = 300;

// Gyro mouse control
const float GYRO_MOUSE_DEADZONE = 0.06f;
const float GYRO_MOUSE_SCALE    = 18.0f;
const unsigned long gyroMoveInterval = 12;

// Head-down detection
const float HEAD_DOWN_Z_THRESHOLD = -7.0f;

// ---------------- STATE ----------------

unsigned long lastJiggle = 0;
unsigned long disconnectStart = 0;
unsigned long lastGyroMove = 0;
unsigned long normalOrientationSince = 0;

bool apStarted = false;
bool rtcAvailable = false;
bool mpuAvailable = false;
bool gyroMouseEnabled = false;

// Default / current MPU offsets
float accelOffsetX = 0.8085f;
float accelOffsetY = 0.1241f;
float accelOffsetZ = 0.1855f;

float gyroOffsetX  = -0.0656f;
float gyroOffsetY  = 0.0128f;
float gyroOffsetZ  = 0.0097f;

// ---------------- OTA PAGE ----------------

const char* otaPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>Magic mouse Control Page</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{
  font-family:Arial,sans-serif;
  max-width:420px;
  margin:auto;
  padding:20px;
}
progress{
  width:100%;
  height:20px;
}
#status,#timeStatus,#gyroStatus,#recalStatus{
  margin-top:10px;
  font-weight:bold;
}
.section{
  margin-top:28px;
  padding-top:18px;
  border-top:1px solid #ccc;
}
input,button{
  font-size:16px;
}
input[type="datetime-local"]{
  width:100%;
  padding:8px;
  box-sizing:border-box;
}
button{
  padding:10px 14px;
  cursor:pointer;
  width:100%;
  margin-top:8px;
}
.small{
  font-size:14px;
  color:#555;
  margin-top:6px;
}
.state{
  display:inline-block;
  margin-top:10px;
  padding:6px 10px;
  background:#f2f2f2;
  border-radius:8px;
}
.gpio-row{
  display:flex;
  justify-content:space-between;
  align-items:center;
  padding:8px 10px;
  border:1px solid #ddd;
  border-radius:8px;
  margin-top:8px;
}
.gpio-value{
  font-weight:bold;
  min-width:24px;
  text-align:center;
  padding:4px 8px;
  border-radius:6px;
  background:#f2f2f2;
}
</style>
</head>
<body>

<h2>ESP32 Firmware Update</h2>

<input type="file" id="file">
<br><br>
<button onclick="upload()">Upload Firmware</button>
<br><br>

<progress id="bar" value="0" max="100"></progress>
<div id="status"></div>

<div class="section">
  <h2>Set RTC Time</h2>
  <div class="state" id="rtcNow">Loading RTC time...</div>
  <input type="datetime-local" id="rtcTime">
  <div class="small">Current device RTC time is shown above.</div>
  <button onclick="setTime()">Set Time</button>
  <button onclick="loadRtcTimeIntoInput()">Use device RTC time</button>
  <div id="timeStatus"></div>
</div>

<div class="section">
  <h2>Gyroscope Mouse</h2>
  <div class="small">Use the MPU6050 to move the BLE mouse cursor.</div>
  <div class="state" id="gyroState">Loading state...</div>
  <button onclick="toggleGyro()">Mouse control with gyroscope</button>
  <button onclick="recalibrateGyro()">Recalibrate gyroscope</button>
  <div id="gyroStatus"></div>
  <div id="recalStatus"></div>
</div>

<div class="section">
  <h2>GPIO Status</h2>
  <div class="small">0 = pulled low, 1 = released/high.</div>
  <button onclick="refreshGPIO()">Refresh GPIO states</button>
  <div id="gpioList">Loading GPIO states...</div>
</div>

<script>
function upload(){
  let file=document.getElementById("file").files[0];
  if(!file){
    alert("Select a .bin file first");
    return;
  }

  let xhr=new XMLHttpRequest();

  xhr.upload.addEventListener("progress",function(e){
    if(e.lengthComputable){
      let percent=(e.loaded/e.total)*100;
      document.getElementById("bar").value=percent;
    }
  });

  xhr.onreadystatechange=function(){
    if(xhr.readyState==4){
      document.getElementById("status").innerHTML=xhr.responseText;
    }
  };

  xhr.open("POST","/update",true);
  let formData=new FormData();
  formData.append("firmware",file);
  xhr.send(formData);

  document.getElementById("status").innerHTML="Uploading...";
}

function setTime(){
  let dt = document.getElementById("rtcTime").value;
  if(!dt){
    alert("Select date and time first");
    return;
  }

  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState == 4){
      document.getElementById("timeStatus").innerHTML = xhr.responseText;
      refreshRtcTime();
    }
  };

  xhr.open("POST", "/settime", true);
  xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
  xhr.send("datetime=" + encodeURIComponent(dt));

  document.getElementById("timeStatus").innerHTML = "Setting time...";
}

function refreshRtcTime(){
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState == 4){
      if(xhr.status == 200){
        document.getElementById("rtcNow").innerHTML = "Device RTC: " + xhr.responseText;
      } else {
        document.getElementById("rtcNow").innerHTML = "Device RTC: unavailable";
      }
    }
  };
  xhr.open("GET", "/rtctime", true);
  xhr.send();
}

function loadRtcTimeIntoInput(){
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState == 4 && xhr.status == 200){
      document.getElementById("rtcTime").value = xhr.responseText;
    }
  };
  xhr.open("GET", "/rtctime-local", true);
  xhr.send();
}

function refreshGyroState(){
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState == 4 && xhr.status == 200){
      document.getElementById("gyroState").innerHTML = xhr.responseText;
    }
  };
  xhr.open("GET", "/gyrostate", true);
  xhr.send();
}

function toggleGyro(){
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState == 4){
      document.getElementById("gyroStatus").innerHTML = xhr.responseText;
      refreshGyroState();
    }
  };
  xhr.open("POST", "/togglegyro", true);
  xhr.send();
  document.getElementById("gyroStatus").innerHTML = "Changing state...";
}

function recalibrateGyro(){
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState == 4){
      document.getElementById("recalStatus").innerHTML = xhr.responseText;
      refreshGyroState();
    }
  };
  xhr.open("POST", "/recalibrate", true);
  xhr.send();
  document.getElementById("recalStatus").innerHTML = "Recalibrating... keep device still";
}

function refreshGPIO(){
  let xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function(){
    if(xhr.readyState == 4){
      if(xhr.status == 200){
        let data = JSON.parse(xhr.responseText);
        let html = "";

        for (let i = 0; i < data.length; i++) {
          html += '<div class="gpio-row">';
          html += '<span>GPIO ' + data[i].pin + '</span>';
          html += '<span class="gpio-value">' + data[i].value + '</span>';
          html += '</div>';
        }

        document.getElementById("gpioList").innerHTML = html;
      } else {
        document.getElementById("gpioList").innerHTML = "Failed to load GPIO states";
      }
    }
  };
  xhr.open("GET", "/gpiostate", true);
  xhr.send();
}

window.onload = function(){
  refreshGyroState();
  refreshRtcTime();
  refreshGPIO();
};
</script>

</body>
</html>
)rawliteral";

// ---------------- VIBRATION ----------------

void vibrateOnce(int onMs = 120, int offMs = 0) {
  digitalWrite(VIBRATION_PIN, HIGH);
  delay(onMs);
  digitalWrite(VIBRATION_PIN, LOW);
  if (offMs > 0) delay(offMs);
}

void vibratePattern(int pulses, int onMs = 120, int offMs = 100) {
  for (int i = 0; i < pulses; i++) {
    digitalWrite(VIBRATION_PIN, HIGH);
    delay(onMs);
    digitalWrite(VIBRATION_PIN, LOW);
    if (i < pulses - 1) delay(offMs);
  }
}

void vibrateCalibrationFinished() {
  vibratePattern(1, 300, 0);
}

void vibrateHotspotOn() {
  vibratePattern(2, 120, 120);
}

void vibrateHotspotOff() {
  vibratePattern(3, 80, 100);
}

// ---------------- PERSISTENCE ----------------

void saveCalibrationToFlash() {
  prefs.begin("mpu", false);

  prefs.putBool("valid", true);
  prefs.putFloat("aox", accelOffsetX);
  prefs.putFloat("aoy", accelOffsetY);
  prefs.putFloat("aoz", accelOffsetZ);
  prefs.putFloat("gox", gyroOffsetX);
  prefs.putFloat("goy", gyroOffsetY);
  prefs.putFloat("goz", gyroOffsetZ);

  prefs.end();

  Serial.println("Calibration saved to flash");
}

bool loadCalibrationFromFlash() {
  prefs.begin("mpu", true);

  bool valid = prefs.getBool("valid", false);
  if (valid) {
    accelOffsetX = prefs.getFloat("aox", accelOffsetX);
    accelOffsetY = prefs.getFloat("aoy", accelOffsetY);
    accelOffsetZ = prefs.getFloat("aoz", accelOffsetZ);
    gyroOffsetX  = prefs.getFloat("gox", gyroOffsetX);
    gyroOffsetY  = prefs.getFloat("goy", gyroOffsetY);
    gyroOffsetZ  = prefs.getFloat("goz", gyroOffsetZ);
  }

  prefs.end();

  if (valid) {
    Serial.println("Loaded calibration from flash");
  } else {
    Serial.println("No saved calibration found, using default values");
  }

  Serial.printf("Accel offsets: %.4f %.4f %.4f\n", accelOffsetX, accelOffsetY, accelOffsetZ);
  Serial.printf("Gyro offsets : %.4f %.4f %.4f\n", gyroOffsetX, gyroOffsetY, gyroOffsetZ);

  return valid;
}

// ---------------- RTC ----------------

void printRTCNow() {
  if (!rtcAvailable) return;

  DateTime now = rtc.now();
  Serial.printf(
    "RTC Time: %04d-%02d-%02d %02d:%02d:%02d\n",
    now.year(),
    now.month(),
    now.day(),
    now.hour(),
    now.minute(),
    now.second()
  );
}

String getRTCStringHuman() {
  if (!rtcAvailable) return "RTC not available";

  DateTime now = rtc.now();
  char buf[20];
  snprintf(
    buf, sizeof(buf),
    "%04d-%02d-%02d %02d:%02d:%02d",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute(), now.second()
  );
  return String(buf);
}

String getRTCStringForInput() {
  if (!rtcAvailable) return "";

  DateTime now = rtc.now();
  char buf[17];
  snprintf(
    buf, sizeof(buf),
    "%04d-%02d-%02dT%02d:%02d",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute()
  );
  return String(buf);
}

void setupRTC() {
  if (!rtc.begin()) {
    Serial.println("DS3231 not found");
    rtcAvailable = false;
    return;
  }

  rtcAvailable = true;
  Serial.println("DS3231 initialized");

  if (rtc.lostPower()) {
    Serial.println("DS3231 lost power, setting time from compile time");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  printRTCNow();
}

bool parseDateTimeLocal(String dt, int &year, int &month, int &day, int &hour, int &minute) {
  if (dt.length() < 16) return false;
  if (dt.charAt(4) != '-' || dt.charAt(7) != '-' || dt.charAt(10) != 'T' || dt.charAt(13) != ':')
    return false;

  year   = dt.substring(0, 4).toInt();
  month  = dt.substring(5, 7).toInt();
  day    = dt.substring(8, 10).toInt();
  hour   = dt.substring(11, 13).toInt();
  minute = dt.substring(14, 16).toInt();

  if (year < 2000 || year > 2099) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > 31) return false;
  if (hour > 23) return false;
  if (minute > 59) return false;

  return true;
}

// ---------------- MPU6050 ----------------

bool setupMPU() {
  if (!mpu.begin(MPU_ADDR, &Wire)) {
    Serial.println("MPU6050 not found at 0x69");
    return false;
  }

  mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
  mpu.setGyroRange(MPU6050_RANGE_250_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  Serial.println("MPU6050 initialized at 0x69");
  return true;
}

bool waitForMPUStable() {
  Serial.println("Waiting for MPU to become stable...");

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  float lastAx = a.acceleration.x;
  float lastAy = a.acceleration.y;
  float lastAz = a.acceleration.z;
  float lastGx = g.gyro.x;
  float lastGy = g.gyro.y;
  float lastGz = g.gyro.z;

  int stableCount = 0;
  unsigned long start = millis();

  while (millis() - start < 15000) {
    mpu.getEvent(&a, &g, &temp);

    float da =
      fabs(a.acceleration.x - lastAx) +
      fabs(a.acceleration.y - lastAy) +
      fabs(a.acceleration.z - lastAz);

    float dg =
      fabs(g.gyro.x - lastGx) +
      fabs(g.gyro.y - lastGy) +
      fabs(g.gyro.z - lastGz);

    if (da < STABLE_ACCEL_DELTA && dg < STABLE_GYRO_DELTA) {
      stableCount++;
      if (stableCount >= STABLE_SAMPLES) {
        Serial.println("MPU is stable");
        return true;
      }
    } else {
      stableCount = 0;
    }

    lastAx = a.acceleration.x;
    lastAy = a.acceleration.y;
    lastAz = a.acceleration.z;
    lastGx = g.gyro.x;
    lastGy = g.gyro.y;
    lastGz = g.gyro.z;

    delay(50);
  }

  Serial.println("MPU stability timeout, calibrating anyway");
  return false;
}

void calibrateMPU() {
  Serial.println("Calibrating MPU6050... keep device still");

  sensors_event_t a, g, temp;
  float ax = 0, ay = 0, az = 0;
  float gx = 0, gy = 0, gz = 0;

  for (int i = 0; i < CAL_SAMPLES; i++) {
    mpu.getEvent(&a, &g, &temp);

    ax += a.acceleration.x;
    ay += a.acceleration.y;
    az += a.acceleration.z;

    gx += g.gyro.x;
    gy += g.gyro.y;
    gz += g.gyro.z;

    delay(10);
  }

  accelOffsetX = ax / CAL_SAMPLES;
  accelOffsetY = ay / CAL_SAMPLES;
  accelOffsetZ = (az / CAL_SAMPLES) - 9.80665f;

  gyroOffsetX = gx / CAL_SAMPLES;
  gyroOffsetY = gy / CAL_SAMPLES;
  gyroOffsetZ = gz / CAL_SAMPLES;

  Serial.println("MPU calibration done");
  Serial.printf("Accel offsets: %.4f %.4f %.4f\n", accelOffsetX, accelOffsetY, accelOffsetZ);
  Serial.printf("Gyro offsets : %.4f %.4f %.4f\n", gyroOffsetX, gyroOffsetY, gyroOffsetZ);

  saveCalibrationToFlash();
  vibrateCalibrationFinished();
}

void recalibrateAndSaveMPU() {
  if (!mpuAvailable) return;

  waitForMPUStable();
  calibrateMPU();
}

void readCorrectedMPU(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  ax = a.acceleration.x - accelOffsetX;
  ay = a.acceleration.y - accelOffsetY;
  az = a.acceleration.z - accelOffsetZ;

  gx = g.gyro.x - gyroOffsetX;
  gy = g.gyro.y - gyroOffsetY;
  gz = g.gyro.z - gyroOffsetZ;
}

void printMPUCorrected() {
  if (!mpuAvailable) return;

  float ax, ay, az, gx, gy, gz;
  readCorrectedMPU(ax, ay, az, gx, gy, gz);

  Serial.printf("MPU A[%.3f %.3f %.3f] G[%.3f %.3f %.3f]\n", ax, ay, az, gx, gy, gz);
}

bool isHeadDownNow(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  readCorrectedMPU(ax, ay, az, gx, gy, gz);
  return (az < HEAD_DOWN_Z_THRESHOLD);
}

void handleGyroMouse() {
  if (!gyroMouseEnabled) return;
  if (!mpuAvailable) return;
  if (!bleMouse.isConnected()) return;

  unsigned long now = millis();
  if (now - lastGyroMove < gyroMoveInterval) return;
  lastGyroMove = now;

  float ax, ay, az, gx, gy, gz;
  readCorrectedMPU(ax, ay, az, gx, gy, gz);

  if (fabs(gx) < GYRO_MOUSE_DEADZONE) gx = 0;
  if (fabs(gy) < GYRO_MOUSE_DEADZONE) gy = 0;

  int moveX = (int)round(gy * GYRO_MOUSE_SCALE);
  int moveY = (int)round(gx * GYRO_MOUSE_SCALE);

  if (moveX != 0 || moveY != 0) {
    bleMouse.move(moveX, moveY);
  }
}

// ---------------- BLE JIGGLE ----------------

void jiggle() {
  if (!bleMouse.isConnected()) return;
  if (gyroMouseEnabled) return;

  int x = random(2) ? 2 : -2;
  int y = random(2) ? 2 : -2;

  bleMouse.move(x, y);
  delay(5);
  bleMouse.move(-x, -y);

  Serial.println("Mouse jiggled");
  printRTCNow();
}

// ---------------- OTA / TIME / GYRO / GPIO HANDLERS ----------------

void handleRoot() {
  server.send(200, "text/html", otaPage);
}

void handleSetTime() {
  if (!rtcAvailable) {
    server.send(500, "text/plain", "RTC not available");
    return;
  }

  if (!server.hasArg("datetime")) {
    server.send(400, "text/plain", "Missing datetime field");
    return;
  }

  String dt = server.arg("datetime");

  int year, month, day, hour, minute;
  if (!parseDateTimeLocal(dt, year, month, day, hour, minute)) {
    server.send(400, "text/plain", "Invalid datetime format");
    return;
  }

  rtc.adjust(DateTime(year, month, day, hour, minute, 0));

  Serial.println("RTC updated from webpage");
  printRTCNow();

  server.send(200, "text/plain", "RTC time set successfully");
}

void handleRTCNow() {
  if (!rtcAvailable) {
    server.send(500, "text/plain", "RTC not available");
    return;
  }

  server.send(200, "text/plain", getRTCStringHuman());
}

void handleRTCNowLocal() {
  if (!rtcAvailable) {
    server.send(500, "text/plain", "");
    return;
  }

  server.send(200, "text/plain", getRTCStringForInput());
}

void handleGyroState() {
  String msg = "Gyroscope mouse: ";
  msg += gyroMouseEnabled ? "ON" : "OFF";

  if (!mpuAvailable) {
    msg += " (MPU not available)";
  }

  server.send(200, "text/plain", msg);
}

void handleToggleGyro() {
  if (!mpuAvailable) {
    server.send(500, "text/plain", "MPU6050 not available");
    return;
  }

  gyroMouseEnabled = !gyroMouseEnabled;

  if (gyroMouseEnabled) {
    Serial.println("Gyroscope mouse enabled");
    server.send(200, "text/plain", "Gyroscope mouse enabled");
  } else {
    Serial.println("Gyroscope mouse disabled");
    server.send(200, "text/plain", "Gyroscope mouse disabled");
  }
}

void handleRecalibrate() {
  if (!mpuAvailable) {
    server.send(500, "text/plain", "MPU6050 not available");
    return;
  }

  gyroMouseEnabled = false;
  Serial.println("Web request: recalibrate gyroscope");

  recalibrateAndSaveMPU();
  printMPUCorrected();

  server.send(200, "text/plain", "Gyroscope recalibrated and saved to flash");
}

void handleGPIOState() {
  String json = "[";

  for (int i = 0; i < gpioCount; i++) {
    int pin = gpioList[i];
    int value = digitalRead(pin);

    json += "{\"pin\":";
    json += pin;
    json += ",\"value\":";
    json += value;
    json += "}";

    if (i < gpioCount - 1) {
      json += ",";
    }
  }

  json += "]";
  server.send(200, "application/json", json);
}

void handleUpdateResult() {
  if (Update.hasError()) {
    server.send(200, "text/plain", "Update Failed");
  } else {
    server.send(200, "text/plain", "Update successful. Rebooting...");
    server.client().stop();
    delay(500);
    ESP.restart();
  }
}

void handleUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Update start: %s\n", upload.filename.c_str());
    printRTCNow();

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {

    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }

  } else if (upload.status == UPLOAD_FILE_END) {

    if (Update.end(true)) {
      Serial.printf("Update Success: %u bytes\n", upload.totalSize);
      printRTCNow();
    } else {
      Update.printError(Serial);
    }
  }
}

// ---------------- HOTSPOT ----------------

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);

  server.on(
    "/update",
    HTTP_POST,
    handleUpdateResult,
    handleUpload
  );

  server.on("/settime", HTTP_POST, handleSetTime);
  server.on("/rtctime", HTTP_GET, handleRTCNow);
  server.on("/rtctime-local", HTTP_GET, handleRTCNowLocal);
  server.on("/gyrostate", HTTP_GET, handleGyroState);
  server.on("/togglegyro", HTTP_POST, handleToggleGyro);
  server.on("/recalibrate", HTTP_POST, handleRecalibrate);
  server.on("/gpiostate", HTTP_GET, handleGPIOState);

  server.begin();
}

void startHotspot() {
  if (apStarted) return;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSSID, apPassword);

  apStarted = true;

  Serial.println("Hotspot started");
  Serial.print("SSID: ");
  Serial.println(apSSID);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
  printRTCNow();

  setupWebServer();
  vibrateHotspotOn();
}

void stopHotspot() {
  if (!apStarted) return;

  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  apStarted = false;

  Serial.println("Hotspot stopped");
  vibrateHotspotOff();
}

void manageHotspotByOrientation() {
  if (!mpuAvailable) return;

  static bool lastHeadDown = false;

  float ax, ay, az, gx, gy, gz;
  bool headDown = isHeadDownNow(ax, ay, az, gx, gy, gz);
  unsigned long now = millis();

  if (headDown && !lastHeadDown) {
    Serial.println("Orientation -> HEAD DOWN");
    normalOrientationSince = 0;

    if (!apStarted) {
      Serial.println("Starting hotspot");
      startHotspot();
    }
  }

  if (!headDown && lastHeadDown) {
    Serial.println("Orientation -> NORMAL");
    normalOrientationSince = now;
  }

  if (!headDown && apStarted) {
    if (normalOrientationSince != 0 &&
        (now - normalOrientationSince >= hotspotStopDelay)) {
      Serial.println("Normal for 30s -> stopping hotspot");
      stopHotspot();
      normalOrientationSince = 0;
    }
  }

  if (headDown) {
    normalOrientationSince = 0;
  }

  lastHeadDown = headDown;

  if (apStarted) {
    server.handleClient();
  }
}

// ---------------- SETUP ----------------

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(0, INPUT_PULLUP);
  pinMode(1, INPUT_PULLUP);
  pinMode(2, INPUT_PULLUP);
  pinMode(3, INPUT_PULLUP);
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);
  pinMode(5, INPUT_PULLUP);
  pinMode(6, INPUT_PULLUP);
  pinMode(7, INPUT_PULLUP);
  pinMode(10, INPUT_PULLUP);
  pinMode(20, INPUT_PULLUP);
  pinMode(21, INPUT_PULLUP);

  randomSeed(micros());

  disconnectStart = millis();

  Wire.begin(SDA_PIN, SCL_PIN);

  setupRTC();

  mpuAvailable = setupMPU();
  if (mpuAvailable) {
    bool loaded = loadCalibrationFromFlash();

    if (!loaded) {
      Serial.println("Using default calibration values");
      Serial.printf("Accel offsets: %.4f %.4f %.4f\n", accelOffsetX, accelOffsetY, accelOffsetZ);
      Serial.printf("Gyro offsets : %.4f %.4f %.4f\n", gyroOffsetX, gyroOffsetY, gyroOffsetZ);
    }

    printMPUCorrected();
  }

  BLESecurity *pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
  pSecurity->setCapability(ESP_IO_CAP_NONE);
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  bleMouse.begin();
  bleMouse.setBatteryLevel(95);
}

// ---------------- LOOP ----------------

void loop() {
  manageHotspotByOrientation();

  bool hotspotClient = apStarted && (WiFi.softAPgetStationNum() > 0);

  if (!bleMouse.isConnected() && !hotspotClient) {
    if (millis() - disconnectStart >= sleepTimeout) {
      Serial.println("Entering deep sleep");
      printRTCNow();

      esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
      esp_deep_sleep_start();
    }

    delay(20);
    return;
  }

  if (bleMouse.isConnected()) {
    disconnectStart = millis();
  }

  handleGyroMouse();

  unsigned long now = millis();

  if (!gyroMouseEnabled && bleMouse.isConnected() && (now - lastJiggle >= jiggleInterval)) {
    jiggle();
    lastJiggle = now;
  }

  delay(5);
}
