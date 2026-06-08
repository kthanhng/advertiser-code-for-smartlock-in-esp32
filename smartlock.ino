#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include <Keypad.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEAdvertising.h>
#include <BLE2902.h>
#include "mbedtls/aes.h"
#include <ArduinoJson.h>

using namespace std;   // ← THÊM DÒNG NÀY
/* ================= WIFI ================= */
const char* ssid = "TOTOLINK_EX200";
const char* password = "kheckhec";

/* ================= FIREBASE ================= */
//const char* FIREBASE_HOST = "https://learn-prj-f5d0d-default-rtdb.firebaseio.com";
const char* FIREBASE_HOST = "https://smartlock-7f70d-default-rtdb.firebaseio.com";

unsigned long lastFirebasePoll = 0;
const unsigned long FIREBASE_POLL_INTERVAL = 30000;

/* ================= FIREBASE QUEUE ================= */
struct FirebaseJob {
    enum JobType { JOB_UNLOCK_HISTORY, JOB_CLOSED_EVENT };
    JobType type;
    char method[32];
    char mac[18];
    unsigned long timestamp;
};

QueueHandle_t firebaseQueue = NULL;
TaskHandle_t firebaseTaskHandle = NULL;

/* ================= LCD ================= */
LiquidCrystal_I2C lcd(0x27, 16, 2);

/* ================= SERVO ================= */
Servo myServo;
const int servoPin = 2;

/* ================= FACE ID UART ================= */
#define FACE_RX 16
#define FACE_TX 17

#define HALL_PIN 4

/* ================= hall sensor ================= */
enum DoorState {
    DOOR_IDLE,
    DOOR_WAITING_FOR_OPEN,
    DOOR_PHYSICALLY_OPEN
};
DoorState doorState = DOOR_IDLE;
unsigned long lastHallRead = 0;
String unlockMethod = "";
/* ================= KEYPAD ================= */
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS] = {26, 25, 33, 32};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

/* ================= PASSWORD ================= */
String masterPassword = "1245";
String inputPassword = "";

/* ================= SYSTEM STATE ================= */
bool faceEnabled = false;
bool keypadEnabled = true;
bool bleEnabled = true;

bool isSystemLocked = false;
unsigned long lockStartTime = 0;

/* ================= BLE ================= */
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHAR_CHALLENGE_UUID "12345678-1234-1234-1234-1234567890cd"
#define CHAR_RESPONSE_UUID  "12345678-1234-1234-1234-1234567890ef"
#define CHAR_UNLOCK_UUID    "12345678-1234-1234-1234-1234567890aa"   // MỚI
#define MAX_CLIENTS 3
#define AUTH_TIMEOUT_SEC 5

static const uint8_t AES_KEY[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
};

BLEServer *pServer = NULL;
BLECharacteristic *pChallengeChar = NULL;
BLECharacteristic *pResponseChar = NULL;
BLECharacteristic *pUnlockChar = NULL;   // MỚI

bool isAuthenticating = false;
uint16_t authenticatingConnId = 0;

struct ClientInfo {
    bool active = false;
    uint16_t connId = 0;
    uint8_t challenge[16] = {0};
    bool authenticated = false;
    char macAddress[18] = {0};
};

esp_timer_handle_t authTimer = NULL;
bool kickPending = false;

// MỚI — phone yêu cầu unlock (set trong callback, xử lý trong loop)
volatile bool unlockPending = false;
char unlockByMac[18] = {0};
String pendingLogMethod = "";
String pendingLogMac = "";
bool hasPendingLog = false;

void authTimeoutCallback(void* arg) {
    Serial.printf("[%lu][TIMER] Auth timeout fired!\n", millis());
    kickPending = true;
}

void startAuthTimer() {
    if (authTimer != NULL) {
        esp_timer_stop(authTimer);
    } else {
        esp_timer_create_args_t timerArgs = {};
        timerArgs.callback = authTimeoutCallback;
        timerArgs.name = "auth_timeout";
        esp_timer_create(&timerArgs, &authTimer);
    }
    kickPending = false;
    esp_timer_start_once(authTimer, AUTH_TIMEOUT_SEC * 1000000ULL);
}

void stopAuthTimer() {
    if (authTimer != NULL) esp_timer_stop(authTimer);
    kickPending = false;
}

ClientInfo clients[MAX_CLIENTS];

