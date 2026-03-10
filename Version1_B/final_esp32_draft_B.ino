// Smart Jacket B (Receiver)

#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

const char* ssid = "Poor nigga";
const char* password = "urpooraf";
const char* serverURL = "https://jsonkeeper.com/b/SDMU9"; // optional — used for start/stop logic


const uint8_t jacketID = 2;           
const char* mappedTagID = "1041750295380"; // tag used by your server to mark this jacket active

// Peers: Jacket A MAC(s) — REPLACE with the real MAC(s) from Jacket A
uint8_t peerAddresses[][6] = {
  {0xB0,0xA7,0x32,0x17,0x27,0xF0}
};
const int peerCount = sizeof(peerAddresses) / 6;

// LMK (must match Jacket A)
uint8_t secretKey[16] = {
  0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
  0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10
};

// -------------------- HARDWARE PINS --------------------
LiquidCrystal_I2C lcd(0x27, 16, 2); // I2C address 0x27, 16x2 LCD

#define PANIC_BUTTON 35
#define ALERT_LED    2   // status LED
#define GAS_LED_PIN  16  // gas/alert LED (user requested extra LED on 16)
#define VIB_PIN      17  // vibration motor on pin 17 (as you noted)
#define ALERT_BUZZER 27  // optional buzzer

// -------------------- MESSAGE STRUCT --------------------
typedef struct {
  uint8_t senderID;
  uint8_t alertType; // 0=panic/jacket-off, 3=gas, 4=hr, 5=temp, 2=shift reminder, 255=hb
} AlertMessage;

// -------------------- STATE --------------------
volatile bool alertActive = false;
AlertMessage currentAlert = {0,255};
unsigned long alertEndTime = 0;

unsigned long lastHeartbeatReceived = 0;
unsigned long lastHeartbeatSent = 0;
unsigned long shiftStartTime = 0;
uint8_t missedHeartbeats = 0;

enum JacketState { DORMANT, ACTIVE };
volatile JacketState currentState = DORMANT;

enum LedMode { NORMAL, PANIC, SHIFT_END, DISCONNECTED };
volatile LedMode currentLedMode = NORMAL;

// -------------------- HELPERS --------------------
static inline uint8_t alertPriority(uint8_t type){
  switch(type){
    case 0: return 5; // panic highest
    case 3: return 4; // gas
    case 4: return 4; // hr
    case 5: return 3; // temp
    case 2: return 2; // shift reminder
    case 255: return 1; // heartbeat
    default: return 0;
  }
}

void showLCD(const char *l1, const char *l2){
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(l1);
  lcd.setCursor(0,1); lcd.print(l2);
}

// -------------------- ESP-NOW SEND --------------------
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

