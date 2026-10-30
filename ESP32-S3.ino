#include "USB.h"
#include "USBHIDMouse.h"
#include "USBMSC.h"
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>

// ---------- Pins ----------
#define SD_CS 10
#define SD_MOSI 11
#define SD_SCK 12
#define SD_MISO 13
#define BTN_LEFT 4
#define BTN_RIGHT 5
#define BTN_MODE 6  // press anytime: switch USB drive <-> WiFi

#define USE_PS2_MOUSE 0  // 0 = no movement source, 1 = PS/2 mouse
#define PS2_CLK 15
#define PS2_DATA 16

#define SD_MAX_HZ 20000000  // lower to 10000000 if you ever see file corruption

USBHIDMouse Mouse;
USBMSC MSC;
WebServer* server = nullptr;  // created fresh every time WiFi mode starts

volatile bool wifiActive = false;
volatile uint32_t lastBusyMs = 0;
bool switchRequested = false;
uint32_t switchReqMs = 0;
bool lastBtn = HIGH;
uint32_t lastBtnMs = 0;
bool sdOK = false;
uint32_t sdSpeed = 0;

static uint8_t ioBuf[16384];  // shared buffer for upload / download

void setLed(uint8_t r, uint8_t g, uint8_t b) {
#ifdef RGB_BUILTIN
  neopixelWrite(RGB_BUILTIN, r, g, b);
#endif
}

void blinkRed() {
  for (int i = 0; i < 4; i++) {
    setLed(60, 0, 0);
    delay(150);
    setLed(0, 0, 0);
    delay(150);
  }
}

// ================= SD INIT (fastest stable speed) =================
bool initSD() {
  const uint32_t speeds[] = { SD_MAX_HZ, 10000000, 4000000 };
  for (uint32_t s : speeds) {
    SD.end();
    delay(20);
    if (!SD.begin(SD_CS, SPI, s)) continue;
    uint32_t total = SD.numSectors();
    bool ok = (total > 0);
    uint8_t first[512];
    for (int i = 0; i < 48 && ok; i++) {  // read twice and compare
      uint32_t sec = (i * 7919UL) % total;
      if (!SD.readRAW(ioBuf, sec)) {
        ok = false;
        break;
      }
      memcpy(first, ioBuf, 512);
      if (!SD.readRAW(ioBuf, sec) || memcmp(first, ioBuf, 512) != 0) ok = false;
    }
    if (ok) {
      sdSpeed = s;
      return true;
    }
  }
  SD.end();
  return false;
}

// ================= PS/2 MOUSE =================
volatile uint8_t rb[64];
volatile uint8_t rbHead = 0, rbTail = 0;
volatile uint8_t bitCount = 0, shiftReg = 0;
volatile uint32_t lastBitUs = 0;
bool ps2L = false, ps2R = false;

void IRAM_ATTR ps2ISR() {
  uint32_t now = micros();
  if (now - lastBitUs > 1000) {
    bitCount = 0;
    shiftReg = 0;
  }
  lastBitUs = now;
  int v = digitalRead(PS2_DATA);
  if (bitCount >= 1 && bitCount <= 8) shiftReg = shiftReg | (v << (bitCount - 1));
  if (bitCount == 10) {
    rb[rbHead] = shiftReg;
    rbHead = (rbHead + 1) & 63;
    bitCount = 0;
    shiftReg = 0;
    return;
  }
  bitCount = bitCount + 1;
}

bool waitPin(int pin, int level, uint32_t us = 20000) {
  uint32_t t = micros();
  while (digitalRead(pin) != level)
    if (micros() - t > us) return false;
  return true;
}

