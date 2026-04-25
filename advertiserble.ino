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
#define MAX_CLIENTS 3

static const uint8_t AES_KEY[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
};

BLEServer *pServer = NULL;
BLECharacteristic *pChallengeChar = NULL;
BLECharacteristic *pResponseChar = NULL;

bool isAuthenticating = false;
uint16_t authenticatingConnId = 0;

struct ClientInfo {
    bool active = false;
    uint16_t connId = 0;
    uint8_t challenge[16] = {0};
    bool authenticated = false;
    unsigned long connectTime = 0;
    char macAddress[18] = {0};
};

ClientInfo clients[MAX_CLIENTS];

ClientInfo* findClient(uint16_t connId) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].connId == connId) {
            return &clients[i];
        }
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
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) count++;
    }
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
    for (int i = 0; i < 16; i++) {
        buffer[i] = esp_random() & 0xFF;
    }
}

void aesEncrypt(const uint8_t *input, uint8_t *output, const uint8_t *key) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, input, output);
    mbedtls_aes_free(&aes);
}

void macToString(const uint8_t *addr, char *out) {
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
        addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

void unlockAndAdvertise() {
    Serial.printf("[%lu][UNLOCK] BEFORE: isAuth=%s authConnId=%d active=%d\n",
        millis(), isAuthenticating ? "Y" : "N", authenticatingConnId, countActiveClients());

    isAuthenticating = false;
    authenticatingConnId = 0;

    if (countActiveClients() < MAX_CLIENTS) {
        delay(100);
        BLEDevice::startAdvertising();
        Serial.printf("[%lu][UNLOCK] Mo khoa -> Bat advertise\n", millis());
    } else {
        Serial.printf("[%lu][UNLOCK] Mo khoa nhung het slot\n", millis());
    }
}

class ResponseCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        Serial.printf("[%lu][RESPONSE] onWrite isAuth=%s authConnId=%d\n",
            millis(), isAuthenticating ? "Y" : "N", authenticatingConnId);

        if (!isAuthenticating) {
            Serial.printf("[%lu][RESPONSE] SKIP - khong co client nao dang xac thuc\n", millis());
            return;
        }

        ClientInfo *client = findClient(authenticatingConnId);
        if (client == NULL) {
            Serial.printf("[%lu][RESPONSE] SKIP - khong tim thay client connId=%d\n",
                millis(), authenticatingConnId);
            unlockAndAdvertise();
            return;
        }

        String value = pChar->getValue();
        Serial.printf("[%lu][RESPONSE] [%s] Nhan response, length=%d\n",
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
            client->connectTime = 0;
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
            ClientInfo *client = findClient(authenticatingConnId);
            if (client != NULL) {
                unsigned long oldTime = client->connectTime;
                client->connectTime = millis();
                Serial.printf("[%lu][CHALLENGE] Reset timer [%s] old=%lu new=%lu\n",
                    millis(), client->macAddress, oldTime, client->connectTime);
            }
        }
        pChallengeChar->notify();
        Serial.printf("[%lu][CHALLENGE] Da gui challenge notify\n", millis());
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
            Serial.printf("[%lu][CONNECT] [%s] REJECT - dang co client khac xac thuc\n", millis(), mac);
            pServer->disconnect(connId);
            return;
        }

        ClientInfo *client = findFreeSlot();
        if (client == NULL) {
            Serial.printf("[%lu][CONNECT] [%s] REJECT - het slot (%d/%d)\n",
                millis(), mac, MAX_CLIENTS, MAX_CLIENTS);
            pServer->disconnect(connId);
            return;
        }

        client->active = true;
        client->connId = connId;
        client->authenticated = false;
        client->connectTime = millis();
        strncpy(client->macAddress, mac, 17);
        generateChallenge(client->challenge);

        isAuthenticating = true;
        authenticatingConnId = connId;

        pChallengeChar->setValue(client->challenge, 16);

        Serial.printf("[%lu][CONNECT] [%s] OK slot %d/%d -> KHOA, doi auth\n",
            millis(), mac, countActiveClients(), MAX_CLIENTS);
    }

    void onDisconnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) {
        uint16_t connId = param->disconnect.conn_id;

        Serial.printf("[%lu][DISCONNECT] connId=%d isAuth=%s authConnId=%d\n",
            millis(), connId, isAuthenticating ? "Y" : "N", authenticatingConnId);

        ClientInfo *client = findClient(connId);

        if (client != NULL) {
            Serial.printf("[%lu][DISCONNECT] [%s] wasAuth=%s active=%s\n",
                millis(), client->macAddress,
                client->authenticated ? "Y" : "N",
                client->active ? "Y" : "N");
            client->active = false;
            client->authenticated = false;
            client->connectTime = 0;
        } else {
            Serial.printf("[%lu][DISCONNECT] Unknown client connId=%d\n", millis(), connId);
        }

        if (isAuthenticating && connId == authenticatingConnId) {
            Serial.printf("[%lu][DISCONNECT] Client dang xac thuc bi disconnect -> Mo khoa\n", millis());
            unlockAndAdvertise();
        } else {
            if (countActiveClients() < MAX_CLIENTS) {
                delay(200);
                BLEDevice::startAdvertising();
                Serial.printf("[%lu][DISCONNECT] Bat lai advertise (con slot)\n", millis());
            }
        }
    }
};

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.printf("[%lu] === BOOT === Free heap: %d bytes\n", millis(), ESP.getFreeHeap());

    BLEDevice::init("ESP32_Lock");

    int bondCount = esp_ble_get_bond_device_num();
    Serial.printf("[%lu] Bond devices: %d\n", millis(), bondCount);

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
    Serial.printf("[%lu] === READY === BLE Server TUAN TU (max %d clients)\n", millis(), MAX_CLIENTS);
}

void loop() {
    if (isAuthenticating) {
        ClientInfo *client = findClient(authenticatingConnId);
        if (client != NULL && !client->authenticated &&
            client->connectTime > 0 && (millis() - client->connectTime > 50000)) {

            Serial.printf("[%lu][TIMEOUT] connId=%d [%s] age=%lums -> Kick\n",
                millis(), client->connId, client->macAddress, millis() - client->connectTime);

            pServer->disconnect(client->connId);
        }
    }

    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 30000) {
        lastCheck = millis();
        Serial.printf("[%lu][MONITOR] Heap=%d Bond=%d Active=%d/%d AuthLock=%s authConnId=%d\n",
            millis(),
            ESP.getFreeHeap(),
            esp_ble_get_bond_device_num(),
            countActiveClients(),
            MAX_CLIENTS,
            isAuthenticating ? "Y" : "N",
            authenticatingConnId
        );
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active) {
                Serial.printf("  [slot %d] [%s] connId=%d auth=%s age=%lums\n",
                    i,
                    clients[i].macAddress,
                    clients[i].connId,
                    clients[i].authenticated ? "Y" : "N",
                    clients[i].connectTime > 0 ? millis() - clients[i].connectTime : 0
                );
            }
        }
    }
}