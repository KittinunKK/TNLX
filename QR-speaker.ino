// ============================================================
//  ESP32 Counter + Barcode Scanner (UART) + WiFi HTTP POST + LCD 20x4
//  🔵 ฟ้า=READY | 🟢 เขียว=RUN | 🟡 เหลือง=PAUSE | 🔴 แดง=DONE
// ============================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <esp_task_wdt.h>

// ─── WiFi ───────────────────────────────────────────────────
const char* WIFI_SSID     = "-";
const char* WIFI_PASSWORD = "-";
const char* ENDPOINT_URL  = "http://10.0.32.70:50000/api/TEAM_A/api/iot/barcode/data";
const char* BASE_URL      = "http://10.0.32.70:50000/api/TEAM_A";
const char* TEAM_NAME     = "TEAM I";

// ─── Target ─────────────────────────────────────────────────
#define DEFAULT_TARGET 20

// ─── LCD ────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 20, 4);

// ─── Barcode Scanner UART ───────────────────────────────────
#define SCANNER_RX   17
#define SCANNER_TX   16
#define SCANNER_BAUD 9600
HardwareSerial scannerSerial(2);

// ─── Buzzer ─────────────────────────────────────────────────
#define BUZZER_PIN 13

// ─── Buttons ────────────────────────────────────────────────
#define BTN_STOP    33
#define BTN_PAUSE   4
#define BTN_RESET   12
#define BTN_COUNT   32
#define BTN_COUNT2  35

// ─── LEDs ───────────────────────────────────────────────────
#define LED_READY  14
#define LED_RUN    25
#define LED_PAUSE  27
#define LED_DONE   26

// ─── Debounce ───────────────────────────────────────────────
#define DEBOUNCE_MS 50

// ─── Status codes ───────────────────────────────────────────
#define STATUS_START  1
#define STATUS_COUNT  2
#define STATUS_BREAK  3
#define STATUS_PAUSE  4
#define STATUS_RESUME 5
#define STATUS_RESET  6
#define STATUS_FINISH 7

// ─── IP ─────────────────────────────────────────────────────
String deviceIP = "unknown";

// ─── ข้อมูลจาก server ───────────────────────────────────────
String infoStyle    = "";
String infoLot      = "";
String infoColor    = "";
int    infoOrderQty = 0;
bool   dataFound    = false;

// ─── Function prototype ──────────────────────────────────────
void updateLCD(bool forceRedraw = false);

// ============================================================
//  Structs
// ============================================================
struct Button {
  uint8_t       pin;
  int           lastState;
  int           stableState;
  unsigned long lastChangeTime;
  bool          triggered;
};

struct PostRequest {
  int           status;
  String        uid;
  int           count;
  unsigned long workTime;
  unsigned long pauseTime;
  bool          pending;
};

// ─── Buttons ────────────────────────────────────────────────
Button btnStop   = { BTN_STOP,   HIGH, HIGH, 0, false };
Button btnPause  = { BTN_PAUSE,  HIGH, HIGH, 0, false };
Button btnReset  = { BTN_RESET,  HIGH, HIGH, 0, false };
Button btnCount  = { BTN_COUNT,  HIGH, HIGH, 0, false };
Button btnCount2 = { BTN_COUNT2, HIGH, HIGH, 0, false };

// ─── State ──────────────────────────────────────────────────
bool running     = false;
bool paused      = false;
int  countValue  = 0;
int  targetValue = DEFAULT_TARGET;

String currentUID    = "";
String barcodeBuffer = "";

unsigned long taskStartTime      = 0;
unsigned long pauseStartTime     = 0;
unsigned long totalPauseDuration = 0;

unsigned long lastLCDUpdate = 0;
#define LCD_REFRESH_MS 300

// ─── POST Queue ─────────────────────────────────────────────
#define MAX_QUEUE 10
PostRequest postQueue[MAX_QUEUE];
int queueIndex = 0;
unsigned long lastPostAttempt = 0;
#define POST_RETRY_MS 500