void ps2Write(uint8_t b) {
  detachInterrupt(PS2_CLK);
  pinMode(PS2_CLK, OUTPUT);
  digitalWrite(PS2_CLK, LOW);
  delayMicroseconds(150);
  pinMode(PS2_DATA, OUTPUT);
  digitalWrite(PS2_DATA, LOW);
  pinMode(PS2_CLK, INPUT_PULLUP);
  uint8_t parity = 1;
  for (int i = 0; i < 8; i++) {
    waitPin(PS2_CLK, LOW);
    digitalWrite(PS2_DATA, b & 1);
    parity ^= (b & 1);
    b >>= 1;
    waitPin(PS2_CLK, HIGH);
  }
  waitPin(PS2_CLK, LOW);
  digitalWrite(PS2_DATA, parity);
  waitPin(PS2_CLK, HIGH);
  waitPin(PS2_CLK, LOW);
  pinMode(PS2_DATA, INPUT_PULLUP);
  waitPin(PS2_DATA, LOW);
  waitPin(PS2_CLK, LOW);
  waitPin(PS2_CLK, HIGH);
  bitCount = 0;
  shiftReg = 0;
  attachInterrupt(PS2_CLK, ps2ISR, FALLING);
}

void ps2Init() {
  pinMode(PS2_CLK, INPUT_PULLUP);
  pinMode(PS2_DATA, INPUT_PULLUP);
  delay(800);
  ps2Write(0xF4);
  delay(100);
  rbHead = rbTail = 0;
}

void ps2Task() {
  static uint8_t pk[3];
  static uint8_t n = 0;
  while (rbHead != rbTail) {
    uint8_t b = rb[rbTail];
    rbTail = (rbTail + 1) & 63;
    if (n == 0 && !(b & 0x08)) continue;
    pk[n++] = b;
    if (n == 3) {
      n = 0;
      int dx = pk[1] - ((pk[0] << 4) & 0x100);
      int dy = pk[2] - ((pk[0] << 3) & 0x100);
      ps2L = pk[0] & 1;
      ps2R = pk[0] & 2;
      Mouse.move(constrain(dx, -127, 127), constrain(-dy, -127, 127));
    }
  }
}

void updateButtons() {
  bool l = ps2L || digitalRead(BTN_LEFT) == LOW;
  bool r = ps2R || digitalRead(BTN_RIGHT) == LOW;
  if (l && !Mouse.isPressed(MOUSE_LEFT)) Mouse.press(MOUSE_LEFT);
  if (!l && Mouse.isPressed(MOUSE_LEFT)) Mouse.release(MOUSE_LEFT);
  if (r && !Mouse.isPressed(MOUSE_RIGHT)) Mouse.press(MOUSE_RIGHT);
  if (!r && Mouse.isPressed(MOUSE_RIGHT)) Mouse.release(MOUSE_RIGHT);
}

// ================= USB MASS STORAGE =================
static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  if (wifiActive) return -1;
  uint8_t* p = (uint8_t*)buffer;
  for (uint32_t i = 0; i < bufsize / 512; i++)
    if (!SD.readRAW(p + i * 512, lba + i)) return -1;
  return bufsize;
}
static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  if (wifiActive) return -1;
  lastBusyMs = millis();
  for (uint32_t i = 0; i < bufsize / 512; i++)
    if (!SD.writeRAW(buffer + i * 512, lba + i)) return -1;
  lastBusyMs = millis();
  return bufsize;
}
static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
  return true;
}

