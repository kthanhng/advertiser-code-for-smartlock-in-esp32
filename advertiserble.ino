#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEAdvertising.h>


#define SERVICE_UUID "12345678-1234-1234-1234-1234567890ab"


BLEServer *pServer = NULL;


void setup() {
  Serial.begin(115200);


  // Khởi tạo tên(Tên này sẽ được nhét vào gói phụ)
  BLEDevice::init("ESP32_Lock");


  // Xây GATT Server để Android connect không bị lỗi 133 / 19
  pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pService->start();


  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();


  // 👇 1. GÓI CHÍNH (Chỉ nhét UUID, dung lượng 21/31 bytes -> Rất an toàn)
  BLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setCompleteServices(BLEUUID(SERVICE_UUID));
  pAdvertising->setAdvertisementData(advData);


  // 👇 2. GÓI PHỤ (Nhét tên thiết bị vào đây)
  BLEAdvertisementData scanResponseData;
  scanResponseData.setName("ESP32_Lock");
  pAdvertising->setScanResponseData(scanResponseData);


  // Kích hoạt cho phép ESP32 ném gói phụ ra khi bị Android hỏi
  pAdvertising->setScanResponse(true);


  // Ép xung tốc độ phát (20ms - 40ms)
  pAdvertising->setMinInterval(0x20);
  pAdvertising->setMaxInterval(0x40);


  BLEDevice::startAdvertising();


  Serial.println("Đã fix lỗi 31-byte! Gói tin chuẩn đang được phát......");
}


void loop() {
  // Để trống
}