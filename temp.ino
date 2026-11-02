#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>

// ============================================================
// WiFi & Telegram Config
// ============================================================

const char* WIFI_SSID     = "-";
const char* WIFI_PASSWORD = "-";

const char* BOT_TOKEN = "-";
const char* CHAT_ID   = "-";

// ============================================================
// LINE Messaging API Config
// ============================================================


const char* LINE_CHANNEL_TOKEN = "-";
const char* LINE_TO            = "-";

// ============================================================
// Pin
// ============================================================

#define DHT_PIN   2   // D4
#define DHT_TYPE  DHT11

// ============================================================
// Config
// ============================================================

const float TEMP_THRESHOLD = 28.0;  // แจ้งเตือนเมื่ออุณหภูมิ > 28°C

// แจ้งซ้ำทุก 5 นาทีเมื่ออุณหภูมิยังสูงอยู่
const unsigned long ALERT_INTERVAL  = 5UL * 60UL * 1000UL;
const unsigned long POLL_INTERVAL   = 2000;
const unsigned long SENSOR_INTERVAL = 5000;

const int MAX_SENSOR_FAIL_ALERT = 6;  // อ่านพลาดติดกันกี่ครั้งถึงจะแจ้งเตือนว่าเซนเซอร์เสีย (6*5s = 30s)
const int MAX_POLL_BATCHES      = 10; // กันบอทค้างถ้ามีคนสแปมข้อความรัว ๆ

// Daily Report (ส่ง Telegram อย่างเดียว ไม่ส่ง LINE)
const int DAILY_REPORT_HOUR   = 17;
const int DAILY_REPORT_MINUTE = 0;
int lastReportDay = -1;

// ============================================================
// Long-Uptime Stability Config
// ============================================================

const unsigned long NTP_RESYNC_INTERVAL = 4UL * 60UL * 60UL * 1000UL; // ลอง NTP ซ้ำทุก 4 ชม.
unsigned long lastNtpResync = 0;

const uint32_t MIN_FREE_HEAP       = 10000;
const uint32_t MIN_FREE_BLOCK_SIZE = 4000;

const int  WEEKLY_RESTART_WDAY   = 0; // 0 = อาทิตย์
const int  WEEKLY_RESTART_HOUR   = 4;
const int  WEEKLY_RESTART_MINUTE = 0;
int lastRestartYday = -1;

// ============================================================
// Objects
// ============================================================

DHT dht(DHT_PIN, DHT_TYPE);

LiquidCrystal_I2C lcd(0x27, 16, 2);

WiFiClientSecure client;

UniversalTelegramBot* bot = nullptr;

// ============================================================
// Variables
// ============================================================

float temperature = 0.0;
float humidity    = 0.0;

bool alertSent = false;

unsigned long lastAlertTime  = 0;
unsigned long lastPollTime   = 0;
unsigned long lastSensorTime = 0;
unsigned long lastReconnect  = 0;

int  sensorFailCount   = 0;
bool sensorFailAlerted = false;

// ============================================================
// Telegram Reconnect State
// ============================================================

unsigned long lastBotReconnect  = 0;
int           telegramFailCount = 0;

const int           MAX_TELEGRAM_FAIL      = 5;
const unsigned long BOT_RECONNECT_INTERVAL = 60000;

// ============================================================
// LCD Custom Chars
// ============================================================

byte thermometer[8] = {
  0b00100, 0b01010, 0b01010, 0b01110,
  0b01110, 0b11111, 0b11111, 0b01110
};

byte droplet[8] = {
  0b00100, 0b00100, 0b01010, 0b01010,
  0b10001, 0b10001, 0b10001, 0b01110
};

// ============================================================
// LCD
// ============================================================