// ================= WIFI FILE SERVER =================
const char PAGE_HEAD[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="theme-color" content="#0b0f19">
<title>Mouse Drive | Personal Storage</title>

<style>
:root {
  --bg: #0b0f19;
  --panel: #121827;
  --panel2: #182135;
  --border: #28344a;
  --text: #eef2ff;
  --muted: #929db4;
  --primary: #818cf8;
  --primary2: #6366f1;
  --success: #34d399;
  --danger: #fb7185;
}

* {
  box-sizing: border-box;
}

body {
  margin: 0;
  min-height: 100vh;
  background:
    radial-gradient(ellipse at top left, #20244b 0, transparent 45%),
    var(--bg);
  color: var(--text);
  font-family: system-ui, -apple-system, BlinkMacSystemFont,
               "Segoe UI", sans-serif;
  line-height: 1.6;
}

.container {
  max-width: 1000px;
  margin: auto;
  padding: 28px 20px 50px;
}

header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 16px;
  flex-wrap: wrap;
  margin-bottom: 30px;
}

.brand {
  display: flex;
  align-items: center;
  gap: 13px;
}

.logo {
  width: 48px;
  height: 48px;
  display: grid;
  place-items: center;
  background: linear-gradient(135deg, #818cf8, #6366f1);
  border-radius: 15px;
  font-size: 25px;
  box-shadow: 0 8px 30px #6366f133;
}

h1 {
  font-size: 23px;
  margin: 0;
  letter-spacing: -0.7px;
}

.subtitle {
  color: var(--muted);
  font-size: 13px;
  margin: 2px 0 0;
}

.status {
  display: flex;
  align-items: center;
  gap: 8px;
  border: 1px solid #245744;
  background: #102a24;
  color: var(--success);
  padding: 8px 13px;
  border-radius: 30px;
  font-size: 12px;
  font-weight: 600;
}

.dot {
  width: 8px;
  height: 8px;
  background: var(--success);
  border-radius: 50%;
  box-shadow: 0 0 10px #34d39988;
}

.hero {
  margin-bottom: 25px;
}

.hero h2 {
  font-size: clamp(27px, 5vw, 40px);
  line-height: 1.15;
  letter-spacing: -1.4px;
  margin: 0 0 12px;
}

.gradient {
  background: linear-gradient(90deg, #a5b4fc, #c4b5fd, #67e8f9);
  -webkit-background-clip: text;
  background-clip: text;
  color: transparent;
}

.hero p {
  color: var(--muted);
  margin: 0;
  font-size: 14px;
}

.panel {
  background: #121827e8;
  border: 1px solid var(--border);
  border-radius: 20px;
  padding: 23px;
  margin-bottom: 22px;
  box-shadow: 0 15px 40px #00000018;
}

.panel-heading {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 12px;
  margin-bottom: 18px;
}

.panel-heading h3 {
  margin: 0;
  font-size: 17px;
}

.panel-heading p {
  margin: 4px 0 0;
  color: var(--muted);
  font-size: 12px;
}

.badge {
  background: #25264b;
  color: #c7d2fe;
  border: 1px solid #3e3d75;
  border-radius: 9px;
  padding: 5px 9px;
  font-size: 11px;
  white-space: nowrap;
}

.dropzone {
  border: 1px dashed #515e82;
  background: #171f32;
  border-radius: 15px;
  padding: 30px 16px;
  text-align: center;
  transition: .2s ease;
}

.dropzone.dragover {
  border-color: var(--primary);
  background: #25264b;
}

.upload-icon {
  font-size: 36px;
  margin-bottom: 5px;
}

.dropzone strong {
  display: block;
  font-size: 15px;
}

.dropzone p {
  color: var(--muted);
  font-size: 12px;
  margin: 7px 0 17px;
}

input[type=file] {
  display: block;
  width: 100%;
  max-width: 380px;
  margin: 0 auto 16px;
  padding: 10px;
  color: var(--muted);
  background: #0b1020;
  border: 1px solid var(--border);
  border-radius: 10px;
  font-size: 12px;
}

input[type=file]::file-selector-button {
  background: #283451;
  color: white;
  border: 0;
  padding: 8px 10px;
  border-radius: 7px;
  margin-right: 10px;
  cursor: pointer;
}

button {
  border: 0;
  cursor: pointer;
  font: inherit;
  transition: .2s ease;
}

.primary-btn {
  color: white;
  background: linear-gradient(135deg, #818cf8, #6366f1);
  padding: 11px 20px;
  border-radius: 10px;
  font-size: 13px;
  font-weight: 650;
  box-shadow: 0 5px 18px #6366f133;
}

.primary-btn:hover {
  transform: translateY(-1px);
  filter: brightness(1.12);
}

.primary-btn:disabled {
  opacity: .5;
  cursor: not-allowed;
  transform: none;
}

.progress-wrap {
  margin-top: 20px;
  display: none;
}

.progress-info {
  display: flex;
  justify-content: space-between;
  gap: 10px;
  font-size: 12px;
  color: var(--muted);
  margin-bottom: 8px;
}

.progress {
  height: 8px;
  background: #283148;
  border-radius: 20px;
  overflow: hidden;
}

#b {
  height: 100%;
  width: 0;
  background: linear-gradient(90deg, #818cf8, #67e8f9);
  border-radius: 20px;
  transition: width .12s;
}

#s {
  color: var(--muted);
  font-size: 12px;
  margin-top: 12px;
  overflow-wrap: anywhere;
}

.file-toolbar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 12px;
  flex-wrap: wrap;
  margin-bottom: 17px;
}

.file-toolbar h3 {
  margin: 0;
  font-size: 17px;
}

.search {
  width: 100%;
  max-width: 270px;
  padding: 10px 13px;
  border-radius: 10px;
  background: #0b1020;
  border: 1px solid var(--border);
  color: var(--text);
  outline: none;
  font-size: 12px;
}

.search:focus {
  border-color: var(--primary);
}

.file-list {
  display: grid;
  gap: 10px;
}

.file-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 15px;
  padding: 14px;
  background: #171f30;
  border: 1px solid #27334a;
  border-radius: 13px;
  transition: .2s;
}

.file-row:hover {
  border-color: #545b91;
  background: #1b253b;
}

.file-info {
  display: flex;
  align-items: center;
  gap: 12px;
  min-width: 0;
}

.file-icon {
  display: grid;
  place-items: center;
  width: 42px;
  height: 45px;
  flex-shrink: 0;
  border-radius: 10px;
  background: #25264b;
  font-size: 21px;
}

.file-name {
  color: var(--text);
  font-size: 13px;
  font-weight: 600;
  overflow-wrap: anywhere;
}

.file-size {
  color: var(--muted);
  font-size: 11px;
  margin-top: 3px;
}

.download-btn {
  flex-shrink: 0;
  text-decoration: none;
  color: #c7d2fe;
  background: #28294c;
  border: 1px solid #414574;
  padding: 8px 12px;
  border-radius: 9px;
  font-size: 12px;
}

.download-btn:hover {
  background: #393966;
}

.empty {
  text-align: center;
  padding: 30px 15px;
  border: 1px dashed var(--border);
  border-radius: 13px;
  color: var(--muted);
  font-size: 13px;
}

footer {
  text-align: center;
  color: #65718a;
  font-size: 11px;
  padding: 10px 0;
}

@media (max-width: 600px) {
  .container {
    padding: 20px 13px 35px;
  }

  .panel {
    padding: 16px;
    border-radius: 15px;
  }

  .hero h2 {
    font-size: 29px;
  }

  .file-row {
    padding: 11px;
  }

  .download-btn {
    padding: 8px;
  }

  .search {
    max-width: none;
  }
}
</style>
</head>

<body>
<div class="container">

<header>
  <div class="brand">
    <div class="logo">💾</div>
    <div>
      <h1>Mouse Drive</h1>
      <p class="subtitle">ESP32-S3 Personal Storage</p>
    </div>
  </div>

  <div class="status">
    <span class="dot"></span>
    Wi-Fi Connected
  </div>
</header>

<section class="hero">
  <h2>Your files.<br><span class="gradient">Your own cloud.</span></h2>
  <p>Transfer and manage files on your SD card wirelessly.</p>
</section>

<section class="panel">
  <div class="panel-heading">
    <div>
      <h3>Upload files</h3>
      <p>Send files from your device to the SD card.</p>
    </div>
    <span class="badge">MULTI-FILE</span>
  </div>

  <div class="dropzone" id="dz">
    <div class="upload-icon">☁️</div>
    <strong>Select files to upload</strong>
    <p>Choose one or more files from your device.</p>

    <input type="file" id="f" multiple>
    <button class="primary-btn" id="ub" onclick="up()">
      ↑ &nbsp; Upload to SD Card
    </button>
  </div>

  <div class="progress-wrap" id="pw">
    <div class="progress-info">
      <span id="s">Waiting...</span>
      <span id="pct">0%</span>
    </div>
    <div class="progress">
      <div id="b"></div>
    </div>
  </div>
</section>

<section class="panel">
  <div class="file-toolbar">
    <div>
      <h3>SD Card Files</h3>
      <p class="subtitle">Browse and download stored files.</p>
    </div>
    <input id="q" class="search" type="search"
           placeholder="Search files..."
           oninput="filterFiles()">
  </div>

  <div class="file-list" id="fl">
)rawliteral";

