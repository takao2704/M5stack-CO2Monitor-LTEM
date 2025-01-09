#define SerialMon Serial

#include <M5Stack.h>
#include <Wire.h>
#include "SparkFun_SCD4x_Arduino_Library.h"

#define TINY_GSM_MODEM_SIM7080
#include <TinyGsmClient.h>

#include <stdlib.h>

// インスタンス生成
SCD4x scd40;

// センサー読み取り周期 (ミリ秒)
static const unsigned long INTERVAL = 10000; // 10秒に変更
unsigned long lastUpdate = 0;

// SIM7080の設定
#define MODEM_TX 15
#define MODEM_RX 13
#define SerialAT Serial2
#define ENDPOINT "uni.soracom.io"

const char udpServer[] = "uni.soracom.io"; // サーバーのIPアドレス
const uint16_t udpPort = 23080;         // サーバーのポート番号

TinyGsm modem(SerialAT);

void setup() {
  // --- M5Stackの初期化 ---
  M5.begin();

  // --- I2C初期化 (PortAのSDA=21, SCL=22) ---
  Wire.begin(21, 22);

  // --- SCD40 (SCD4x) の初期化 ---
  if (!scd40.begin(Wire)) {
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.println("SCD40 init failed! Check wiring/address = 0x62");
    SerialMon.println("SCD40 init failed! Check wiring/address = 0x62");
  } else {
    scd40.startPeriodicMeasurement();
    M5.Lcd.clear(BLACK);
    M5.Lcd.println("SCD40 (SCD4x) Setup Complete.");
    SerialMon.println("SCD40 (SCD4x) Setup Complete.");
  }

  // --- SIM7080の初期化 ---
  SerialMon.begin(115200);
  delay(10);
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);

  // モデムの初期化
  SerialMon.println("Initializing modem...");
  modem.init();

  // モデム情報の取得
  String modemInfo = modem.getModemInfo();
  SerialMon.print("Modem Info: ");
  SerialMon.println(modemInfo);
  SerialMon.print("Modem Name: ");
  SerialMon.println(modem.getModemName());
  SerialMon.print("Modem Manufacturer: ");
  SerialMon.println(modem.getModemManufacturer());
  SerialMon.print("Modem Model: ");
  SerialMon.println(modem.getModemModel());
  SerialMon.print("Modem Revision: ");
  SerialMon.println(modem.getModemRevision());
  SerialMon.print("Modem IMEI: ");
  SerialMon.println(modem.getModemSerialNumber());
  SerialMon.print("Modem ICCID: ");
  SerialMon.println(modem.getSimCCID());
  SerialMon.print("SIM Status: ");
  SerialMon.println(modem.getSimStatus());

  // ネットワーク接続の待機
  SerialMon.println("Waiting for network registration...");
  int retryCount = 0;
  int maxRetries = 5;
  int baseDelay = 1000; // 1秒
  while (!modem.waitForNetwork() && retryCount < maxRetries) {
    SerialMon.println("Retrying network registration...");
    retryCount++;
    int jitter = rand() % 1000; // 0から999ミリ秒のランダムな遅延
    int delayTime = baseDelay * (1 << retryCount) + jitter; // exponential backoff with jitter
    SerialMon.printf("Retry %d/%d, waiting for %d ms\n", retryCount, maxRetries, delayTime);
    delay(delayTime);
  }

  // ネットワーク接続成功
  SerialMon.println("Network registered successfully");
  SerialMon.print("Network Operator: ");
  SerialMon.println(modem.getOperator());
  //信号品質の取得
  int8_t csq = modem.getSignalQuality();
  SerialMon.print("Signal quality: ");
  SerialMon.println(csq);
  
  //SORACOMのAPNに接続
  if (!modem.gprsConnect("soracom.io", "sora", "sora")) {
    SerialMon.println("GPRS connection failed");
    delay(10000);
    return;
  }
  SerialMon.println("GPRS connected");

  //IPアドレスの取得
  IPAddress localIP = modem.localIP();
  SerialMon.print("Local IP: ");
  SerialMon.println(localIP);

  // UDPソケットを開く（ATコマンド使用）
  SerialMon.println("Opening UDP socket...");
  // ATコマンド送信
  modem.sendAT("+CAOPEN=0,0,\"UDP\",\"" + String(udpServer) + "\"," + String(udpPort));

  // レスポンスを取得して詳細を表示
  int response = modem.waitResponse(10000L); // 最大10秒待機

  if (response == 1) {
    SerialMon.println("UDP socket opened successfully!");
  } else {
    SerialMon.println("Failed to open UDP socket. AT Response:");
    while (SerialAT.available()) {
      char c = SerialAT.read();
      SerialMon.print(c); // レスポンスを1文字ずつ出力
    }
  SerialMon.println(); // 改行
  }
}


void loop() {
  unsigned long current = millis();
  if (current - lastUpdate >= INTERVAL) {
    lastUpdate = current;

    if (scd40.readMeasurement()) {
      float co2 = scd40.getCO2();
      float temp = scd40.getTemperature();
      float humidity = scd40.getHumidity();

      // データをバイナリ形式でパッキング
      uint8_t payload[12];
      memcpy(payload, &co2, sizeof(co2));
      memcpy(payload + 4, &temp, sizeof(temp));
      memcpy(payload + 8, &humidity, sizeof(humidity));
      // この場合はバイナリパーサーでパースする必要がある
      // co2::float:32:little-endian Temp::float:32:little-endian Humi::float:32:little-endian

      SerialMon.println("Sending data...");

      // データ送信ATコマンド
      modem.sendAT("+CASEND=0," + String(sizeof(payload)));
      if (modem.waitResponse(">") != 1) {
        SerialMon.println("Failed to initiate data send");
        return;
      }

      SerialAT.write(payload, sizeof(payload));
      if (modem.waitResponse() != 1) {
        SerialMon.println("Failed to send data");
        return;
      }

      SerialMon.println("Data sent successfully!");

      M5.Lcd.clear(BLACK);
      M5.Lcd.setCursor(0, 0);
      M5.Lcd.setTextFont(4);
      M5.Lcd.println("UNIT CO2 (SCD40)");
      M5.Lcd.printf("CO2   : %.0f ppm\n", co2);
      M5.Lcd.printf("Temp  : %.2f C\n", temp);
      M5.Lcd.printf("Hum   : %.2f %%\n", humidity);

      SerialMon.printf("CO2: %.0f ppm, Temp: %.2f C, Hum: %.2f %%\n", co2, temp, humidity);
    } else {
      M5.Lcd.clear(BLACK);
      M5.Lcd.println("SCD40 readMeasurement() failed");
      SerialMon.println("SCD40 readMeasurement() failed");
    }
  }

  M5.update();
}