ClientInfo* findClient(uint16_t connId) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].connId == connId) return &clients[i];
    }
    return NULL;
}

ClientInfo* findFreeSlot() {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) return &clients[i];
    }
    return NULL;
}

int countActiveClients() {
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].active) count++;
    return count;
}

int countAuthenticatedClients() {
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].authenticated) count++;
    }
    return count;
}

void generateChallenge(uint8_t *buffer) {
    for (int i = 0; i < 16; i++) buffer[i] = esp_random() & 0xFF;
}

void aesEncrypt(const uint8_t *input, uint8_t *output, const uint8_t *key) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, input, output);
    mbedtls_aes_free(&aes);
}

// MỚI
void aesDecrypt(const uint8_t *input, uint8_t *output, const uint8_t *key) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, key, 128);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, input, output);
    mbedtls_aes_free(&aes);
}

void macToString(const uint8_t *addr, char *out) {
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
        addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

void unlockAndAdvertise() {
    Serial.printf("[%lu][UNLOCK_AUTH] BEFORE: isAuth=%s authConnId=%d active=%d\n",
        millis(), isAuthenticating ? "Y" : "N", authenticatingConnId, countActiveClients());

    stopAuthTimer();
    isAuthenticating = false;
    authenticatingConnId = 0;

    if (countActiveClients() < MAX_CLIENTS) {
        delay(100);
        BLEDevice::startAdvertising();
        Serial.printf("[%lu][UNLOCK_AUTH] Mo khoa -> Bat advertise\n", millis());
    } else {
        Serial.printf("[%lu][UNLOCK_AUTH] Mo khoa nhung het slot\n", millis());
    }
}

/* ================= BLE CALLBACKS ================= */

class ResponseCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        Serial.printf("[%lu][RESPONSE] onWrite isAuth=%s authConnId=%d\n",
            millis(), isAuthenticating ? "Y" : "N", authenticatingConnId);

        if (!isAuthenticating) {
            Serial.printf("[%lu][RESPONSE] SKIP - khong dang xac thuc\n", millis());
            return;
        }

        ClientInfo *client = findClient(authenticatingConnId);
        if (client == NULL) {
            Serial.printf("[%lu][RESPONSE] SKIP - khong tim thay client\n", millis());
            unlockAndAdvertise();
            return;
        }

        string value = pChar->getValue();
        Serial.printf("[%lu][RESPONSE] [%s] length=%d\n",
            millis(), client->macAddress, value.length());

        if (value.length() != 16) {
            Serial.printf("[%lu][RESPONSE] [%s] Sai length -> Kick\n", millis(), client->macAddress);
            pServer->disconnect(client->connId);
            return;
        }

        uint8_t expected[16];
        aesEncrypt(client->challenge, expected, AES_KEY);

        if (memcmp(expected, value.c_str(), 16) == 0) {
            client->authenticated = true;
            Serial.printf("[%lu][RESPONSE] [%s] XAC THUC THANH CONG! (Auth: %d/%d)\n",
                millis(), client->macAddress, countAuthenticatedClients(), MAX_CLIENTS);
            unlockAndAdvertise();
        } else {
            Serial.printf("[%lu][RESPONSE] [%s] SAI KEY -> Kick\n", millis(), client->macAddress);
            pServer->disconnect(client->connId);
        }
    }
};

class ChallengeDescriptorCallback : public BLEDescriptorCallbacks {
    void onWrite(BLEDescriptor *pDescriptor) override {
        Serial.printf("[%lu][CHALLENGE] Android bat notify, isAuth=%s authConnId=%d\n",
            millis(), isAuthenticating ? "Y" : "N", authenticatingConnId);

        if (isAuthenticating) {
            startAuthTimer();
            Serial.printf("[%lu][CHALLENGE] Reset timer cho client\n", millis());
        }
        pChallengeChar->notify();
        Serial.printf("[%lu][CHALLENGE] Da gui challenge notify\n", millis());
    }
};

// MỚI — nhận lệnh unlock từ phone
// class UnlockCallback : public BLECharacteristicCallbacks {
//     void onWrite(BLECharacteristic *pChar, esp_ble_gatts_cb_param_t *param) override {
//         uint16_t connId = param->write.conn_id;
//         ClientInfo *client = findClient(connId);
//         if (!client || !client->authenticated) {
//             Serial.printf("[UNLOCK] connId=%d chua auth -> reject\n", connId);
//             return;
//         }

