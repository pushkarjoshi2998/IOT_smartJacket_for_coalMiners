//A
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_now.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <PulseSensorPlayground.h>

const char* ssid = "Galaxy M52 5G";
const char* password = "gbshome12";
const char* serverURL = "http://10.221.82.1:5000/status"; 

const uint8_t jacketID = 1;
const char* mappedTagID = "120681202479";

uint8_t peerAddresses[][6] = {
  {0x54,0x43,0xB2,0x43,0xC0,0x48} 
};
const int peerCount = sizeof(peerAddresses) / 6;

uint8_t secretKey[16] = {
  0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
  0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10
};

LiquidCrystal_I2C lcd(0x27, 16, 2);
volatile uint8_t currentAlertType = 255;  // 255 = no alert
char lcdLine1[19] = "Smart Jacket B";     // 16 chars + null
char lcdLine2[19] = "Waiting Signal...";
unsigned long lastAlertTime = 0;

#define TRIG_PIN 5
#define ECHO_PIN 18
#define MQ2_PIN 34
#define DHT_PIN 32
#define LDR_PIN 36
#define RGB_PIN 4
#define PULSE_PIN 12
#define GAS_LED_PIN 16
#define PANIC_BUTTON 35   
#define ALERT_LED 2
#define ALERT_BUZZER 14   
#define DHTTYPE DHT11
DHT dht(DHT_PIN, DHTTYPE);

PulseSensorPlayground pulseSensor;

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

const uint16_t MQ2_GAS_THRESHOLD = 3000;
const uint16_t PULSE_RAW_THRESHOLD = 550;
const uint16_t PULSE_BPM_LOW = 40;
const uint16_t PULSE_BPM_HIGH = 140;
const float TEMP_THRESHOLD_C = 30.0;
const uint16_t LDR_DARK_THRESHOLD = 1000; 
const uint16_t JACKET_OFF_DISTANCE_CM = 100; 

volatile bool alertActive = false;
AlertMessage currentAlert = {0,255};
unsigned long alertEndTime = 0;

enum JacketState { DORMANT, ACTIVE };
volatile JacketState currentState = DORMANT;

unsigned long lastHeartbeatSent = 0;
unsigned long lastHeartbeatReceived = 0;
unsigned long shiftStartTime = 0;
uint8_t missedHeartbeats = 0;

enum LedMode { NORMAL, PANIC, SHIFT_END, DISCONNECTED };
volatile LedMode currentLedMode = NORMAL;

