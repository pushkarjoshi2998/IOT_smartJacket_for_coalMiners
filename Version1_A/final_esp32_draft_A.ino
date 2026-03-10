// JacketA_Final.ino

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_now.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>

#include <DHT.h>
#include <PulseSensorPlayground.h>

const char* ssid = "Poor nigga";
const char* password = "urpooraf";
// Example JSONKeeper / mock endpoint - replace with your URL
const char* serverURL = "https://jsonkeeper.com/b/SDMU9"; 

// --- Jacket identity & peers
const uint8_t jacketID = 1;
const char* mappedTagID = "120681202479";

// peer MAC(s) - Jacket B
uint8_t peerAddresses[][6] = {
  {0x54,0x43,0xB2,0x43,0xC0,0x48} // replace with your peer MAC(s)
};
const int peerCount = sizeof(peerAddresses) / 6;

// optional LMK (must match peer if encrypt=true)
uint8_t secretKey[16] = {
  0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
  0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10
};

// --- I2C / LCD
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- Sensor pins (as agreed)
#define TRIG_PIN 5
#define ECHO_PIN 18
#define MQ2_PIN 34
#define DHT_PIN 32
#define LDR_PIN 36
#define RGB_PIN 4
#define PULSE_PIN 12
#define GAS_LED_PIN 16
#define VIB_PIN 14

#define PANIC_BUTTON 35   // keep your original if different
#define ALERT_LED 2
#define ALERT_BUZZER 14   // kept as original buzzer pin if used

// DHT
#define DHTTYPE DHT11
DHT dht(DHT_PIN, DHTTYPE);

// PulseSensor
PulseSensorPlayground pulseSensor;

// --- structs
typedef struct {
  uint8_t senderID;
  uint8_t alertType;
} AlertMessage;

typedef struct {
  uint8_t senderID;
  uint16_t bpm;
  float tempC;
  uint16_t mq2_raw;
  uint16_t ldr_raw;
  uint16_t dist_cm;
  uint8_t flags;
} SensorPayload;

// --- thresholds & timing
const uint16_t MQ2_GAS_THRESHOLD = 3000;
const uint16_t PULSE_RAW_THRESHOLD = 550;
const uint16_t PULSE_BPM_LOW = 40;
const uint16_t PULSE_BPM_HIGH = 140;
const float TEMP_THRESHOLD_C = 30.0;
const uint16_t LDR_DARK_THRESHOLD = 1000; // tune it
const uint16_t JACKET_OFF_DISTANCE_CM = 100; // ultrasonic threshold

volatile bool alertActive = false;
AlertMessage currentAlert;
unsigned long alertEndTime = 0;

// heartbeat / state
enum JacketState { DORMANT, ACTIVE };
volatile JacketState currentState = DORMANT;
unsigned long lastHeartbeatSent = 0;
unsigned long lastHeartbeatReceived = 0;
unsigned long shiftStartTime = 0;
uint8_t missedHeartbeats = 0;
enum LedMode { NORMAL, PANIC, SHIFT_END, DISCONNECTED };
volatile LedMode currentLedMode = NORMAL;

// --- helpers
uint8_t alertPriority(uint8_t type){
  switch(type){
    case 0: return 5;
    case 3: return 4;
    case 4: return 4;
    case 5: return 3;
    case 2: return 2;
    case 255: return 1;
    default: return 0;
  }
}

uint16_t readAnalogAvg(int pin, int samples=6){
  long s=0;
  for(int i=0;i<samples;i++){
    s += analogRead(pin);
    delay(2);
  }
  return (uint16_t)(s/samples);
}

uint16_t readUltrasonicCM(){
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout
  if(duration <= 0) return 65535;
  uint16_t cm = (uint16_t)(duration / 29 / 2);
  return cm;
}

// send alert messages (ESP-NOW)
void sendMessage(uint8_t alertType){
  if(currentState != ACTIVE) return;
  AlertMessage msg;
  msg.senderID = jacketID;
  msg.alertType = alertType;
  for(int i=0;i<peerCount;i++){
    esp_err_t res = esp_now_send(peerAddresses[i], (uint8_t*)&msg, sizeof(msg));
    Serial.printf("[ESPNOW] send(type=%u) -> %s\n", alertType, res==ESP_OK?"OK":"FAIL");
  }
}

void sendSensorPayload(const SensorPayload &p){
  // Reuse ESP-NOW to broadcast sensor payload (optional)
  for(int i=0;i<peerCount;i++){
    esp_err_t res = esp_now_send(peerAddresses[i], (uint8_t*)&p, sizeof(p));
    // don't spam serial for every peer
  }
}