//         // Tìm client đã auth (theo connId của lần write này)
//         // Đơn giản: nếu có client nào đang authed thì coi như chính nó gửi
//         ClientInfo *authedClient = NULL;
//         for (int i = 0; i < MAX_CLIENTS; i++) {
//             if (clients[i].active && clients[i].authenticated) {
//                 authedClient = &clients[i];
//                 break;
//             }
//         }
//         if (authedClient == NULL) {
//             Serial.printf("[%lu][UNLOCK] Chua co client auth -> ignore\n", millis());
//             return;
//         }

//         String value = pChar->getValue();
//         if (value.length() != 16) {
//             Serial.printf("[%lu][UNLOCK] Sai length %d\n", millis(), value.length());
//             return;
//         }

//         // Decrypt: payload = [timestamp(4) | magic "OPEN"(4) | random(8)]
//         uint8_t plain[16];
//         aesDecrypt((uint8_t*)value.c_str(), plain, AES_KEY);

//         // Check magic bytes
//         if (plain[4] != 'O' || plain[5] != 'P' || plain[6] != 'E' || plain[7] != 'N') {
//             Serial.printf("[%lu][UNLOCK] Magic sai -> reject\n", millis());
//             return;
//         }

//         // Lấy timestamp (big-endian)
//         uint32_t ts = ((uint32_t)plain[0] << 24) |
//                       ((uint32_t)plain[1] << 16) |
//                       ((uint32_t)plain[2] <<  8) |
//                        (uint32_t)plain[3];

//         // Anti-replay: timestamp phải lớn hơn lần trước
//         if (ts <= authedClient->lastUnlockTimestamp) {
//             Serial.printf("[%lu][UNLOCK] Replay! ts=%u last=%u\n",
//                 millis(), ts, authedClient->lastUnlockTimestamp);
//             return;
//         }
//         authedClient->lastUnlockTimestamp = ts;

//         // OK — set flag, loop() sẽ xử lý
//         strncpy(unlockByMac, authedClient->macAddress, 17);
//         unlockPending = true;
//         Serial.printf("[%lu][UNLOCK] [%s] OK ts=%u\n",
//             millis(), authedClient->macAddress, ts);
//     }
// };

class UnlockCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar, esp_ble_gatts_cb_param_t *param) override {
        uint16_t connId = param->write.conn_id;
        ClientInfo *client = findClient(connId);
        if (!client || !client->authenticated) {
            Serial.printf("[UNLOCK] connId=%d chua auth -> reject\n", connId);
            return;
        }

        string value = pChar->getValue();
        Serial.printf("[%lu][UNLOCK] [%s] nhan: '%s'\n",
            millis(), client->macAddress, value.c_str());

        if (value == "OPEN") {
            strncpy(unlockByMac, client->macAddress, 17);
            unlockPending = true;
            Serial.printf("[%lu][UNLOCK] [%s] OK\n", millis(), client->macAddress);
        } else {
            Serial.printf("[%lu][UNLOCK] [%s] Sai noi dung -> ignore\n",
                millis(), client->macAddress);
        }
    }
};

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) {
        uint16_t connId = param->connect.conn_id;
        char mac[18];
        macToString(param->connect.remote_bda, mac);

        Serial.printf("[%lu][CONNECT] connId=%d mac=%s isAuth=%s authConnId=%d active=%d\n",
            millis(), connId, mac, isAuthenticating ? "Y" : "N", authenticatingConnId, countActiveClients());

        if (isAuthenticating) {
            Serial.printf("[%lu][CONNECT] [%s] REJECT - dang xac thuc client khac\n", millis(), mac);
            pServer->disconnect(connId);
            return;
        }
        ClientInfo *client = findFreeSlot();
        if (client == NULL) {
            Serial.printf("[%lu][CONNECT] [%s] REJECT - het slot\n", millis(), mac);
            pServer->disconnect(connId);
            return;
        }
        client->active = true;
        client->connId = connId;
        client->authenticated = false;
        strncpy(client->macAddress, mac, 17);
        generateChallenge(client->challenge);

        isAuthenticating = true;
        authenticatingConnId = connId;

        pChallengeChar->setValue(client->challenge, 16);
        startAuthTimer();
        Serial.printf("[%lu][CONNECT] [%s] OK slot %d/%d -> KHOA, doi auth\n",
            millis(), mac, countActiveClients(), MAX_CLIENTS);
    }

    void onDisconnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) {
        uint16_t connId = param->disconnect.conn_id;

        Serial.printf("[%lu][DISCONNECT] connId=%d isAuth=%s authConnId=%d\n",
            millis(), connId, isAuthenticating ? "Y" : "N", authenticatingConnId);

        ClientInfo *client = findClient(connId);
        if (client != NULL) {
            Serial.printf("[%lu][DISCONNECT] [%s] wasAuth=%s\n",
                millis(), client->macAddress, client->authenticated ? "Y" : "N");
            client->active = false;
            client->authenticated = false;
            
        } else {
            Serial.printf("[%lu][DISCONNECT] Unknown connId=%d\n", millis(), connId);
        }

        if (isAuthenticating && connId == authenticatingConnId) {
            Serial.printf("[%lu][DISCONNECT] Client dang xac thuc disconnect -> Mo khoa\n", millis());
            unlockAndAdvertise();
        } else {
            if (countActiveClients() < MAX_CLIENTS) {
                delay(200);
                BLEDevice::startAdvertising();
                Serial.printf("[%lu][DISCONNECT] Bat lai advertise\n", millis());
            }
        }
    }
};