void lcdStatus(String line1, String line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

void lcdSplashScreen() {
  lcd.clear();
  lcd.setCursor(2, 0);
  lcd.print("Temp Monitor");
  lcd.setCursor(4, 1);
  lcd.print("ESP8266");
  delay(2000);
}

// ============================================================
// Telegram Reconnect
// ============================================================

void reconnectTelegramBot(bool isError = false) {

  if (millis() - lastBotReconnect < BOT_RECONNECT_INTERVAL) return;

  lastBotReconnect = millis();

  Serial.println("Reconnecting Telegram bot...");

  if (isError) {
    lcdStatus("Telegram Error", "Reconnecting...");
  } else {
    lcdStatus("Reconnect Bot", "After WiFi");
  }

  client.stop();
  delay(200);
  client.setInsecure(); // ข้ามการเช็ค certificate — ไม่พึ่งเวลาเครื่อง

  if (bot != nullptr) {
    delete bot;
    bot = nullptr;
  }
  delay(200);

  bot = new UniversalTelegramBot(BOT_TOKEN, client);
  telegramFailCount = 0;

  lcdStatus("Telegram OK", "Reconnected");
  delay(1000);
}

// ============================================================
// LINE Messaging API (Push Message)
// ============================================================

bool lineEnabled() {
  return strlen(LINE_CHANNEL_TOKEN) > 10 && strlen(LINE_TO) > 5;
}

// LINE ไม่รองรับ Markdown — ตัด * และ ` ออก
String stripMarkdown(String s) {
  s.replace("*", "");
  s.replace("`", "");
  return s;
}

// escape ข้อความให้ใส่ใน JSON ได้ (ไบต์ UTF-8 ภาษาไทยส่งผ่านตรง ๆ ได้)
String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"')       out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') { /* ข้าม */ }
    else if (c == '\t') out += "\\t";
    else                out += c;
  }
  return out;
}

bool sendLine(const String& text) {

  if (!lineEnabled()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  // ใช้ client ตัวเดิมต่อ TLS เพื่อประหยัด heap (Telegram เปิดการเชื่อมต่อใหม่ทุกครั้งอยู่แล้ว)
  client.stop();
  client.setInsecure();
  client.setTimeout(8000);

  if (!client.connect("api.line.me", 443)) {
    Serial.println("LINE: connect failed");
    return false;
  }

  String body = "{\"to\":\"" + String(LINE_TO) +
                "\",\"messages\":[{\"type\":\"text\",\"text\":\"" +
                jsonEscape(text) + "\"}]}";

  client.print("POST /v2/bot/message/push HTTP/1.1\r\n");
  client.print("Host: api.line.me\r\n");
  client.print("Authorization: Bearer " + String(LINE_CHANNEL_TOKEN) + "\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: " + String(body.length()) + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  unsigned long start = millis();
  while (client.connected() && !client.available() && millis() - start < 8000) {
    delay(10);
    yield();
  }

  String statusLine = client.readStringUntil('\n');
  client.stop();

  bool ok = statusLine.indexOf(" 200") > 0;

  Serial.print("LINE push: ");
  Serial.println(statusLine);

  return ok;
}

// ============================================================
// Notify (Telegram + LINE)
// ============================================================
// รับข้อความแบบ Markdown ส่งไป Telegram ตามเดิม
// ถ้า sendToLine = true จะส่งเข้า LINE ด้วย (ตัด Markdown ออก)
// คืน true ถ้าอย่างน้อยหนึ่งช่องทางส่งสำเร็จ

bool notifyAll(const String& mdMsg, bool sendToLine = true) {

  bool tgOk = false;

  if (bot && strlen(CHAT_ID) > 0) {
    tgOk = bot->sendMessage(CHAT_ID, mdMsg, "Markdown");
    if (tgOk) {
      telegramFailCount = 0;
    } else {
      telegramFailCount++;
      if (telegramFailCount >= MAX_TELEGRAM_FAIL) reconnectTelegramBot(true);
    }
  }

  bool lineOk = false;
  if (sendToLine) {
    lineOk = sendLine(stripMarkdown(mdMsg));
  }

  return tgOk || lineOk;
}

// ============================================================
// WiFi Connect
// ============================================================

void connectWiFi() {

  lcdStatus("Connecting WiFi", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 60) {
    delay(500);
    yield();
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    lcdStatus("WiFi Connected", WiFi.localIP().toString());
    delay(1500);
  } else {
    Serial.println("WiFi connection FAILED — restarting");
    lcdStatus("WiFi Failed", "Restarting...");
    delay(2000);
    ESP.restart();
  }
}

// ============================================================
// Access Control Helper
// ============================================================

bool isAuthorizedChat(const String& incomingChatId) {
  return incomingChatId == String(CHAT_ID);
}

// ============================================================
// Time Helpers
// ============================================================

bool isTimeSet() {
  return time(nullptr) >= 8 * 3600 * 2;
}

String currentTimeString() {
  time_t now = time(nullptr);
  if (!isTimeSet()) return "ยังไม่ได้ตั้งเวลา (ใช้ /settime)";
  struct tm timeInfo;
  localtime_r(&now, &timeInfo);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeInfo);
  return String(buf);
}

// รับรูปแบบ "YYYY-MM-DD HH:MM:SS" เป็นเวลาไทย (local) แล้วตั้งนาฬิกาเครื่องเอง
bool setManualTime(const String& input) {
  int y, mo, d, h, mi, s;
  int matched = sscanf(input.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s);
  if (matched != 6) return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) return false;

  struct tm tmStruct = {};
  tmStruct.tm_year  = y - 1900;
  tmStruct.tm_mon   = mo - 1;
  tmStruct.tm_mday  = d;
  tmStruct.tm_hour  = h;
  tmStruct.tm_min   = mi;
  tmStruct.tm_sec   = s;
  tmStruct.tm_isdst = 0;

  time_t epoch = mktime(&tmStruct);
  if (epoch < 0) return false;

  struct timeval tv = { epoch, 0 };
  settimeofday(&tv, nullptr);

  return true;
}