String urlEncode(const String& s) {
  String o;
  char c;
  for (size_t i = 0; i < s.length(); i++) {
    c = s[i];
    if (isalnum(c) || c == '.' || c == '_' || c == '-') o += c;
    else {
      char buf[4];
      sprintf(buf, "%%%02X", (uint8_t)c);
      o += buf;
    }
  }
  return o;
}

String htmlEscape(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    switch (s[i]) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += s[i]; break;
    }
  }
  return out;
}

void handleRoot() {
  String h = FPSTR(PAGE_HEAD);

  File root = SD.open("/");
  int count = 0;

  if (root) {
    File f = root.openNextFile();

    while (f) {
      if (!f.isDirectory()) {
        String n = f.name();
        String safeName = htmlEscape(n);
        String encodedName = urlEncode(n);
        size_t bytes = f.size();

        h += F("<div class='file-row' data-name='");
        h += safeName;
        h += F("'><div class='file-info'>");

        h += F("<div class='file-icon'>📄</div>");
        h += F("<div><div class='file-name'>");
        h += safeName;
        h += F("</div><div class='file-size'>");

        if (bytes >= 1024ULL * 1024ULL) {
          h += String((double)bytes / (1024.0 * 1024.0), 2);
          h += F(" MB");
        } else if (bytes >= 1024) {
          h += String((double)bytes / 1024.0, 1);
          h += F(" KB");
        } else {
          h += String(bytes);
          h += F(" bytes");
        }

        h += F("</div></div></div>");

        h += F("<a class='download-btn' href='/get?f=");
        h += encodedName;
        h += F("'>↓ Download</a></div>");

        count++;
      }

      f.close();
      f = root.openNextFile();
    }

    root.close();
  }

  if (count == 0) {
    h += F("<div class='empty'>");
    h += F("<div style='font-size:30px;margin-bottom:8px'>📂</div>");
    h += F("No files found. Upload your first file above.");
    h += F("</div>");
  }

  h += F("</div></section>");

  h += F("<footer>Mouse Drive &bull; ESP32-S3 &bull; Local storage</footer>");
  h += F("</div>");

  // JavaScript for upload progress and file search
  h += F("<script>");
  h += R"rawliteral(
function filterFiles() {
  const q = document.getElementById('q').value.toLowerCase();

  document.querySelectorAll('.file-row').forEach(row => {
    row.style.display =
      row.dataset.name.toLowerCase().includes(q) ? 'flex' : 'none';
  });
}

function up() {
  const input = document.getElementById('f');
  const files = input.files;

  if (!files.length) {
    document.getElementById('s').textContent =
      'Please select at least one file.';
    document.getElementById('pw').style.display = 'block';
    return;
  }

  const btn = document.getElementById('ub');
  const bar = document.getElementById('b');
  const status = document.getElementById('s');
  const pct = document.getElementById('pct');
  const pw = document.getElementById('pw');

  let i = 0;
  let failed = [];

  btn.disabled = true;
  pw.style.display = 'block';

  function next() {
    if (i >= files.length) {
      btn.disabled = false;
      input.value = '';
      bar.style.width = '100%';
      pct.textContent = '100%';

      status.textContent = failed.length
        ? 'Failed: ' + failed.join(', ')
        : 'All files uploaded successfully.';

      if (!failed.length) {
        setTimeout(() => location.reload(), 1000);
      }
      return;
    }

    const file = files[i];
    const xhr = new XMLHttpRequest();
    const data = new FormData();

    data.append('file', file, file.name);

    bar.style.width = '0%';
    pct.textContent = '0%';
    status.textContent =
      'Uploading ' + file.name + ' (' + (i + 1) + '/' +
      files.length + ')';

    xhr.upload.onprogress = function(e) {
      if (e.lengthComputable) {
        const p = Math.round(e.loaded * 100 / e.total);
        bar.style.width = p + '%';
        pct.textContent = p + '%';
      }
    };

    xhr.onload = function() {
      if (xhr.status !== 200 || xhr.responseText.trim() !== 'OK') {
        failed.push(file.name);
      }

      i++;
      next();
    };

    xhr.onerror = function() {
      failed.push(file.name);
      i++;
      next();
    };

    xhr.open('POST', '/upload');
    xhr.send(data);
  }

  next();
}

const dz = document.getElementById('dz');

dz.addEventListener('dragover', e => {
  e.preventDefault();
  dz.classList.add('dragover');
});

dz.addEventListener('dragleave', () => {
  dz.classList.remove('dragover');
});

dz.addEventListener('drop', e => {
  e.preventDefault();
  dz.classList.remove('dragover');

  if (e.dataTransfer.files.length) {
    document.getElementById('f').files = e.dataTransfer.files;
  }
});
)rawliteral";

  h += F("</script></body></html>");

  server->send(200, "text/html", h);
}

