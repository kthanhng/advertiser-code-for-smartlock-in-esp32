// code thêm aes challenge response 

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEAdvertising.h>
#include <BLE2902.h>
#include "mbedtls/aes.h"

#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHAR_CHALLENGE_UUID "12345678-1234-1234-1234-1234567890cd"
#define CHAR_RESPONSE_UUID  "12345678-1234-1234-1234-1234567890ef"

static const uint8_t AES_KEY[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
};

BLEServer *pServer = NULL;
BLECharacteristic *pChallengeChar = NULL;
BLECharacteristic *pResponseChar = NULL;

uint8_t currentChallenge[16];
bool authenticated = false;
unsigned long connectTime = 0;
uint16_t connectedClientId = 0;

void generateChallenge() {
    for (int i = 0; i < 16; i++) {
        currentChallenge[i] = esp_random() & 0xFF;
    }
}

void aesEncrypt(const uint8_t *input, uint8_t *output, const uint8_t *key) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, input, output);
    mbedtls_aes_free(&aes);
}

// Callback khi Android gửi response về
class ResponseCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        String value = pChar->getValue();
        if (value.length() != 16) {
            Serial.println("Response sai length -> Kick");
            pServer->disconnect(connectedClientId);
            return;
        }

        uint8_t expected[16];
        aesEncrypt(currentChallenge, expected, AES_KEY);

        if (memcmp(expected, value.c_str(), 16) == 0) {
            authenticated = true;
            connectTime = 0;  // Tắt timer kick
            Serial.println("XAC THUC THANH CONG!");
        } else {
            Serial.println("SAI KEY -> Kick ngay");
            pServer->disconnect(connectedClientId);
        }
    }
};

// === THAY ĐỔI 1 ===
// Callback khi Android bật notify (báo "tôi sẵn sàng nghe rồi")
// Lúc này mới gửi challenge
class ChallengeDescriptorCallback : public BLEDescriptorCallbacks {
    void onWrite(BLEDescriptor *pDescriptor) override {
        Serial.println("Android da san sang nghe -> Gui challenge NOW");
        pChallengeChar->notify();
    }
};

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) {
        connectedClientId = param->connect.conn_id;
        authenticated = false;
        connectTime = millis();

        // === THAY ĐỔI 2 ===
        // Tạo challenge và SET giá trị sẵn, NHƯNG CHƯA GỬI
        // Đợi Android bật notify xong mới gửi (xem ChallengeDescriptorCallback)
        generateChallenge();
        pChallengeChar->setValue(currentChallenge, 16);

        Serial.println("Client connected -> Challenge ready, doi Android bat notify...");

        delay(100);
        BLEDevice::startAdvertising();
    }

    void onDisconnect(BLEServer* pServer) {
        authenticated = false;
        connectTime = 0;
        Serial.println("Client disconnected");
        delay(500);
        BLEDevice::startAdvertising();
    }
};

void setup() {
    Serial.begin(115200);
    BLEDevice::init("ESP32_Lock");

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);

    // Challenge characteristic (ESP32 -> Android)
    pChallengeChar = pService->createCharacteristic(
        CHAR_CHALLENGE_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );

    // === THAY ĐỔI 3 ===
    // Gắn callback vào descriptor để biết khi nào Android bật notify
    BLE2902 *pDesc = new BLE2902();
    pDesc->setCallbacks(new ChallengeDescriptorCallback());
    pChallengeChar->addDescriptor(pDesc);

    // Response characteristic (Android -> ESP32)
    pResponseChar = pService->createCharacteristic(
        CHAR_RESPONSE_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    pResponseChar->setCallbacks(new ResponseCallback());

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
    pAdvertising->setMinInterval(0x20);
    pAdvertising->setMaxInterval(0x40);

    BLEDevice::startAdvertising();
    Serial.println("BLE Server + AES Auth ready");
}

void loop() {
    // Kick nếu connect hơn 2 giây mà chưa xác thực
    if (connectTime > 0 && !authenticated && (millis() - connectTime > 2000)) {
        Serial.println("TIMEOUT -> Kick client");
        pServer->disconnect(connectedClientId);
        connectTime = 0;
    }
}