// ============================================================
// Setup
// ============================================================

void setup() {

  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Booting Temp Monitor ===");

  Wire.begin(5, 4);

  lcd.init();
  lcd.backlight();
  lcd.createChar(0, thermometer);
  lcd.createChar(1, droplet);
  lcdSplashScreen();

  dht.begin();

  connectWiFi();

  // ลอง NTP แบบ best-effort สั้นๆ (5 วิ)
  lcdStatus("Trying NTP", "Quick Check");
  configTime(7 * 3600, 0, "th.pool.ntp.org", "pool.ntp.org", "time.google.com");

  int ntpTry = 0;
  while (!isTimeSet() && ntpTry < 10) {
    delay(500);
    ntpTry++;
    yield();
  }
  lastNtpResync = millis();
  delay(500);

  if (isTimeSet()) {
    Serial.print("NTP OK, local time: ");
    Serial.println(currentTimeString());
  } else {
    Serial.println("NTP not available on this network — use /settime in Telegram to set clock manually");
  }

  client.setInsecure();
  bot = new UniversalTelegramBot(BOT_TOKEN, client);

  Serial.print("Bot Token length: ");
  Serial.println(strlen(BOT_TOKEN));
  Serial.print("Chat ID: '");
  Serial.print(CHAT_ID);
  Serial.println("'");
  Serial.print("LINE enabled: ");
  Serial.println(lineEnabled() ? "YES" : "NO");

  bool tokenValid = bot->getMe();
  Serial.print("Telegram getMe() check: ");
  Serial.println(tokenValid ? "OK" : "FAILED");

  lcdStatus("System Ready", "Temp Monitor");
  delay(2000);
}

// ============================================================
// Main Loop
// ============================================================

