#include <BleMouse.h>
#include <BLESecurity.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include "esp_sleep.h"

BleMouse bleMouse("Magic Mouse", "Apple", 100);
WebServer server(80);

// ---------------- SETTINGS ----------------

const int distance = 1;
const unsigned long interval = 60000;
const unsigned long sleepTimeout = 30000;
const unsigned long apWindow = 20000;

const char* apSSID = "Magic Mouse";
const char* apPassword = "";

// ---------------- STATE ----------------

unsigned long lastMove = 0;
unsigned long disconnectStart = 0;
unsigned long bootTime = 0;

bool apStarted = false;
bool apLockedOff = false;
bool clientEverConnected = false;

// ---------------- OTA PAGE ----------------

const char* otaPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>ESP32 Firmware Update</title>
<meta name="viewport" content="width=device-width,initial-scale=1">

<style>
body{
font-family:Arial;
max-width:420px;
margin:auto;
padding:20px;
}

progress{
width:100%;
height:20px;
}

#status{
margin-top:10px;
font-weight:bold;
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

</script>

</body>
</html>
)rawliteral";

// ---------------- BLE JIGGLE ----------------

void jiggle() {

  if (!bleMouse.isConnected()) return;

  int x = random(2) ? 2 : -2;
  int y = random(2) ? 2 : -2;

  bleMouse.move(x, y);
  delay(5);
  bleMouse.move(-x, -y);
}

// ---------------- OTA HANDLERS ----------------

void handleRoot() {
  server.send(200, "text/html", otaPage);
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

    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
      Update.printError(Serial);

  } else if (upload.status == UPLOAD_FILE_WRITE) {

    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
      Update.printError(Serial);

  } else if (upload.status == UPLOAD_FILE_END) {

    if (Update.end(true))
      Serial.printf("Update Success: %u bytes\n", upload.totalSize);
    else
      Update.printError(Serial);
  }
}

// ---------------- WEB SERVER ----------------

void setupWebServer() {

  server.on("/", HTTP_GET, handleRoot);

  server.on(
    "/update",
    HTTP_POST,
    handleUpdateResult,
    handleUpload
  );

  server.begin();
}

// ---------------- HOTSPOT ----------------

void startHotspot() {

  if (apStarted || apLockedOff) return;

  WiFi.mode(WIFI_AP);

  WiFi.softAP(apSSID, apPassword);

  apStarted = true;

  Serial.println("Hotspot started");
  Serial.print("SSID: ");
  Serial.println(apSSID);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  setupWebServer();
}

void stopHotspot() {

  if (!apStarted) return;

  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  apStarted = false;
  apLockedOff = true;

  Serial.println("Hotspot stopped");
}

void manageHotspot() {

  if (apLockedOff) return;

  unsigned long now = millis();

  if (!apStarted)
    startHotspot();

  int clients = WiFi.softAPgetStationNum();

  if (clients > 0)
    clientEverConnected = true;

  if (!clientEverConnected && (now - bootTime >= apWindow)) {

    stopHotspot();
    return;
  }

  if (apStarted)
    server.handleClient();
}

// ---------------- SETUP ----------------

void setup() {

  Serial.begin(115200);

  randomSeed(micros());

  bootTime = millis();
  disconnectStart = millis();

  BLESecurity *pSecurity = new BLESecurity();

  pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
  pSecurity->setCapability(ESP_IO_CAP_NONE);
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  bleMouse.begin();
  bleMouse.setBatteryLevel(95);

  startHotspot();
}

// ---------------- LOOP ----------------

void loop() {

  manageHotspot();

  bool hotspotClient = WiFi.softAPgetStationNum() > 0;

  if (!bleMouse.isConnected() && !hotspotClient) {

    if (millis() - disconnectStart >= sleepTimeout) {

      Serial.println("Entering deep sleep");

      esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
      esp_deep_sleep_start();
    }

    delay(100);
    return;
  }

  if (bleMouse.isConnected())
    disconnectStart = millis();

  unsigned long now = millis();

  if (bleMouse.isConnected() && (now - lastMove >= interval)) {

    jiggle();
    lastMove = now;
  }

  delay(10);
}