// ============================================================
//  Helpers
// ============================================================
void updateButton(Button &btn) {
  int reading = digitalRead(btn.pin);
  btn.triggered = false;
  if (reading != btn.lastState) {
    btn.lastChangeTime = millis(); 
    btn.lastState = reading;
  }
  if ((millis() - btn.lastChangeTime) >= DEBOUNCE_MS) {
    if (reading != btn.stableState) {
      btn.stableState = reading;
      if (btn.stableState == LOW) btn.triggered = true;
    }
  }
}

void setLEDs(bool ready, bool run, bool pause, bool done) {
  digitalWrite(LED_READY, ready ? LOW : HIGH);
  digitalWrite(LED_RUN,   run   ? LOW : HIGH);
  digitalWrite(LED_PAUSE, pause ? LOW : HIGH);
  digitalWrite(LED_DONE,  done  ? LOW : HIGH);
}

void getDurations(unsigned long &workTime, unsigned long &pauseTime) {
  if (!running) { workTime = 0; pauseTime = 0; return; }
  if (paused) {
    workTime  = pauseStartTime - taskStartTime - totalPauseDuration;
    pauseTime = totalPauseDuration + (millis() - pauseStartTime);
  } else {
    workTime  = millis() - taskStartTime - totalPauseDuration;
    pauseTime = totalPauseDuration;
  }
}

// ─── Buzzer ──────────────────────────────────────────────────
void beep(int times, int duration) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, LOW);   // เปิดเสียง
    delay(duration);
    digitalWrite(BUZZER_PIN, HIGH);    // ปิดเสียง
    if (i < times - 1) delay(100);
    
  }
}

// ─── ดึง target จาก server ตาม barcode ──────────────────────
void fetchTarget(String uid) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String teamEncoded = String(TEAM_NAME);
  teamEncoded.replace(" ", "%20");

  String url = String(BASE_URL) + "/api/bundle/qty/barcode/" + uid + "?sn_mac=" + teamEncoded;
  http.begin(url);
  http.setTimeout(3000);

  int code = http.GET();
  if (code == 200) {
    String response = http.getString();
    Serial.println("[Response] " + response);

    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, response) == DeserializationError::Ok) {
      targetValue  = doc["target"]    | DEFAULT_TARGET;
      countValue   = doc["actual"]    | 0;
      infoStyle    = doc["style"]     | "";
      infoLot      = doc["lot"]       | "";
      infoColor    = doc["color"]     | "";
      infoOrderQty = (int)(doc["order_qty"].as<float>());
      dataFound    = true;

      Serial.printf("[LOAD] actual=%d target=%d style=%s lot=%s color=%s order_qty=%d\n",
                    countValue, targetValue,
                    infoStyle.c_str(), infoLot.c_str(),
                    infoColor.c_str(), infoOrderQty);
    }   
  } else {
    dataFound = false;
    Serial.printf("[HTTP] Error %d\n", code);

    running            = false;
    paused             = false;
    countValue         = 0;
    currentUID         = "";
    taskStartTime      = 0;
    pauseStartTime     = 0;
    totalPauseDuration = 0;
    targetValue        = DEFAULT_TARGET;
    infoStyle          = "";
    infoLot            = "";
    infoColor          = "";
    infoOrderQty       = 0;
    queueIndex         = 0;

    beep(2, 500);

    setLEDs(true, false, false, false);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Data not found      ");
    delay(3000);

    lcd.clear();
    updateLCD(true);
  }
  http.end();
}

// ─── LCD ─────────────────────────────────────────────────────
void updateLCD(bool forceRedraw) {
  String line0, line1, line2, line3;

  line0 = "ORD:" + (infoOrderQty > 0 ? String(infoOrderQty) : "--------");
  while (line0.length() < 20) line0 += " ";

  line1 = "TG:" + String(countValue) + "/" + String(targetValue);
  while (line1.length() < 20) line1 += " ";

  line2 = "STY:" + (infoStyle.length() > 0 ? infoStyle : "--------");
  while (line2.length() < 20) line2 += " ";
  line2 = line2.substring(0, 20);

  line3 = "LOT:" + (infoLot.length() > 0 ? infoLot : "----") +
          " " + (infoColor.length() > 0 ? infoColor : "--");
  while (line3.length() < 20) line3 += " ";
  line3 = line3.substring(0, 20);

  static String prev0, prev1, prev2, prev3;

  if (forceRedraw) { prev0 = ""; prev1 = ""; prev2 = ""; prev3 = ""; }

  if (line0 != prev0) { lcd.setCursor(0, 0); lcd.print(line0); prev0 = line0; }
  if (line1 != prev1) { lcd.setCursor(0, 1); lcd.print(line1); prev1 = line1; }
  if (line2 != prev2) { lcd.setCursor(0, 2); lcd.print(line2); prev2 = line2; }
  if (line3 != prev3) { lcd.setCursor(0, 3); lcd.print(line3); prev3 = line3; }
}