void loop() {

  unsigned long now = millis();

  // Sensor
  if (now - lastSensorTime >= SENSOR_INTERVAL) {
    lastSensorTime = now;
    readSensor();
    updateLCD();
    if (WiFi.status() == WL_CONNECTED) checkAndAlert();
  }

  // Daily Report
  if (WiFi.status() == WL_CONNECTED && isTimeSet()) {
    checkDailyReport();
  }

  // Scheduled Weekly Restart
  if (WiFi.status() == WL_CONNECTED && isTimeSet()) {
    checkScheduledRestart();
  }

  // NTP Resync แบบ background
  if (WiFi.status() == WL_CONNECTED && now - lastNtpResync >= NTP_RESYNC_INTERVAL) {
    lastNtpResync = now;
    configTime(7 * 3600, 0, "th.pool.ntp.org", "pool.ntp.org", "time.google.com");
    Serial.println("Background NTP retry sent");
  }

  // Telegram Poll
  if (bot && WiFi.status() == WL_CONNECTED &&
      (now - lastPollTime >= POLL_INTERVAL)) {
    lastPollTime = now;
    pollTelegram();
  }

  // WiFi Reconnect
  static bool wifiWasDisconnected = false;

  if (WiFi.status() != WL_CONNECTED) {
    wifiWasDisconnected = true;
    if (now - lastReconnect >= 5000) {
      lastReconnect = now;
      Serial.println("WiFi disconnected — attempting reconnect");
      lcdStatus("WiFi Lost", "Reconnecting");
      WiFi.reconnect();
    }
  } else {
    if (wifiWasDisconnected) {
      wifiWasDisconnected = false;
      reconnectTelegramBot(false);
    }
  }

  // Heap Protection
  uint32_t freeHeap  = ESP.getFreeHeap();
  uint32_t maxBlock  = ESP.getMaxFreeBlockSize();

  if (freeHeap < MIN_FREE_HEAP || maxBlock < MIN_FREE_BLOCK_SIZE) {
    Serial.printf("Low Heap (free=%u, maxBlock=%u) — Restarting\n", freeHeap, maxBlock);
    ESP.restart();
  }

  // Heap Monitor
  static unsigned long lastHeapLog = 0;
  if (millis() - lastHeapLog > 60000) {
    lastHeapLog = millis();
    Serial.printf("Heap: %u bytes, MaxBlock: %u bytes\n", freeHeap, maxBlock);
  }
}

// ============================================================
// Read Sensor
// ============================================================

void readSensor() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {
    lcdStatus("Sensor Error", "Check DHT11 D4");
    sensorFailCount++;

    if (sensorFailCount >= MAX_SENSOR_FAIL_ALERT && !sensorFailAlerted &&
        WiFi.status() == WL_CONNECTED) {
      if (notifyAll("*Sensor Error*\nDHT11 อ่านค่าไม่ได้ติดต่อกันหลายครั้ง กรุณาตรวจสอบสาย/เซนเซอร์")) {
        sensorFailAlerted = true;
      }
    }
    return;
  }

  if (sensorFailAlerted && WiFi.status() == WL_CONNECTED) {
    notifyAll("*Sensor OK*\nDHT11 กลับมาอ่านค่าได้ปกติแล้ว");
  }
  sensorFailCount   = 0;
  sensorFailAlerted = false;

  temperature = t;
  humidity    = h;
}

// ============================================================
// LCD Update
// ============================================================

void updateLCD() {

  lcd.setCursor(0, 0);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  lcd.print("                ");

  lcd.setCursor(0, 0);
  lcd.write(byte(0));
  lcd.print(" Temp:");
  lcd.print(temperature, 1);
  lcd.print((char)223);
  lcd.print("C");

  lcd.setCursor(0, 1);
  lcd.write(byte(1));
  lcd.print(" Hum:");
  lcd.print(humidity, 1);
  lcd.print("%");

  if (temperature > TEMP_THRESHOLD) {
    lcd.setCursor(14, 0);
    lcd.print("!!");
  }

  if (sensorFailCount > 0) {
    lcd.setCursor(14, 1);
    lcd.print("!!");
  }
}

// ============================================================
// Alert
// ============================================================

void checkAndAlert() {

  unsigned long now = millis();

  if (temperature > TEMP_THRESHOLD) {

    if (!alertSent || (now - lastAlertTime >= ALERT_INTERVAL)) {

      String msg;
      msg.reserve(256);
      msg  = "*Alert! High Temperature*\n\n";
      msg += "Temp: *" + String(temperature, 1) + " C*\n";
      msg += "Humidity: " + String(humidity, 1) + "%\n";
      msg += "Threshold: " + String(TEMP_THRESHOLD, 0) + " C\n\n";
      msg += "Please check immediately!";

      if (notifyAll(msg)) {
        alertSent = true;
        lastAlertTime = now;
      }
    }

  } else {

    if (alertSent) {
      String msg;
      msg.reserve(128);
      msg  = "*Temperature back to normal*\n\n";
      msg += "Temp: *" + String(temperature, 1) + " C*\n";
      msg += "Humidity: " + String(humidity, 1) + "%";

      if (notifyAll(msg)) {
        alertSent = false;
      }
    }
  }
}

