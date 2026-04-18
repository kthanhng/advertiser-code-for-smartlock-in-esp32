// code cho mutiple access còn đâu như code trên 
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEAdvertising.h>

#define SERVICE_UUID "12345678-1234-1234-1234-1234567890ab"

BLEServer *pServer = NULL;

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        Serial.println("Client connected");
        // Tiếp tục phát advertising để người khác vẫn thấy và connect được
        delay(100);
        BLEDevice::startAdvertising();
    }

    void onDisconnect(BLEServer* pServer) {
        Serial.println("Client disconnected -> Restart advertising");
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

    Serial.println("BLE Server ready - Multi-connect enabled");
}

void loop() {
    // Để trống
}