// ─── POST Queue ──────────────────────────────────────────────
void queuePost(int statusCode, int count,
               unsigned long workTime, unsigned long pauseTime) {
  if (queueIndex < MAX_QUEUE) {
    PostRequest &req = postQueue[queueIndex];
    req.status    = statusCode;
    req.uid       = currentUID;
    req.count     = count;
    req.workTime  = workTime;
    req.pauseTime = pauseTime;
    req.pending   = true;
    Serial.printf("[Queue] status=%d count=%d\n", statusCode, count);
    queueIndex++;
  } else {
    Serial.println("[Queue] FULL!");
  }
}

void sendPostRequest(int idx) {
  PostRequest &req = postQueue[idx];

  StaticJsonDocument<256> doc;
  doc["status"]     = req.status;
  doc["uid"]        = req.uid;
  doc["time"]       = req.workTime;
  doc["pauseTime"]  = req.pauseTime;
  doc["actual"]     = req.count;
  doc["sn_mac"]     = TEAM_NAME;
  doc["key_button"] = req.status;
  doc["ip_address"] = deviceIP;
  doc["ssid_wifi"]  = WIFI_SSID;

  String payload;
  serializeJson(doc, payload);
  Serial.println("[Payload] " + payload);

  HTTPClient http;
  http.begin(ENDPOINT_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);
  
  int code = http.POST(payload);
  if (code == 200 || code == 201) {
    Serial.printf("[POST] status=%d → HTTP %d ✓\n", req.status, code);
    req.pending = false;
  } else {
    String response = http.getString();
    Serial.printf("[POST] status=%d → HTTP %d retry\n", req.status, code);
    if (code > 0) Serial.println(response);

    if (code == 422) {
      StaticJsonDocument<256> errDoc;
      if (deserializeJson(errDoc, response) == DeserializationError::Ok) {
        String detail = errDoc["detail"] | "Error 422";
        Serial.println("[ERROR] " + detail);

        lcd.setCursor(0, 0);
        lcd.print("ERR:                ");
        lcd.setCursor(5, 0);
        if (detail.length() > 15) detail = detail.substring(detail.length() - 15);
        lcd.print(detail);

        beep(2, 500);
        delay(2000);

        running            = false;
        paused             = false;
        countValue         = 0;
        currentUID         = "";
        taskStartTime      = 0;
        pauseStartTime     = 0;
        totalPauseDuration = 0;
        targetValue        = DEFAULT_TARGET;
        infoStyle          = "";
        infoLot            = "";
        infoColor          = "";
        infoOrderQty       = 0;
        dataFound          = false;
        queueIndex         = 0;

        setLEDs(true, false, false, false);
        lcd.clear();
        updateLCD(true);
        Serial.println("[RESET] Auto reset after error");
      }
    }
  }
  http.end();
}

void processPostQueue() {
  if (millis() - lastPostAttempt < POST_RETRY_MS) return;
  lastPostAttempt = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  for (int i = 0; i < queueIndex; i++) {
    if (postQueue[i].pending) { sendPostRequest(i); break; }
  }
}

void cleanupPostQueue() {
  int w = 0;
  for (int i = 0; i < queueIndex; i++) {
    if (postQueue[i].pending) postQueue[w++] = postQueue[i];
  }
  queueIndex = w;
}