// ============================================================
// Daily Report (Telegram อย่างเดียว)
// ============================================================

void checkDailyReport() {

  time_t now = time(nullptr);

  struct tm timeInfo;
  localtime_r(&now, &timeInfo);

  if (timeInfo.tm_hour == DAILY_REPORT_HOUR &&
      timeInfo.tm_min  == DAILY_REPORT_MINUTE &&
      timeInfo.tm_yday != lastReportDay) {

    lastReportDay = timeInfo.tm_yday;

    String msg;
    msg.reserve(256);
    msg  = "*Daily Report — 17:00*\n\n";
    msg += "Temp: *" + String(temperature, 1) + " C*\n";
    msg += "Humidity: " + String(humidity, 1) + "%\n";
    msg += "WiFi RSSI: " + String(WiFi.RSSI()) + " dBm\n";
    msg += "Heap: " + String(ESP.getFreeHeap()) + " bytes\n";
    msg += "Uptime: " + formatUptime(millis());

    notifyAll(msg, false); // false = ไม่ส่งเข้า LINE (ประหยัดโควตา)
  }
}

// ============================================================
// Scheduled Weekly Restart
// ============================================================

void checkScheduledRestart() {

  time_t now = time(nullptr);

  struct tm timeInfo;
  localtime_r(&now, &timeInfo);

  if (timeInfo.tm_wday == WEEKLY_RESTART_WDAY &&
      timeInfo.tm_hour == WEEKLY_RESTART_HOUR &&
      timeInfo.tm_min  == WEEKLY_RESTART_MINUTE &&
      timeInfo.tm_yday != lastRestartYday) {

    lastRestartYday = timeInfo.tm_yday;

    notifyAll("*Scheduled Restart*\nรีสตาร์ทเครื่องตามรอบสัปดาห์ (บำรุงรักษาระบบ) จะกลับมาใน ~30 วิ\n\n"
              "หมายเหตุ: หลัง restart อาจต้องตั้งเวลาใหม่ด้วย /settime ถ้า NTP ยังใช้ไม่ได้");
    delay(500);

    Serial.println("Scheduled weekly restart triggered");
    ESP.restart();
  }
}

// ============================================================
// Telegram Poll
// ============================================================