/* ================= LCD ================= */
void updateLCD() {
    lcd.clear();
    if (isSystemLocked) {
        lcd.setCursor(0,0); lcd.print("SYSTEM RESTING");
        lcd.setCursor(0,1); lcd.print("Wait 5s...");
        return;
    }
    lcd.setCursor(0,0);
    lcd.print("F:"); lcd.print(faceEnabled ? "ON" : "X");
    lcd.print(" K:"); lcd.print(keypadEnabled ? "ON" : "X");
    lcd.print(" B:"); lcd.print(bleEnabled ? "ON" : "X");

    lcd.setCursor(0,1);
    if (keypadEnabled && inputPassword.length() > 0) {
        lcd.print("Pass: ");
        for (int i = 0; i < inputPassword.length(); i++) lcd.print("*");
    } else {
        lcd.print("Ready...");
    }
}

/* ================= FIREBASE ================= */

void pollFirebaseConfig() {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure client;  // local, không static
    client.setInsecure();
    HTTPClient http;

    //String url = String(FIREBASE_HOST) + "/config.json";
    String url = String(FIREBASE_HOST) + "/devices/door_01/config.json";
    Serial.printf("[FB_POLL] heap=%d largest=%d\n", 
        ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    if (!http.begin(client, url)) {
        Serial.println("[FB_POLL] begin failed");
        return;
    }
    http.setTimeout(8000);
    http.setConnectTimeout(8000);
    http.useHTTP10(true);

    int code = http.GET();
    Serial.printf("[FB_POLL] GET -> %d\n", code);

    if (code == 200) {
        String payload = http.getString();
        http.end();
        client.stop();

        Serial.printf("[FB_POLL] payload: %s\n", payload.c_str()); // ← thêm dòng này

        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, payload))  {       
        Serial.println("[FB_POLL] JSON parse failed");  // ← thêm dòng này 
        return;}

        // In ra giá trị đọc được
        Serial.printf("[FB_POLL] face=%s ble=%s pass=%s\n",  // ← thêm block này
            doc.containsKey("face_enabled") ? (doc["face_enabled"] ? "true" : "false") : "N/A",
            doc.containsKey("ble_enabled")  ? (doc["ble_enabled"]  ? "true" : "false") : "N/A",
            doc.containsKey("password")     ? doc["password"].as<String>().c_str()      : "N/A"
        );
        
        bool changed = false;
        if (doc.containsKey("face_enabled")) {
            bool v = doc["face_enabled"];
            if (v != faceEnabled) { faceEnabled = v; changed = true; }
        }
        if (doc.containsKey("ble_enabled")) {
            bool v = doc["ble_enabled"];
            if (v != bleEnabled) { bleEnabled = v; changed = true; }
        }
        if (doc.containsKey("password")) {
            String v = doc["password"].as<String>();
            if (v != masterPassword) { masterPassword = v; changed = true; }
        }
        if (changed) {
            Serial.println("[FB_POLL] Config changed");
            updateLCD();
        }
    } else {
        http.end();
        client.stop();
    }
}