void doFinish() {
  unsigned long wTime, pTime;
  getDurations(wTime, pTime);
  paused  = false;
  running = false;
  Serial.printf("FINISH | count=%d/%d\n", countValue, targetValue);
  setLEDs(false, false, false, true);
  beep(3, 200);
  queuePost(STATUS_FINISH, countValue, wTime, pTime);

  countValue   = 0;
  currentUID   = "";
  infoStyle    = "";
  infoLot      = "";
  infoColor    = "";
  infoOrderQty = 0;
  dataFound    = false;
  targetValue  = DEFAULT_TARGET;

  lcd.clear();
  updateLCD(true);
}

void doStop() {
  unsigned long wTime, pTime;
  getDurations(wTime, pTime);
  paused  = false;
  running = false;
  Serial.printf("BREAK | count=%d/%d\n", countValue, targetValue);
  queuePost(STATUS_BREAK, countValue, wTime, pTime);

  countValue   = 0;
  currentUID   = "";
  infoStyle    = "";
  infoLot      = "";
  infoColor    = "";
  infoOrderQty = 0;
  dataFound    = false;
  targetValue  = DEFAULT_TARGET;

  setLEDs(true, false, false, false);
  lcd.clear();
  updateLCD(true);
}

// ─── อ่าน Barcode จาก Serial2 ───────────────────────────────
String readBarcode() {
  static unsigned long lastByteTime = 0;

  while (scannerSerial.available()) {
    char c = scannerSerial.read();
    lastByteTime = millis();
    if (c != '\r' && c != '\n') {
      barcodeBuffer += c;
    }
  }

  if (barcodeBuffer.length() > 0 && millis() - lastByteTime >= 300) {
    String result = barcodeBuffer;
    barcodeBuffer = "";
    result.trim();
    Serial.println("[SCAN] Got: " + result);
    return result;
  }

  return "";
}

// ============================================================
//  setup()
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  scannerSerial.begin(SCANNER_BAUD, SERIAL_8N1, SCANNER_RX, SCANNER_TX);
  Serial.println("[SCANNER] Barcode scanner ready");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);

  Wire.begin(21, 22);
  delay(100);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("  ESP32 Counter     ");
  lcd.setCursor(0, 1); lcd.print("  Initializing...   ");

  pinMode(BTN_STOP,   INPUT_PULLUP);
  pinMode(BTN_PAUSE,  INPUT_PULLUP);
  pinMode(BTN_RESET,  INPUT_PULLUP);
  pinMode(BTN_COUNT,  INPUT_PULLUP);
  pinMode(BTN_COUNT2, INPUT_PULLUP);

  pinMode(LED_READY, OUTPUT);
  pinMode(LED_RUN,   OUTPUT);
  pinMode(LED_PAUSE, OUTPUT);
  pinMode(LED_DONE,  OUTPUT);
  setLEDs(true, false, false, false);

  lcd.setCursor(0, 2); lcd.print("WiFi connecting...  ");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long wStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wStart < 10000) {
    delay(300); Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    deviceIP = WiFi.localIP().toString();
    Serial.println("\n[WiFi] " + deviceIP);
    lcd.setCursor(0, 2); lcd.print("WiFi OK             ");
    lcd.setCursor(0, 3);
    String ipLine = "IP: " + deviceIP;
    while (ipLine.length() < 20) ipLine += " ";
    lcd.print(ipLine);
    beep(1, 100);
  } else {
    Serial.println("\n[WiFi] FAILED");
    lcd.setCursor(0, 2); lcd.print("WiFi FAILED         ");
    lcd.setCursor(0, 3); lcd.print("Check SSID/Password ");
    beep(3, 300);
  }

  delay(1500);
  lcd.clear();

  esp_task_wdt_deinit();
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);
  Serial.println("[WDT] Watchdog 30s enabled");

  updateLCD(true);
}