static inline uint8_t alertPriority(uint8_t type){
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

void showLCD(const char *l1, const char *l2){
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(l1);
  lcd.setCursor(0,1); lcd.print(l2);
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

void sendMessage(uint8_t alertType){
  if(currentState != ACTIVE) return;
  AlertMessage msg;
  msg.senderID = jacketID;
  msg.alertType = alertType;
  for(int i=0;i<peerCount;i++){
    esp_err_t res = esp_now_send(peerAddresses[i], (uint8_t*)&msg, sizeof(msg));
    Serial.printf("[ESPNOW] Sent alert type %u to peer %02X:%02X:%02X:%02X:%02X:%02X -> %s\n",
                  alertType,
                  peerAddresses[i][0], peerAddresses[i][1], peerAddresses[i][2],
                  peerAddresses[i][3], peerAddresses[i][4], peerAddresses[i][5],
                  res==ESP_OK?"SUCCESS":"FAILURE");
  }
}

void activateAlert(uint8_t type){
    if(!alertActive || alertPriority(type) >= alertPriority(currentAlert.alertType)){
        currentAlert.senderID = 0;
        currentAlert.alertType = type;
        alertActive = true;
        alertEndTime = millis() + 8000;
        lastAlertTime = millis();  // record alert time
        currentAlertType = type;

        switch(type){
            case 0: strncpy(lcdLine2, "PANIC ALERT!      ", 16); break;
            case 3: strncpy(lcdLine2, "GAS ALERT!        ", 16); break;
            case 4: strncpy(lcdLine2, "HR ALERT!         ", 16); break;
            case 5: strncpy(lcdLine2, "TEMP HIGH!        ", 16); break;
            case 2: strncpy(lcdLine2, "Shift Reminder    ", 16); break;
            default: strncpy(lcdLine2, "Unknown Alert     ", 16); break;
        }
    }

    switch(type){
        case 0:
            currentLedMode = PANIC;
            digitalWrite(GAS_LED_PIN, HIGH);
            tone(ALERT_BUZZER, 1200);
            Serial.println("[ALERT] PANIC activated!");
            break;
        case 3:
            digitalWrite(GAS_LED_PIN, HIGH);
            tone(ALERT_BUZZER, 1000, 500);
            Serial.println("[ALERT] Gas alert received!");
            break;
        case 4:
            digitalWrite(GAS_LED_PIN, HIGH);
            tone(ALERT_BUZZER, 900, 400);
            Serial.println("[ALERT] Heart rate alert received!");
            break;
        case 5:
            digitalWrite(GAS_LED_PIN, HIGH);
            tone(ALERT_BUZZER, 800, 400);
            Serial.println("[ALERT] Temperature alert received!");
            break;
        case 2:
            Serial.println("[INFO] Shift reminder triggered.");
            break;
        case 255:
            strncpy(lcdLine2, "Waiting Signal... ", 16);
            break;
        default:
            Serial.printf("[ALERT] Unknown alert type: %u\n", type);
            break;
    }
}

void handleIncomingAlert(const AlertMessage &incoming){
  if(incoming.alertType == 255){
    lastHeartbeatReceived = millis();
    missedHeartbeats = 0;
    return;
  }
  activateAlert(incoming.alertType);
}

void sendSensorPayload(const SensorPayload &p){
  // Reuse ESP-NOW to broadcast sensor payload (optional)
  for(int i=0;i<peerCount;i++){
    esp_err_t res = esp_now_send(peerAddresses[i], (uint8_t*)&p, sizeof(p));
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

void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len){
  if(len != sizeof(AlertMessage)) return;
  AlertMessage msg;
  memcpy(&msg, data, sizeof(msg));
  Serial.printf("[ESPNOW] Received alert type %u from Jacket ID %u\n", msg.alertType, msg.senderID);
  handleIncomingAlert(msg);
}

void OnDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status){
  Serial.printf("[ESPNOW] Send status: %s\n", status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAILURE");
}

TaskHandle_t PanicHandle = NULL;
TaskHandle_t HeartbeatHandle = NULL;
TaskHandle_t LEDHandle = NULL;
TaskHandle_t LCDHandle = NULL;
TaskHandle_t SensorHandle = NULL;

void PanicButtonTask(void *pv){
  pinMode(PANIC_BUTTON, INPUT_PULLUP);
  for(;;){
    if(currentState == ACTIVE && digitalRead(PANIC_BUTTON) == LOW){
      vTaskDelay(pdMS_TO_TICKS(30));
      if(digitalRead(PANIC_BUTTON) == LOW){
        sendMessage(0);
        Serial.println("[INPUT] Panic button pressed. Alert sent.");
        activateAlert(0);
        vTaskDelay(pdMS_TO_TICKS(1500));
      }
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
        Serial.println("[HEARTBEAT] Sent heartbeat.");
      }
      if(now - shiftStartTime >= 20000){
        sendMessage(2);
        shiftStartTime = now;
        Serial.println("[HEARTBEAT] Shift reminder sent.");
      }
      if(now - lastHeartbeatReceived > 10000){
        missedHeartbeats++;
        if(missedHeartbeats >= 2 && currentLedMode != DISCONNECTED){
          currentLedMode = DISCONNECTED;
          Serial.println("[WARNING] Peer disconnected detected!");
        }
      } else if(currentLedMode == DISCONNECTED){
        currentLedMode = NORMAL;
        Serial.println("[INFO] Peer reconnected. Status NORMAL.");
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
  pinMode(GAS_LED_PIN, OUTPUT);
  pinMode(ALERT_BUZZER, OUTPUT);

  digitalWrite(GAS_LED_PIN, LOW);
  digitalWrite(ALERT_LED, LOW);

  for(;;){
    if(currentState == ACTIVE){
      switch(currentLedMode){
        case NORMAL:
          digitalWrite(ALERT_LED, LOW);
          noTone(ALERT_BUZZER);
          break;
        case PANIC:
          digitalWrite(ALERT_LED, !digitalRead(ALERT_LED));
          tone(ALERT_BUZZER, 1200);
          vTaskDelay(pdMS_TO_TICKS(200));
          break;
        case SHIFT_END:
          digitalWrite(ALERT_LED, !digitalRead(ALERT_LED));
          tone(ALERT_BUZZER, 800, 150);
          vTaskDelay(pdMS_TO_TICKS(500));
          break;
        case DISCONNECTED:
          digitalWrite(ALERT_LED, !digitalRead(ALERT_LED));
          tone(ALERT_BUZZER, 1000, 200);
          vTaskDelay(pdMS_TO_TICKS(500));
          break;
      }
    } else {
      digitalWrite(ALERT_LED, LOW);
      noTone(ALERT_BUZZER);
      digitalWrite(GAS_LED_PIN, LOW);
    }

    if(alertActive && millis() > alertEndTime){
      alertActive = false;
      digitalWrite(GAS_LED_PIN, LOW);
      noTone(ALERT_BUZZER);
      if(currentLedMode != DISCONNECTED) currentLedMode = NORMAL;
      Serial.println("[INFO] Alert cleared.");
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void LCDTask(void *pv){
    lcd.init();
    lcd.backlight();

    lcd.setCursor(0,0);
    lcd.print("Smart Jacket B");
    lcd.setCursor(0,1);
    lcd.print("Initializing...");

    uint8_t lastAlertType = 255;
    char lastLine2[19];
    strncpy(lastLine2, lcdLine2, 17);

    for(;;){
        unsigned long now = millis();

        // If no alert for 8 seconds, revert to default
        if(!alertActive && (now - lastAlertTime > 8000)){
            if(currentState == ACTIVE){
                if(currentLedMode == DISCONNECTED)
                    strncpy(lcdLine2, "DISCONNECTED      ", 16);
                else
                    strncpy(lcdLine2, "Status: ACTIVE    ", 16);
            } else {
                strncpy(lcdLine2, "Dormant Mode      ", 16);
            }
            currentAlertType = 255;
        }

        // Only update LCD when the text changes
        if(currentAlertType != lastAlertType || strncmp(lcdLine2, lastLine2, 16) != 0){
            lcd.setCursor(0,0);
            lcd.print("Smart Jacket B    ");
            lcd.setCursor(0,1);
            lcd.print(lcdLine2);
            lastAlertType = currentAlertType;
            strncpy(lastLine2, lcdLine2, 17);
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void postSimpleSensorToServer(float tempC, uint16_t mq2_raw, int bpm) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] WiFi not connected, skipping POST");
    return;
  }

  WiFiClient client;
  HTTPClient http;

  String webserverURL= "http://10.221.82.1:5000/sensordata";
  http.begin(webserverURL);
  http.addHeader("Content-Type", "application/json");

  String payload = "{";
  payload += "\"id\":" + String(mappedTagID) + ",";
  payload += "\"temperutre\":" + String(tempC, 1) + ",";
  payload += "\"gas senoser\":" + String(mq2_raw) + ",";
  payload += "\"pluse\":" + String(bpm);
  payload += "}";

  int code = http.POST(payload);
  Serial.printf("[HTTP] POST -> %d | %s\n", code, payload.c_str());
  
  if (code > 0) {
    String response = http.getString();
    Serial.println("[HTTP] Response: " + response);
  } else {
    Serial.printf("[HTTP] Error: %s\n", http.errorToString(code).c_str());
  }

  http.end();
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
  digitalWrite(GAS_LED_PIN, LOW);
  digitalWrite(RGB_PIN, LOW);

  unsigned long lastSample = 0;

  for(;;){
    unsigned long now = millis();
    if(currentState == ACTIVE && now - lastSample >= 7000){  // every 7s
      lastSample = now;

      uint16_t mq2_raw = readAnalogAvg(MQ2_PIN, 6);
      uint16_t ldr_raw = readAnalogAvg(LDR_PIN, 6);
      uint16_t ultrasonic = readUltrasonicCM();
      float tempC = dht.readTemperature();
      int bpm = pulseSensor.getBeatsPerMinute();

      uint8_t flags = 0;
      if(mq2_raw >= MQ2_GAS_THRESHOLD) flags |= 0x01;
      if(bpm > 0 && (bpm < PULSE_BPM_LOW || bpm > PULSE_BPM_HIGH)) flags |= 0x02;
      uint16_t pulse_raw = readAnalogAvg(PULSE_PIN, 4);
      if(pulse_raw >= PULSE_RAW_THRESHOLD) flags |= 0x02;
      if(tempC > TEMP_THRESHOLD_C) flags |= 0x04;

      if(ultrasonic == 65535 || ultrasonic > JACKET_OFF_DISTANCE_CM){
        sendMessage(0);
        alertActive = true;
        currentAlert.senderID = jacketID;
        currentAlert.alertType = 0;
        alertEndTime = millis() + 8000;
        Serial.println("[SENSOR] Jacket not on. PANIC sent.");
      }

      // Immediate alerts
      if(flags & 0x01){ sendMessage(3); digitalWrite(GAS_LED_PIN, HIGH); Serial.println("[SENSOR] GAS threshold exceeded. Alert sent"); }
      if(flags & 0x02){ sendMessage(4); digitalWrite(GAS_LED_PIN, HIGH); Serial.println("[SENSOR] HR abnormal. Alert sent"); }
      if(flags & 0x04){ sendMessage(5); digitalWrite(GAS_LED_PIN, HIGH); Serial.println("[SENSOR] TEMP high. Alert sent"); }

      // LDR darkness -> RGB
      digitalWrite(RGB_PIN, (ldr_raw >= LDR_DARK_THRESHOLD) ? HIGH : LOW);

      // POST simplified payload to server every read
      postSimpleSensorToServer(tempC, mq2_raw, bpm);

      Serial.printf("[SENSOR] T=%.1fC MQ2=%u LDR=%u Dist=%u BPM=%d flags=0x%02X\n",
                    tempC, mq2_raw, ldr_raw, (ultrasonic==65535?999:ultrasonic), bpm, flags);

      // simple auto-off for GAS LED
      if(digitalRead(GAS_LED_PIN) == HIGH){
        unsigned long expire = millis() + 6000;
        while(millis() < expire) vTaskDelay(pdMS_TO_TICKS(100));
        digitalWrite(GAS_LED_PIN, LOW);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}


void startJacket(){
  if(currentState == ACTIVE) return;
  Serial.println("✅ [STATE] Jacket STARTED (ACTIVE).");
  alertActive = false;
  missedHeartbeats = 0;
  currentLedMode = NORMAL;
  lastHeartbeatSent = lastHeartbeatReceived = shiftStartTime = millis();
  currentState = ACTIVE;
}
void stopJacket(){
  if(currentState == DORMANT) return;
  Serial.println("🛑 [STATE] Jacket STOPPED (DORMANT).");
  currentState = DORMANT;
  alertActive = false;
  currentLedMode = NORMAL;
  missedHeartbeats = 0;
}

void setup(){
  Serial.begin(115200);
  delay(20);

  Wire.begin(21,22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0);
  lcd.print("Smart Jacket B     "); 
  lcd.setCursor(0,1);
  lcd.print("Booting . . .      "); 


  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("[WIFI] Connecting");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000){
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  if(WiFi.status() == WL_CONNECTED) Serial.println("[WIFI] connected successfully.");
  else Serial.println("[WIFI] not connected, continuing offline.");

  if(esp_now_init() != ESP_OK){
    Serial.println("[ESPNOW] Initialization FAILED");
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
        Serial.printf("[ESPNOW] Failed to add peer %d\n", i);
      }
    }
    Serial.println("[ESPNOW] Initialized and peers added successfully.");
  }

  xTaskCreatePinnedToCore(PanicButtonTask, "PanicBtn", 4096, NULL, 1, &PanicHandle, 1);
  xTaskCreatePinnedToCore(HeartbeatTask, "Heartbeat", 4096, NULL, 1, &HeartbeatHandle, 1);
  xTaskCreatePinnedToCore(LEDTask, "LED", 4096, NULL, 1, &LEDHandle, 1);
  xTaskCreatePinnedToCore(LCDTask, "LCD", 8192, NULL, 1, &LCDHandle, 1);
  xTaskCreatePinnedToCore(SensorTask, "Sensor", 8192, NULL, 1, &SensorHandle, 1);

  currentState = DORMANT;
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Waiting Signal...");
}

void loop(){
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if(now - lastCheck < 5000) return;
  lastCheck = now;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Disconnected. Attempting reconnect...");
    WiFi.reconnect();
    delay(1000);
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Reconnect failed. Skipping this cycle.");
      return;
    }
  }

  WiFiClient client;  // ✅ plain HTTP client
  HTTPClient http;
  http.begin(serverURL);

  Serial.println("[HTTP] Sending GET request...");
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    Serial.println("[HTTP] GET success. Payload received.");

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
      Serial.printf("[HTTP] JSON parse error: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[HTTP] GET request failed with code: %d\n", code);
  }

  http.end();
}