void pollTelegram() {

  int numMessages = bot->getUpdates(bot->last_message_received + 1);

  if (numMessages == 0) return;

  telegramFailCount = 0;

  int batches = 0;

  while (numMessages > 0 && batches < MAX_POLL_BATCHES) {
    batches++;

    for (int i = 0; i < numMessages; i++) {

      telegramMessage msg = bot->messages[i];

      String fromChatId  = msg.chat_id;
      String rawText     = msg.text;
      String fromName    = msg.from_name;

      if (!isAuthorizedChat(fromChatId)) {
        Serial.print("Ignored message from unauthorized chat: ");
        Serial.println(fromChatId);
        continue;
      }

      String text = rawText;
      text.toLowerCase();
      text.trim();

      // /settime YYYY-MM-DD HH:MM:SS
      if (text.startsWith("/settime")) {

        String arg = rawText.substring(rawText.indexOf(' ') + 1);
        arg.trim();

        if (rawText.indexOf(' ') == -1 || !setManualTime(arg)) {
          bot->sendMessage(fromChatId,
            "รูปแบบไม่ถูกต้อง ใช้แบบนี้:\n`/settime 2026-08-31 17:15:00`",
            "Markdown");
        } else {
          String reply = "*ตั้งเวลาเรียบร้อย*\nเวลาปัจจุบัน: " + currentTimeString();
          bot->sendMessage(fromChatId, reply, "Markdown");
        }
      }

      // hello
      else if (text == "hello" || text == "สวัสดี") {

        String reply;
        reply.reserve(256);
        reply  = "Hello *" + fromName + "*!\n\n";
        reply += "*Temperature Status:*\n";
        reply += "Temp: *" + String(temperature, 1) + " C*\n";
        reply += "Humidity: " + String(humidity, 1) + "%\n";
        reply += "Alert at: > " + String(TEMP_THRESHOLD, 0) + " C\n\n";
        reply += (temperature > TEMP_THRESHOLD)
                 ? "Status: *HIGH TEMP!*"
                 : "Status: *Normal*";

        bot->sendMessage(fromChatId, reply, "Markdown");
      }

      // status
      else if (text == "status" || text == "สถานะ" || text == "/status") {

        String reply;
        reply.reserve(360);
        reply  = "*System Report*\n\n";
        reply += "Temp: *" + String(temperature, 1) + " C*\n";
        reply += "Humidity: " + String(humidity, 1) + "%\n";
        reply += "WiFi RSSI: " + String(WiFi.RSSI()) + " dBm\n";
        reply += "Heap: " + String(ESP.getFreeHeap()) + " bytes\n";
        reply += "MaxBlock: " + String(ESP.getMaxFreeBlockSize()) + " bytes\n";
        reply += "IP: " + WiFi.localIP().toString() + "\n";
        reply += "Uptime: " + formatUptime(millis()) + "\n";
        reply += "Clock: " + currentTimeString() + "\n";
        reply += "LINE: " + String(lineEnabled() ? "enabled" : "disabled");

        bot->sendMessage(fromChatId, reply, "Markdown");
      }

      // linetest — ทดสอบส่งข้อความเข้า LINE
      else if (text == "linetest" || text == "/linetest") {

        if (!lineEnabled()) {
          bot->sendMessage(fromChatId,
            "LINE ยังไม่ได้ตั้งค่า (LINE_CHANNEL_TOKEN / LINE_TO)", "");
        } else {
          bool ok = sendLine("ทดสอบแจ้งเตือน LINE จาก Temp Monitor\nTemp: " +
                             String(temperature, 1) + " C  Humidity: " +
                             String(humidity, 1) + "%");
          bot->sendMessage(fromChatId,
            ok ? "ส่ง LINE สำเร็จ" : "ส่ง LINE ไม่สำเร็จ ดูรายละเอียดที่ Serial Monitor",
            "");
        }
      }

      // help
      else if (text == "help" || text == "/help") {

        String reply;
        reply.reserve(360);
        reply  = "*Commands:*\n\n";
        reply += "`hello` - Temperature status\n";
        reply += "`status` - Full system info\n";
        reply += "`linetest` - ทดสอบส่งแจ้งเตือนเข้า LINE\n";
        reply += "`/settime YYYY-MM-DD HH:MM:SS` - ตั้งเวลาเครื่อง (เวลาไทย)\n";
        reply += "`help` - This menu\n\n";
        reply += "Auto-alert (Telegram + LINE) when temp > ";
        reply += String(TEMP_THRESHOLD, 0);
        reply += " C";

        bot->sendMessage(fromChatId, reply, "Markdown");
      }

      // unknown
      else {
        bot->sendMessage(fromChatId,
          "Unknown command.\nType `help`", "Markdown");
      }
    }

    yield();
    numMessages = bot->getUpdates(bot->last_message_received + 1);
  }
}

// ============================================================
// Format Uptime
// ============================================================

String formatUptime(unsigned long ms) {

  char buf[32];

  unsigned long sec = ms / 1000;
  unsigned long mn  = sec / 60;
  unsigned long hr  = mn / 60;
  unsigned long day = hr / 24;

  hr  %= 24;
  mn  %= 60;
  sec %= 60;

  if (day > 0) {
    snprintf(buf, sizeof(buf), "%lud %02lu:%02lu:%02lu", day, hr, mn, sec);
  } else {
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", hr, mn, sec);
  }

  return String(buf);
}