void postSensorToServer(const SensorPayload &p){
  if(WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure(); // for testing with JSONKeeper; remove in production
  HTTPClient https;
  https.begin(client, serverURL);
  https.addHeader("Content-Type","application/json");
  String payload = "{";
  payload += "\"id\":" + String(p.senderID) + ",";
  payload += "\"temp\":" + String(p.tempC,2) + ",";
  payload += "\"bpm\":" + String(p.bpm) + ",";
  payload += "\"mq2\":" + String(p.mq2_raw) + ",";
  payload += "\"ldr\":" + String(p.ldr_raw) + ",";
  payload += "\"dist\":" + String(p.dist_cm) + ",";
  payload += "\"flags\":" + String(p.flags);
  payload += "}";
  int code = https.POST(payload);
  Serial.printf("[HTTP] POST /sensor -> %d\n", code);
  https.end();
}

// --- ESP-NOW callbacks
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len){
  if(len == sizeof(AlertMessage)){
    AlertMessage incoming;
    memcpy(&incoming, data, sizeof(incoming));
    if(incoming.alertType == 255){
      lastHeartbeatReceived = millis();
      missedHeartbeats = 0;
      return;
    }
    if(!alertActive || alertPriority(incoming.alertType) >= alertPriority(currentAlert.alertType)){
      currentAlert = incoming;
      alertActive = true;
      alertEndTime = millis() + 8000;
    }
    return;
  }
  // ignore other sizes for now
}

void OnDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status){
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "[ESPNOW] Send Success" : "[ESPNOW] Send Fail");
}

// --- Tasks
TaskHandle_t PanicHandle = NULL;
TaskHandle_t HeartbeatHandle = NULL;
TaskHandle_t LEDHandle = NULL;
TaskHandle_t LCDHandle = NULL;
TaskHandle_t SensorHandle = NULL;