void handleGet() {
  String name = server->arg("f");
  if (name.length() == 0 || name.indexOf("..") >= 0) {
    server->send(400, "text/plain", "Bad name");
    return;
  }
  File f = SD.open("/" + name);
  if (!f || f.isDirectory()) {
    server->send(404, "text/plain", "Not found");
    return;
  }

  size_t total = f.size();
  server->setContentLength(total);
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server->send(200, "application/octet-stream", "");

  WiFiClient client = server->client();
  client.setNoDelay(true);
  size_t sent = 0;
  while (sent < total && client.connected()) {
    size_t n = f.read(ioBuf, sizeof(ioBuf));
    if (n == 0) break;
    size_t off = 0;
    uint32_t stall = millis();
    while (off < n && client.connected()) {
      size_t w = client.write(ioBuf + off, n - off);
      if (w == 0) {
        if (millis() - stall > 5000) break;
        delay(1);
      } else {
        off += w;
        stall = millis();
      }
    }
    if (off < n) break;
    sent += n;
  }
  f.close();
  lastBusyMs = millis();
}

// ---- upload ----
File upFile;
size_t upLen = 0;
bool upOK = true;

void upFlush() {
  if (upLen && upOK && upFile) {
    if (upFile.write(ioBuf, upLen) != upLen) upOK = false;
  }
  upLen = 0;
}