void postUnlockLog(const String &method, const String &mac) {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure client;  // local, không static
    client.setInsecure();
    HTTPClient http;

    String url = String(FIREBASE_HOST) + "/unlock_history.json";
    String body = "{\"method\":\"" + method + 
                  "\",\"timestamp\":" + String(millis()) + 
                  ",\"mac\":\"" + mac + "\"}";

    Serial.printf("[FB_POST] heap=%d largest=%d\n",
        ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    http.begin(client, url);
    http.setTimeout(8000);
    http.setConnectTimeout(8000);
    http.useHTTP10(true);
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(body);
    Serial.printf("[FB_POST] POST -> %d\n", code);
    if (code < 0) {
    Serial.printf("[FB_POST] Error: %s\n", 
        http.errorToString(code).c_str());
    }
    http.end();
    client.stop();
}
// Task này hoạt động thế nào:

// xQueueReceive(... pdMS_TO_TICKS(1000)) = "đợi job tối đa 1 giây"
// Nếu có job → xử lý ngay
// Nếu không (timeout 1s) → đi xuống check poll
// Mỗi 30s thì poll config 1 lần
// Lặp vô hạn

/* ================= DOOR ================= */
// void openDoorSequence(const String &method) {
//     //isSystemLocked = true;

//     lcd.clear();
//     lcd.setCursor(0,0); lcd.print("ACCESS GRANTED");
//     lcd.setCursor(0,1); lcd.print("By: " + method);

//     myServo.write(90);
//     delay(500);//chờ servo đến vị trí

//     doorState = DOOR_WAITING_FOR_OPEN;
//     unlockMethod = method;
//     // 2. Bật Hall watch
//     // doorState = DOOR_WAITING_FOR_OPEN;
//     // Serial.printf("[%lu][HALL] Bat theo doi cua\n", millis());
//     lcd.clear();
//     lcd.setCursor(0,0); lcd.print("Door unlocked");
//     lcd.setCursor(0,1); lcd.print("Please open...");

//     Serial.printf("[%lu][DOOR] door unlocked !!!!!!!\n", millis());

      //postUnlockLog(method);

// }
void openDoorSequence(const String &method) {
    Serial.printf("[%lu][DOOR] Buoc 1: vao ham\n", millis());
    
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("ACCESS GRANTED");
    lcd.setCursor(0,1); lcd.print("By: " + method);
    Serial.printf("[%lu][DOOR] Buoc 2: LCD ACCESS GRANTED xong\n", millis());

    myServo.write(90);
    Serial.printf("[%lu][DOOR] Buoc 3: servo write xong\n", millis());
    
    delay(500);
    Serial.printf("[%lu][DOOR] Buoc 4: delay 500ms xong\n", millis());

    doorState = DOOR_WAITING_FOR_OPEN;
    unlockMethod = method;
    
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Door unlocked");
    lcd.setCursor(0,1); lcd.print("Please open...");
    Serial.printf("[%lu][DOOR] Buoc 5: LCD Door unlocked xong\n", millis());

    Serial.printf("[%lu][DOOR] door unlocked !!!!!!!\n", millis());

    pendingLogMethod = method;
    pendingLogMac = String(unlockByMac);
    hasPendingLog = true;
    Serial.println("[DOOR] Da luu log, cho BLE ranh se gui");

    Serial.printf("[%lu][DOOR] Buoc 6: Firebase xong\n", millis());
}

void handleHall() {
    if (millis() - lastHallRead < 100) return;
    lastHallRead = millis();
    bool magnetClose = (digitalRead(HALL_PIN) == LOW);

    if (doorState == DOOR_WAITING_FOR_OPEN) {
        if (!magnetClose) {
            doorState = DOOR_PHYSICALLY_OPEN;
            if (pUnlockChar != NULL && countAuthenticatedClients() > 0) {
                String msg = "OPENED:" + unlockMethod;
                pUnlockChar->setValue(msg.c_str());
                pUnlockChar->notify();
            }
            lcd.clear();
            lcd.setCursor(0,0); lcd.print("Door is OPEN");
            lcd.setCursor(0,1); lcd.print("Welcome!");
            Serial.printf("[%lu][DOOR] door open welcome !!!!!!!\n", millis());
        }
    }
    else if (doorState == DOOR_PHYSICALLY_OPEN) {
        if (magnetClose) {
            Serial.printf("[%lu][DOOR] door closed!!!!!!!\n", millis());
            lcd.clear();
            lcd.setCursor(0,0); lcd.print("Locking...");
            myServo.write(0);
            delay(800);

            lockStartTime = millis();
            isSystemLocked = true;
            doorState = DOOR_IDLE;
            updateLCD();
        }
    }
}

/* ================= HELPERS ================= */
bool isBleAuthOk() {
    return countAuthenticatedClients() > 0;
}
void sendEventToPi(char code) { 
  Serial2.write(code); 
  Serial.printf( 
    "[UART->PI] Sent '%c' (0x%02X)\n", 
    code, 
    code 
    ); 
}

/* ================= SETUP ================= */
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("[%lu] === BOOT ===\n", millis());

    Serial2.begin(9600, SERIAL_8N1, FACE_RX, FACE_TX);

    lcd.init();
    lcd.backlight();

    myServo.attach(servoPin);
    myServo.write(0);
    pinMode(HALL_PIN, INPUT_PULLUP);

// 1. Ép sử dụng DNS của Google để tránh router nội bộ chặn
    IPAddress dns(8, 8, 8, 8);
    IPAddress dns2(8, 8, 4, 4);
    //WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, dns, dns2);
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    //WiFi.config(local_IP, gateway, subnet);
    WiFi.begin(ssid, password);
    Serial.print("[WIFI] Connecting");
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
        if (millis() - wifiStart > 10000) {
            Serial.println("\n[WIFI] Timeout");
            break;
        }
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] OK IP=%s DNS=%s\n",
            WiFi.localIP().toString().c_str(),
            WiFi.dnsIP().toString().c_str());
            //WiFi.setSleep(WIFI_PS_MIN_MODEM);    // ← thêm dòng này
            //WiFi.setSleep(false); // BẮT BUỘC PHẢI MỞ DÒNG NÀY ĐỂ BLE KHÔNG CƯỚP SÓNG
                // ← THÊM VÀO ĐÂY
            WiFiClientSecure testClient;
            testClient.setInsecure();
            Serial.printf("[TEST] Connect Firebase: %s\n",
                testClient.connect("smartlock-7f70d-default-rtdb.firebaseio.com", 443) 
                ? "OK" : "FAIL");
            testClient.stop();

    }
    // BLE Server
    BLEDevice::init("ESP32_Lock");
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);

    pChallengeChar = pService->createCharacteristic(
        CHAR_CHALLENGE_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    BLE2902 *pDesc = new BLE2902();
    pDesc->setCallbacks(new ChallengeDescriptorCallback());
    pChallengeChar->addDescriptor(pDesc);

    pResponseChar = pService->createCharacteristic(
        CHAR_RESPONSE_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    pResponseChar->setCallbacks(new ResponseCallback());

    // MỚI — UNLOCK characteristic
    // UNLOCK characteristic — kênh giao tiếp 2 chiều sau khi auth
    pUnlockChar = pService->createCharacteristic(
        CHAR_UNLOCK_UUID,
        BLECharacteristic::PROPERTY_READ |
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pUnlockChar->addDescriptor(new BLE2902());        // cần cho NOTIFY
    pUnlockChar->setValue("IDLE");                    // giá trị mặc định khi READ
    pUnlockChar->setCallbacks(new UnlockCallback());

    pService->start();

    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    BLEAdvertisementData advData;
    advData.setFlags(0x06);
    advData.setCompleteServices(BLEUUID(SERVICE_UUID));
    pAdvertising->setAdvertisementData(advData);

    BLEAdvertisementData scanResponseData;
    scanResponseData.setName("ESP32_Lock");
    pAdvertising->setScanResponseData(scanResponseData);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinInterval(0x280);
    pAdvertising->setMaxInterval(0x320);

    BLEDevice::startAdvertising();
    Serial.printf("[%lu] === BLE READY === Max %d clients\n", millis(), MAX_CLIENTS);

    updateLCD();

    // Tạo task chạy trên core 0 (loop chính ở core 1)
    // xTaskCreatePinnedToCore(
    //     firebaseTask,        // function
    //     "FirebaseTask",      // name
    //     8192,                // stack size (Firebase + TLS cần kha khá)
    //     NULL,                // param
    //     1,                   // priority thấp (1 = thấp nhất, dưới loop = 1)
    //     &firebaseTaskHandle, // handle
    //     1                    // core 0
    // );
    Serial.println("[FB] Task created");
    // Đo heap khi BLE đang advertising
    Serial.printf("BLE ON heap: %d\n", ESP.getFreeHeap());

    // Tắt advertising
    BLEDevice::stopAdvertising();
    delay(200);
    Serial.printf("BLE advertising OFF heap: %d\n", ESP.getFreeHeap());

    // Thử GET Firebase
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, String(FIREBASE_HOST) + "/config.json");
    int code = http.GET();
    Serial.printf("GET -> %d, heap: %d\n", code, ESP.getFreeHeap());
    http.end();
    client.stop();

    // Bật lại
    BLEDevice::startAdvertising();
    Serial.printf("BLE back ON heap: %d\n", ESP.getFreeHeap());
}

/* ================= LOOP ================= */
void loop() {
    static unsigned long lastHeap = 0;
    if (millis() - lastHeap > 5000) {
        lastHeap = millis();
        Serial.printf("[HEAP] free=%d largest=%d\n", 
            ESP.getFreeHeap(), 
            ESP.getMaxAllocHeap());  // ← cái này quan trọng nhất
    }
    static unsigned long lastPoll = 0;
    // Gửi log khi BLE rảnh
    if (hasPendingLog && countActiveClients() == 0 && doorState == DOOR_IDLE) {
        hasPendingLog = false;
        Serial.println("[FB] BLE ranh -> gui log");
        postUnlockLog(pendingLogMethod, pendingLogMac);
        lastPoll = millis(); // reset poll timer, tránh poll ngay sau post

    }
    if (countActiveClients() == 0 && 
        doorState == DOOR_IDLE &&
        !hasPendingLog && 
        millis() - lastPoll > 30000) {
        lastPoll = millis();
        pollFirebaseConfig();
    }
    /* Auth timeout kick */
    if (kickPending) {
        kickPending = false;
        if (isAuthenticating) {
            ClientInfo *client = findClient(authenticatingConnId);
            if (client != NULL && !client->authenticated) {
                Serial.printf("[%lu][KICK] [%s] timeout %ds\n",
                    millis(), client->macAddress, AUTH_TIMEOUT_SEC);
                pServer->disconnect(client->connId);
            }
        }
    }

    /* System lock 5s sau khi mở cửa */
    if (isSystemLocked) {
        if (millis() - lockStartTime >= 5000) {
            isSystemLocked = false;
            updateLCD();
        }
        while (Serial2.available()) Serial2.read();
        return;
    }
    // (c) MỚI: Đang trong chu kỳ mở cửa — chỉ chạy hall, bỏ qua input
    if (doorState != DOOR_IDLE) {
        handleHall();
        while (Serial2.available()) Serial2.read();
        return;
    }

    /* ============ LOGIC MỞ CỬA ============ */
    bool bleOk = isBleAuthOk();

    /* --- Phone gửi lệnh UNLOCK (BLE geofence từ phone) --- */
    // Xử lý ngay trong loop để đảm bảo không gọi từ BLE callback
    // if (unlockPending) {
    //     //unlockPending = false;

    //     if (!bleEnabled) {
    //         Serial.printf("[%lu][UNLOCK] BLE disabled, ignore\n", millis());
    //     }
    //     else if (faceEnabled) {
    //         // Case ON/ON — BLE chỉ là điều kiện AND với Face, KHÔNG tự mở
    //         Serial.printf("[%lu][UNLOCK] Face dang bat -> can them Face\n", millis());
    //         lcd.clear();
    //         lcd.setCursor(0,0); lcd.print("PHONE CLOSE");
    //         lcd.setCursor(0,1); lcd.print("WAIT FACE...");
    //         delay(1500);
    //         updateLCD();
    //     }
    //     else {
    //         // Case OFF/ON — Face tắt, BLE bật → unlock luôn
    //         openDoorSequence("Phone BLE");
    //     }
    // }
    if (unlockPending) {
    if (!bleEnabled) {
        unlockPending = false;        // ← tắt cờ, đã xử lý xong
        Serial.printf("[%lu][UNLOCK] BLE disabled, ignore\n", millis());
    }
    else if (faceEnabled) {
        // Case ON/ON — GIỮ cờ, đợi Face block
        // Chỉ in log + LCD 1 lần, không delay
        static bool waitFaceShown = false;
        if (!waitFaceShown) {
            Serial.printf("[%lu][UNLOCK] Face dang bat -> giu co, doi Face\n", millis());
            lcd.clear();
            lcd.setCursor(0,0); lcd.print("PHONE CLOSE");
            lcd.setCursor(0,1); lcd.print("WAIT FACE...");
            waitFaceShown = true;
        }
        // Khi cờ tắt (Face xử lý xong) → reset waitFaceShown ở chỗ khác
        if (!unlockPending) waitFaceShown = false;
    }
    else {
        // Case OFF/ON — Face tắt, BLE bật → mở luôn
        unlockPending = false;        // ← tắt cờ, đã xử lý
        Serial.printf("[%lu][LOOP] Sap goi openDoorSequence\n", millis());
        openDoorSequence("Phone BLE");
    }
}

    /* --- FACE ID --- */
    // if (faceEnabled && Serial2.available()) {
    //     char faceByte = Serial2.read();
    //     if (faceByte == '1') {
    //         if (bleEnabled) {
    //             // Case ON/ON — phải có BLE authed
    //             if (unlockPending) {
    //                 unlockPending = false;
    //                 openDoorSequence("Face+BLE");
    //             } else {
    //                 Serial.printf("[%lu][FACE] OK nhung khong co BLE -> reject\n", millis());
    //                 lcd.clear();
    //                 lcd.setCursor(0,0); lcd.print("FACE OK BUT");
    //                 lcd.setCursor(0,1); lcd.print("NO PHONE NEAR");
    //                 delay(1500);
    //                 updateLCD();
    //             }
    //         } else {
    //             // Case ON/OFF — chỉ cần Face
    //             openDoorSequence("Face");
    //         }
    //     }
    // }
        /* --- FACE ID --- */
    if (faceEnabled && Serial2.available()) {
        char faceByte = Serial2.read();
        Serial.printf("[%lu][FACE] Nhan byte: 0x%02X ('%c')\n", 
            millis(), faceByte, (faceByte >= 32 && faceByte < 127) ? faceByte : '?');
        
        if (faceByte == '1') {
            Serial.printf("[%lu][FACE] Byte = '1', bleEnabled=%s unlockPending=%s\n",
                millis(), bleEnabled ? "Y" : "N", unlockPending ? "Y" : "N");
            
            if (bleEnabled) {
                if (unlockPending) {
                    unlockPending = false;
                    sendEventToPi('3');      // <-- thêm ở đây

                    Serial.printf("[%lu][FACE] Pass! Co lenh unlock + Face OK -> MO CUA\n", millis());
                    openDoorSequence("Face+BLE");
                } else {
                    Serial.printf("[%lu][FACE] REJECT - Chua co lenh unlock tu phone\n", millis());
                    lcd.clear();
                    lcd.setCursor(0,0); lcd.print("FACE OK BUT");
                    lcd.setCursor(0,1); lcd.print("NO PHONE NEAR");
                    delay(1500);
                    updateLCD();
                }
            } else {
                Serial.printf("[%lu][FACE] BLE tat -> chi can Face -> MO CUA\n", millis());
                openDoorSequence("Face");
            }
        } else {
            Serial.printf("[%lu][FACE] Byte sai (khong phai '1') -> ignore\n", millis());
        }
    }

    /* --- KEYPAD (luôn độc lập) --- */
    if (keypadEnabled) {
        char key = keypad.getKey();
        if (key) {
            if (key == 'D') {
                if (inputPassword == masterPassword) {
                    sendEventToPi('4');
                    openDoorSequence("Keypad");
                } else {
                    sendEventToPi('2');
                    lcd.setCursor(0,1);
                    lcd.print("WRONG PASS!     ");
                    delay(1500);
                    updateLCD();
                }
                inputPassword = "";
                updateLCD();
            }
            else if (key == '*') {
                inputPassword = "";
                updateLCD();
            }
            else {
                inputPassword += key;
                updateLCD();
            }
        }
    }

    /* Monitor */
    static unsigned long lastMon = 0;
    if (millis() - lastMon > 30000) {
        lastMon = millis();
        Serial.printf("[%lu][MON] Heap=%d Authed=%d/%d WiFi=%s F:%d K:%d B:%d AuthLock=%s\n",
            millis(), ESP.getFreeHeap(),
            countAuthenticatedClients(), MAX_CLIENTS,
            WiFi.status() == WL_CONNECTED ? "OK" : "X",
            faceEnabled, keypadEnabled, bleEnabled,
            isAuthenticating ? "Y" : "N");
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active) {
                Serial.printf("  [slot %d] [%s] auth=%s\n",
                    i, clients[i].macAddress, clients[i].authenticated ? "Y" : "N");
            }
        }
    }
}
