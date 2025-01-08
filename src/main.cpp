#define SerialMon Serial

#include <M5Stack.h>
#include <Wire.h>
#include "SparkFun_SCD4x_Arduino_Library.h"

// インスタンス生成
SCD4x scd40;

// センサー読み取り周期 (ミリ秒)
static const unsigned long INTERVAL = 10000; // 10秒に変更
unsigned long lastUpdate = 0;

// SIM7080の設定
#define MODEM_RST 5
#define MODEM_PWRKEY 4
#define MODEM_POWER_ON 23
#define MODEM_TX 15
#define MODEM_RX 13
#define SerialAT Serial2

bool SORACOM_CONNECTED = false;

String sendATCommand(const char* command, unsigned long timeout = 2000) {
  SerialMon.print("Sending: ");
  SerialMon.println(command);
  SerialAT.println(command);
  unsigned long start = millis();
  String response = "";
  while (millis() - start < timeout) {
    if (SerialAT.available()) {
      response += SerialAT.readString();
    }
  }
  SerialMon.print("Response: ");
  SerialMon.println(response);
  return response;
}

bool waitForNetworkRegistration(unsigned long timeout = 60000) {
  unsigned long start = millis();
  while (millis() - start < timeout) {
    String response = sendATCommand("AT+CREG?");
    if (response.indexOf("+CREG: 0,1") != -1 || response.indexOf("+CREG: 0,5") != -1) {
      return true;
    }
    delay(1000);
  }
  return false;
}

void connectSoracom() {
  SerialMon.println("Connecting to SORACOM...");
  sendATCommand("AT+GSN");  // Request TA Serial Number Identification(IMEI)
  sendATCommand("AT+CFUN=0");
  sendATCommand("AT+CGDCONT=1,\"IP\",\"soracom.io\"");
  sendATCommand("AT+CFUN=1");
  sendATCommand("AT+CGNAPN");
  sendATCommand("AT+CMNB?");
  sendATCommand("AT+CPSI?"); // Inquiring UE System Information
  sendATCommand("AT+CNACT=0,1");
  String response = sendATCommand("AT+CNACT?");
  if (response.indexOf("ACTIVE") != -1) {
    SerialMon.println("IP: " + response);
    SORACOM_CONNECTED = true;
    SerialMon.println("Connected Successfully!");
  }
}

void initializeModem() {
  SerialMon.println("Initializing modem...");
  while (sendATCommand("AT").indexOf("OK") == -1) {
    SerialMon.println("Retrying AT command...");
    delay(1000);
  }
  sendATCommand("ATE0"); // Disable echo
  sendATCommand("AT+CGMM"); // Get model information
  sendATCommand("AT+CSQ"); // Signal quality
}

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

  // モデムの電源をオンにする
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(1000);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1000);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(3000);

  // モデムの初期化
  initializeModem();

  // SORACOMへの接続
  connectSoracom();

  // ネットワーク登録の待機
  SerialMon.println("Waiting for network registration...");
  while (!waitForNetworkRegistration()) {
    SerialMon.println("Retrying network registration...");
    delay(10000); // 10秒待機して再試行
  }
  SerialMon.println("Network registered successfully");
}

void loop() {
  unsigned long current = millis();
  if (current - lastUpdate >= INTERVAL) {
    lastUpdate = current;

    if (scd40.readMeasurement()) {
      float co2 = scd40.getCO2();
      float temp = scd40.getTemperature();
      float humidity = scd40.getHumidity();

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