// -------------------- ALERT HANDLING --------------------
void activateAlert(uint8_t type){
  // record alert if higher or none active
  if(!alertActive || alertPriority(type) >= alertPriority(currentAlert.alertType)){
    currentAlert.senderID = 0; // sender not used for display here
    currentAlert.alertType = type;
    alertActive = true;
    alertEndTime = millis() + 8000; // show for 8s by default
  }

  // immediate reactions
  switch(type){
    case 0: // PANIC / jacket off
      currentLedMode = PANIC;
      digitalWrite(GAS_LED_PIN, HIGH);
      digitalWrite(VIB_PIN, HIGH);
      tone(ALERT_BUZZER, 1200);
      showLCD("ALERT: PANIC!","Check wearer");
      Serial.println("[ALERT] PANIC received");
      break;
    case 3: // GAS
      digitalWrite(GAS_LED_PIN, HIGH);
      digitalWrite(VIB_PIN, HIGH);
      tone(ALERT_BUZZER, 1000, 500);
      showLCD("ALERT: GAS!","Evacuate");
      Serial.println("[ALERT] GAS received");
      break;
    case 4: // HR
      digitalWrite(GAS_LED_PIN, HIGH);
      digitalWrite(VIB_PIN, HIGH);
      tone(ALERT_BUZZER, 900, 400);
      showLCD("ALERT: HR!","Check vitals");
      Serial.println("[ALERT] HR received");
      break;
    case 5: // TEMP
      digitalWrite(GAS_LED_PIN, HIGH);
      digitalWrite(VIB_PIN, HIGH);
      tone(ALERT_BUZZER, 800, 400);
      showLCD("ALERT: TEMP!","Overheat");
      Serial.println("[ALERT] TEMP received");
      break;
    case 2: // shift reminder
      showLCD("Reminder:","Take break");
      Serial.println("[INFO] Shift reminder");
      break;
    case 255: // heartbeat - do nothing visible
      break;
    default:
      showLCD("ALERT:","Unknown type");
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

// -------------------- ESP-NOW CALLBACKS --------------------
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len){
  if(len != sizeof(AlertMessage)) return;
  AlertMessage msg;
  memcpy(&msg, data, sizeof(msg));
  Serial.printf("[ESPNOW] From ID=%u type=%u\n", msg.senderID, msg.alertType);
  handleIncomingAlert(msg);
}

void OnDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
  Serial.printf("[ESPNOW] Send callback -> %s\n",
                status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}


// -------------------- TASKS --------------------
TaskHandle_t PanicHandle = NULL;
TaskHandle_t HeartbeatHandle = NULL;
TaskHandle_t LEDHandle = NULL;
TaskHandle_t LCDHandle = NULL;

void PanicButtonTask(void *pv){
  pinMode(PANIC_BUTTON, INPUT_PULLUP);
  for(;;){
    if(currentState == ACTIVE && digitalRead(PANIC_BUTTON) == LOW){
      // debounce simple
      vTaskDelay(pdMS_TO_TICKS(30));
      if(digitalRead(PANIC_BUTTON) == LOW){
        sendMessage(0); // send PANIC
        Serial.println("[INPUT] Panic pressed -> sent");
        // give user feedback locally too
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
        sendMessage(255); // heartbeat
        lastHeartbeatSent = now;
      }
      // periodic shift reminder every 20s (keeps parity with Jacket A behaviour used while testing)
      if(now - shiftStartTime >= 20000){
        sendMessage(2);
        shiftStartTime = now;
      }
      // detect missed heartbeats
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
  pinMode(GAS_LED_PIN, OUTPUT);
  pinMode(VIB_PIN, OUTPUT);
  pinMode(ALERT_BUZZER, OUTPUT);

  digitalWrite(GAS_LED_PIN, LOW);
  digitalWrite(VIB_PIN, LOW);
  digitalWrite(ALERT_LED, LOW);

  unsigned long gasVibOffTime = 0;

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
      digitalWrite(VIB_PIN, LOW);
    }

    // Auto-shut gas/vib after alertEndTime
    if(alertActive && millis() > alertEndTime){
      alertActive = false;
      digitalWrite(GAS_LED_PIN, LOW);
      digitalWrite(VIB_PIN, LOW);
      noTone(ALERT_BUZZER);
      if(currentLedMode != DISCONNECTED) currentLedMode = NORMAL;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void LCDTask(void *pv){
  Wire.begin(21,22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Smart Jacket B");
  lcd.setCursor(0,1);
  lcd.print("Waiting Signal...");

  for(;;){
    if(currentState == ACTIVE){
      if(alertActive){
        // show alert until it times out
        switch(currentAlert.alertType){
          case 0: lcd.setCursor(0,1); lcd.print("PANIC / JacketOff "); break;
          case 3: lcd.setCursor(0,1); lcd.print("GAS ALERT!        "); break;
          case 4: lcd.setCursor(0,1); lcd.print("HR ALERT!         "); break;
          case 5: lcd.setCursor(0,1); lcd.print("TEMP HIGH!        "); break;
          case 2: lcd.setCursor(0,1); lcd.print("Shift reminder    "); break;
          default: lcd.setCursor(0,1); lcd.print("Alert received    "); break;
        }
      } else {
        lcd.setCursor(0,1);
        if(currentLedMode == DISCONNECTED) lcd.print("DISCONNECTED      ");
        else lcd.print("Status: ACTIVE    ");
      }
    } else {
      lcd.setCursor(0,1);
      lcd.print("Dormant Mode      ");
    }

    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// -------------------- START / STOP --------------------
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

// -------------------- SETUP --------------------
void setup(){
  Serial.begin(115200);
  delay(20);

  // I2C / LCD early to avoid garbage
  Wire.begin(21,22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Smart Jacket B");
  lcd.setCursor(0,1);
  lcd.print("Booting...");

  // WiFi start (used only for server polling if configured)
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("[WIFI] Connecting");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 8000){
    Serial.print(".");
    delay(300);
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

  // create tasks
  xTaskCreatePinnedToCore(PanicButtonTask, "PanicBtn", 4096, NULL, 1, &PanicHandle, 1);
  xTaskCreatePinnedToCore(HeartbeatTask, "Heartbeat", 4096, NULL, 1, &HeartbeatHandle, 1);
  xTaskCreatePinnedToCore(LEDTask, "LED", 4096, NULL, 1, &LEDHandle, 1);
  xTaskCreatePinnedToCore(LCDTask, "LCD", 6144, NULL, 1, &LCDHandle, 1);

  currentState = DORMANT;
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Waiting Signal...");
}

// -------------------- LOOP (server poll for start/stop) --------------------
void loop(){
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if(now - lastCheck < 5000) return;
  lastCheck = now;

  // If you don't want server polling, simply call startJacket() manually
  if(WiFi.status() != WL_CONNECTED){
    WiFi.reconnect();
    Serial.println("[WIFI] reconnect attempt...");
    if(WiFi.status() != WL_CONNECTED) return;
  }

  // Query server to find mappedTagID (keeps same UX as Jacket A)
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, serverURL);
  int code = http.GET();
  if(code == 200){
    String payload = http.getString();
    Serial.print("[HTTP] 200 payload: "); Serial.println(payload);
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, payload);
    if(!err){
      bool found = false;
      JsonArray users = doc["users"].as<JsonArray>();
      for(JsonObject u : users){
        if(u["tag_id"].as<const char*>() && strcmp(u["tag_id"].as<const char*>(), mappedTagID) == 0){
          found = true; break;
        }
      }
      if(found) startJacket(); else stopJacket();
    } else {
      Serial.print("[HTTP] JSON parse error: "); Serial.println(err.c_str());
    }
  } else {
    Serial.printf("[HTTP] GET failed, code: %d\n", code);
  }
  http.end();
}