void PanicButtonTask(void *pv){
  pinMode(PANIC_BUTTON, INPUT_PULLUP);
  for(;;){
    if(currentState == ACTIVE && digitalRead(PANIC_BUTTON) == LOW){
      sendMessage(0);
      Serial.println("[INPUT] Panic button pressed -> sending PANIC");
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void HeartbeatTask(void *pv){
  lastHeartbeatSent = lastHeartbeatReceived = shiftStartTime = millis();
  for(;;){
    if(currentState == ACTIVE){
      unsigned long now = millis();
      if(now - lastHeartbeatSent > 7000){
        sendMessage(255);
        lastHeartbeatSent = now;
      }
      if(now - shiftStartTime >= 20000){
        sendMessage(2);
        shiftStartTime = now;
      }
      if(now - lastHeartbeatReceived > 10000){
        missedHeartbeats++;
        if(missedHeartbeats >= 2) currentLedMode = DISCONNECTED;
      } else {
        if(currentLedMode == DISCONNECTED) currentLedMode = NORMAL;
      }
    } else {
      missedHeartbeats = 0;
      lastHeartbeatSent = lastHeartbeatReceived = shiftStartTime = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void LEDTask(void *pv){
  pinMode(ALERT_LED, OUTPUT);
  pinMode(ALERT_BUZZER, OUTPUT);
  pinMode(GAS_LED_PIN, OUTPUT);
  pinMode(RGB_PIN, OUTPUT);
  pinMode(VIB_PIN, OUTPUT);
  digitalWrite(GAS_LED_PIN, LOW);
  digitalWrite(RGB_PIN, LOW);
  digitalWrite(VIB_PIN, LOW);

  for(;;){
    if(currentState == ACTIVE){
      switch(currentLedMode){
        case NORMAL:
          digitalWrite(ALERT_LED, LOW);
          noTone(ALERT_BUZZER);
          break;
        case PANIC:
          digitalWrite(ALERT_LED, !digitalRead(ALERT_LED));
          tone(ALERT_BUZZER, 1000);
          vTaskDelay(pdMS_TO_TICKS(200));
          break;
        case SHIFT_END:
          digitalWrite(ALERT_LED, !digitalRead(ALERT_LED));
          tone(ALERT_BUZZER, 800, 150);
          vTaskDelay(pdMS_TO_TICKS(500));
          break;
        case DISCONNECTED:
          digitalWrite(ALERT_LED, !digitalRead(ALERT_LED));
          tone(ALERT_BUZZER, 1200, 200);
          vTaskDelay(pdMS_TO_TICKS(500));
          break;
      }
    } else {
      digitalWrite(ALERT_LED, LOW);
      noTone(ALERT_BUZZER);
      digitalWrite(GAS_LED_PIN, LOW);
      digitalWrite(RGB_PIN, LOW);
      digitalWrite(VIB_PIN, LOW);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void LCDTask(void *pv){
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Smart Jacket A");
  lcd.setCursor(0,1);
  lcd.print("Waiting Signal...");
  for(;;){
    lcd.setCursor(0,1);
    if(currentState == ACTIVE){
      unsigned long now = millis();
      if(alertActive && now < alertEndTime){
        if(currentAlert.alertType == 0){
          lcd.print("PANIC / JacketOff ");
          currentLedMode = PANIC;
        } else if(currentAlert.alertType == 3){
          lcd.print("GAS ALERT!        ");
          currentLedMode = PANIC;
        } else if(currentAlert.alertType == 4){
          lcd.print("HR ALERT!         ");
          currentLedMode = PANIC;
        } else if(currentAlert.alertType == 5){
          lcd.print("TEMP HIGH!        ");
          currentLedMode = PANIC;
        } else {
          lcd.print("Status: Active    ");
        }
      } else {
        lcd.print("Status: Active    ");
        alertActive = false;
        if(currentLedMode != DISCONNECTED) currentLedMode = NORMAL;
      }
    } else {
      lcd.print("Dormant Mode      ");
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void SensorTask(void *pv){
  dht.begin();
  pulseSensor.analogInput(PULSE_PIN);
  pulseSensor.setThreshold(PULSE_RAW_THRESHOLD);
  pulseSensor.begin();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(GAS_LED_PIN, OUTPUT);
  pinMode(RGB_PIN, OUTPUT);
  pinMode(VIB_PIN, OUTPUT);
  digitalWrite(GAS_LED_PIN, LOW);
  digitalWrite(RGB_PIN, LOW);
  digitalWrite(VIB_PIN, LOW);

  unsigned long lastSample = 0;
  int postCounter = 0;

  for(;;){
    unsigned long now = millis();
    if(currentState == ACTIVE && now - lastSample >= 7000){
      lastSample = now;

      uint16_t mq2_raw = readAnalogAvg(MQ2_PIN, 6);
      uint16_t ldr_raw = readAnalogAvg(LDR_PIN, 6);
      uint16_t ultrasonic = readUltrasonicCM();
      float tempC = dht.readTemperature();
      int bpm = pulseSensor.getBeatsPerMinute();

      uint8_t flags = 0;
      if(mq2_raw >= MQ2_GAS_THRESHOLD) flags |= 0x01;
      if(bpm > 0 && (bpm < PULSE_BPM_LOW || bpm > PULSE_BPM_HIGH)) flags |= 0x02;
      // raw amplitude fallback
      uint16_t pulse_raw = readAnalogAvg(PULSE_PIN, 4);
      if(pulse_raw >= PULSE_RAW_THRESHOLD) flags |= 0x02;
      if(tempC > TEMP_THRESHOLD_C) flags |= 0x04;
      if(ultrasonic == 65535 || ultrasonic > JACKET_OFF_DISTANCE_CM){
        // treat as jacket off => panic
        sendMessage(0);
        alertActive = true;
        currentAlert.senderID = jacketID;
        currentAlert.alertType = 0;
        alertEndTime = millis() + 8000;
        Serial.println("[SENSOR] Ultrasonic: jacket appears off -> PANIC sent");
      }

      // Immediate alerts
      if(flags & 0x01){
        sendMessage(3); // gas
        digitalWrite(GAS_LED_PIN, HIGH);
        digitalWrite(VIB_PIN, HIGH);
        Serial.println("[SENSOR] GAS threshold exceeded -> GAS alert sent");
      }
      if(flags & 0x02){
        sendMessage(4); // hr
        digitalWrite(GAS_LED_PIN, HIGH);
        digitalWrite(VIB_PIN, HIGH);
        Serial.println("[SENSOR] HR abnormal -> HR alert sent");
      }
      if(flags & 0x04){
        sendMessage(5); // temp
        digitalWrite(GAS_LED_PIN, HIGH);
        digitalWrite(VIB_PIN, HIGH);
        Serial.println("[SENSOR] TEMP high -> TEMP alert sent");
      }

      // LDR darkness -> RGB
      if(ldr_raw >= LDR_DARK_THRESHOLD) digitalWrite(RGB_PIN, HIGH);
      else digitalWrite(RGB_PIN, LOW);

      // Broadcast sensor payload via ESPNOW (optional)
      SensorPayload p;
      p.senderID = jacketID;
      p.bpm = (uint16_t)(bpm > 0 ? bpm : 0);
      p.tempC = tempC;
      p.mq2_raw = mq2_raw;
      p.ldr_raw = ldr_raw;
      p.dist_cm = (ultrasonic == 65535 ? 65535 : ultrasonic);
      p.flags = flags;
      sendSensorPayload(p);

      // occasional POST to server
      if(++postCounter >= 2){
        postCounter = 0;
        postSensorToServer(p);
      }

      Serial.printf("[SENSOR] T=%.1fC MQ2=%u LDR=%u Dist=%u BPM=%d flags=0x%02X\n",
                    tempC, mq2_raw, ldr_raw, (ultrasonic==65535?999:ultrasonic), bpm, flags);

      // simple auto-off after burst
      if(digitalRead(GAS_LED_PIN) == HIGH || digitalRead(VIB_PIN) == HIGH){
        unsigned long expire = millis() + 6000;
        while(millis() < expire) vTaskDelay(pdMS_TO_TICKS(100));
        digitalWrite(GAS_LED_PIN, LOW);
        digitalWrite(VIB_PIN, LOW);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// --- start/stop control (unchanged)
void startJacket(){
  if(currentState == ACTIVE) return;
  Serial.println("✅ startJacket -> ACTIVE");
  alertActive = false;
  missedHeartbeats = 0;
  currentLedMode = NORMAL;
  lastHeartbeatSent = lastHeartbeatReceived = shiftStartTime = millis();
  currentState = ACTIVE;
}
void stopJacket(){
  if(currentState == DORMANT) return;
  Serial.println("🛑 stopJacket -> DORMANT");
  currentState = DORMANT;
  alertActive = false;
  currentLedMode = NORMAL;
  missedHeartbeats = 0;
}

// --- setup & loop
void setup(){
  Serial.begin(115200);
  delay(10);

  // Wi-Fi (used for server polling). Keep STA mode.
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("[WIFI] Connecting");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000){
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  if(WiFi.status() == WL_CONNECTED) Serial.println("[WIFI] connected");
  else Serial.println("[WIFI] not connected (continuing)");

  // ESP-NOW init
  if(esp_now_init() != ESP_OK){
    Serial.println("[ESPNOW] init failed");
  } else {
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_register_send_cb(OnDataSent);
    for(int i=0;i<peerCount;i++){
      esp_now_peer_info_t peer = {};
      memcpy(peer.peer_addr, peerAddresses[i], 6);
      peer.channel = 0;
      peer.encrypt = true;
      memcpy(peer.lmk, secretKey, 16);
      if(esp_now_add_peer(&peer) != ESP_OK){
        Serial.printf("[ESPNOW] Peer add failed index %d\n", i);
      }
    }
  }

  // Create tasks
  xTaskCreatePinnedToCore(PanicButtonTask, "PanicBtn", 4096, NULL, 1, &PanicHandle, 1);
  xTaskCreatePinnedToCore(HeartbeatTask, "Heartbeat", 4096, NULL, 1, &HeartbeatHandle, 1);
  xTaskCreatePinnedToCore(LEDTask, "LED", 4096, NULL, 1, &LEDHandle, 1);
  xTaskCreatePinnedToCore(LCDTask, "LCD", 8192, NULL, 1, &LCDHandle, 1);
  xTaskCreatePinnedToCore(SensorTask, "Sensor", 8192, NULL, 1, &SensorHandle, 1);

  // start dormant by default
  currentState = DORMANT;
  Wire.begin();
  delay(100);
  Wire.begin(21,22); // explicit SDA/SCL
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Waiting Signal...");
}
void loop(){
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if(now - lastCheck < 5000) return;
  lastCheck = now;

  if(WiFi.status() != WL_CONNECTED){
    WiFi.reconnect();
    Serial.println("[WIFI] reconnect attempt...");
    if(WiFi.status() != WL_CONNECTED) return;
  }

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, serverURL); // GET status from server (mock or real)
  int code = http.GET();
  if(code == 200){
    String payload = http.getString();
    Serial.print("[HTTP] 200 payload: ");
    Serial.println(payload);
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, payload);
    if(!err){
      bool found = false;
      JsonArray users = doc["users"].as<JsonArray>();
      for(JsonObject u : users){
        if(u["tag_id"].as<const char*>() && strcmp(u["tag_id"].as<const char*>(), mappedTagID) == 0){
          found = true;
          break;
        }
      }
      if(found) startJacket();
      else stopJacket();
    } else {
      Serial.print("[HTTP] JSON parse error: ");
      Serial.println(err.c_str());
    }
  } else {
    Serial.printf("[HTTP] GET failed, code: %d\n", code);
  }
  http.end();
}