// ============================================================
//  loop()
// ============================================================
void loop() {
  unsigned long now = millis();

  processPostQueue();
  cleanupPostQueue();

  updateButton(btnStop);
  updateButton(btnPause);
  updateButton(btnReset);
  updateButton(btnCount);
  updateButton(btnCount2);

  // ── อ่าน Barcode (เฉพาะตอนไม่ running) ──────────────────
  static unsigned long lastScanTime = 0;
  if (!running) {
    String barcode = readBarcode();
    if (barcode.length() > 0 && millis() - lastScanTime >= 5000) {
      lastScanTime   = millis();
      currentUID     = barcode;        // ← แก้บัก: ใส่ค่า barcode
      barcodeBuffer  = "";             // ← ล้าง buffer
      Serial.println("[SCAN] Barcode: " + currentUID);

      running            = true;
      paused             = false;
      taskStartTime      = millis();
      pauseStartTime     = 0;
      totalPauseDuration = 0;

      setLEDs(false, true, false, false);
      queuePost(STATUS_START, 0, 0, 0);
      updateLCD();

      fetchTarget(currentUID);
      updateLCD(true);
    }
  }

  // ── BREAK (ปุ่มแดง) ──────────────────────────────────────
  if (btnStop.triggered && running) {
    beep(1, 100);
    doStop();
  }

  // ── COUNT +1 (ปุ่มเขียว) ─────────────────────────────────
  if (btnCount.triggered && running && !paused) {
    countValue++;
    Serial.printf("COUNT: %d/%d\n", countValue, targetValue);
    beep(1, 50);
    unsigned long wTime, pTime;
    getDurations(wTime, pTime);
    queuePost(STATUS_COUNT, countValue, wTime, pTime);
    updateLCD();
    if (countValue >= targetValue) doFinish();
  }

  // ── COUNT2 +1 (ปุ่มนับเพิ่ม) ─────────────────────────────
  if (btnCount2.triggered && running && !paused) {
    countValue++;
    Serial.printf("COUNT2: %d/%d\n", countValue, targetValue);
    beep(1, 50);
    unsigned long wTime, pTime;
    getDurations(wTime, pTime);
    queuePost(STATUS_COUNT, countValue, wTime, pTime);
    updateLCD();
    if (countValue >= targetValue) doFinish();
  }

  // ── PAUSE / RESUME (ปุ่มเหลือง) ──────────────────────────
  if (btnPause.triggered && running) {
    beep(1, 100);
    unsigned long wTime, pTime;
    if (!paused) {
      paused         = true;
      pauseStartTime = millis();
      Serial.println("PAUSE");
      setLEDs(false, false, true, false);
      getDurations(wTime, pTime);
      queuePost(STATUS_PAUSE, countValue, wTime, pTime);
    } else {
      totalPauseDuration += millis() - pauseStartTime;
      pauseStartTime      = 0;
      paused              = false;
      Serial.println("RESUME");
      setLEDs(false, true, false, false);
      getDurations(wTime, pTime);
      queuePost(STATUS_RESUME, countValue, wTime, pTime);
    }
    updateLCD();
  }

  // ── RESET (ปุ่มฟ้า) ───────────────────────────────────────
  if (btnReset.triggered) {
    beep(1, 100);
    unsigned long wTime, pTime;
    getDurations(wTime, pTime);
    Serial.println("RESET");
    setLEDs(true, false, false, false);
    queuePost(STATUS_RESET, 0, wTime, pTime);

    running            = false;
    paused             = false;
    countValue         = 0;
    currentUID         = "";
    taskStartTime      = 0;
    pauseStartTime     = 0;
    totalPauseDuration = 0;
    targetValue        = DEFAULT_TARGET;
    infoStyle          = "";
    infoLot            = "";
    infoColor          = "";
    infoOrderQty       = 0;
    dataFound          = false;
    lcd.clear();
    updateLCD(true);
  }

  // ── refresh LCD ──────────────────────────────────────────
  if (now - lastLCDUpdate >= LCD_REFRESH_MS) {
    lastLCDUpdate = now;
    updateLCD();
  }

  // ── WiFi reconnect ───────────────────────────────────────
  static unsigned long lastReconnect = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastReconnect >= 10000) {
      lastReconnect = millis();
      Serial.println("[WiFi] Reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  } else if (deviceIP == "unknown") {
    deviceIP = WiFi.localIP().toString();
  }

  // ── LCD keepalive ทุก 30 วินาที ──────────────────────────
  static unsigned long lastKeepAlive = 0;
  if (now - lastKeepAlive >= 30000) {
    lastKeepAlive = now;
    lcd.backlight();
  }

  esp_task_wdt_reset();
  delay(10);
}