void handleUpload() {
  HTTPUpload& up = server->upload();
  lastBusyMs = millis();
  if (up.status == UPLOAD_FILE_START) {
    String n = up.filename;
    int s1 = n.lastIndexOf('/');
    int s2 = n.lastIndexOf('\\');
    if (s2 > s1) s1 = s2;
    if (s1 >= 0) n = n.substring(s1 + 1);
    const char* bad = ":*?\"<>|";
    for (size_t i = 0; i < strlen(bad); i++) n.replace(String(bad[i]), "_");
    upLen = 0;
    upOK = sdOK && n.length() > 0;
    if (upOK) {
      if (SD.exists("/" + n)) SD.remove("/" + n);
      upFile = SD.open("/" + n, FILE_WRITE);
      if (!upFile) upOK = false;
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    size_t off = 0;
    while (off < up.currentSize) {
      size_t c = sizeof(ioBuf) - upLen;
      if (c > up.currentSize - off) c = up.currentSize - off;
      memcpy(ioBuf + upLen, up.buf + off, c);
      upLen += c;
      off += c;
      if (upLen == sizeof(ioBuf)) upFlush();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    upFlush();
    if (upFile) upFile.close();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    upLen = 0;
    if (upFile) upFile.close();
  }
}

void handleUploadDone() {
  server->send(upOK ? 200 : 500, "text/plain", upOK ? "OK" : "FAIL");
  lastBusyMs = millis();
}

// ================= MODE SWITCHING =================
void backToUsbAfterFailure() {
  Serial.println("[MODE] failed -> staying in USB drive mode");
  if (server) {
    server->stop();
    delete server;
    server = nullptr;
  }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);
  sdOK = initSD();
  wifiActive = false;
  if (sdOK) MSC.mediaPresent(true);
  blinkRed();
  setLed(0, 40, 0);
}

void startWifi() {
  Serial.printf("[MODE] -> WIFI (free heap %u)\n", (unsigned)ESP.getFreeHeap());
  wifiActive = true;        // block USB reads/writes
  MSC.mediaPresent(false);  // PC sees "media removed"
  delay(800);

  sdOK = initSD();  // fresh mount of the latest files
  Serial.printf("[SD] %s at %u Hz\n", sdOK ? "OK" : "FAILED", (unsigned)sdSpeed);
  if (!sdOK) {
    backToUsbAfterFailure();
    return;
  }

  bool apOK = false;
  for (int t = 1; t <= 3 && !apOK; t++) {
    WiFi.mode(WIFI_OFF);
    delay(250);
    WiFi.mode(WIFI_AP);
    delay(250);
    apOK = WiFi.softAP("MouseDrive", "12345678", 6, 0, 2);  // channel 6, max 2 clients
    Serial.printf("[WIFI] softAP try %d: %s\n", t, apOK ? "OK" : "FAILED");
  }
  if (!apOK) {
    backToUsbAfterFailure();
    return;
  }

  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  server = new WebServer(80);  // brand-new server every time
  server->on("/", handleRoot);
  server->on("/get", handleGet);
  server->on("/upload", HTTP_POST, handleUploadDone, handleUpload);
  server->begin();

  Serial.printf("[WIFI] ready: connect to MouseDrive, open http://%s\n",
                WiFi.softAPIP().toString().c_str());
  setLed(0, 0, 40);  // blue
}

void stopWifi() {
  Serial.println("[MODE] -> USB drive");
  if (server) {
    server->stop();
    delete server;
    server = nullptr;
  }
  WiFi.softAPdisconnect(true);
  delay(150);
  WiFi.mode(WIFI_OFF);
  delay(300);

  sdOK = initSD();
  Serial.printf("[SD] %s at %u Hz\n", sdOK ? "OK" : "FAILED", (unsigned)sdSpeed);
  wifiActive = false;
  if (sdOK) MSC.mediaPresent(true);  // drive comes back on the PC
  setLed(0, 40, 0);                  // green
  Serial.printf("[MODE] USB drive active (free heap %u)\n", (unsigned)ESP.getFreeHeap());
}

// ================= SETUP / LOOP =================
void setup() {
  Serial.begin(115200);  // shows on the UART port Serial Monitor
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOK = initSD();
  Serial.printf("[BOOT] SD %s at %u Hz\n", sdOK ? "OK" : "FAILED", (unsigned)sdSpeed);

  if (sdOK) {
    MSC.vendorID("ESP32");
    MSC.productID("MouseDrive");
    MSC.productRevision("1.0");
    MSC.onRead(onRead);
    MSC.onWrite(onWrite);
    MSC.onStartStop(onStartStop);
    MSC.mediaPresent(true);
    MSC.begin(SD.numSectors(), 512);
  }
  Mouse.begin();
  USB.productName("Smart Mouse Drive");
  USB.begin();
#if USE_PS2_MOUSE
  ps2Init();
#endif
  setLed(0, 40, 0);  // green = USB drive mode
}

void loop() {
  bool b = digitalRead(BTN_MODE);
  if (lastBtn == HIGH && b == LOW && millis() - lastBtnMs > 300) {
    switchRequested = true;
    switchReqMs = millis();
    lastBtnMs = millis();
    Serial.println("[BTN] switch requested");
  }
  lastBtn = b;

  if (switchRequested) {
    bool idle = millis() - lastBusyMs > 2000;     // PC not writing
    bool forced = millis() - switchReqMs > 8000;  // do not wait forever
    if (idle || forced) {
      if (forced && !idle) Serial.println("[BTN] PC still busy, forcing switch");
      switchRequested = false;
      if (wifiActive) stopWifi();
      else startWifi();
    }
  }

#if USE_PS2_MOUSE
  ps2Task();
#endif
  updateButtons();

  if (wifiActive && server) server->handleClient();
  else delay(1);
}