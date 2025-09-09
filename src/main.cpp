#define SerialMon Serial

#include <M5Stack.h>
#include <Wire.h>
#include "SparkFun_SCD4x_Arduino_Library.h"
#include "SparkFun_FS3000_Arduino_Library.h"

#define TINY_GSM_MODEM_SIM7080
#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>

#include <stdlib.h>

// インスタンス生成
SCD4x scd40;
FS3000 fs3000;

// センサー読み取り周期 (ミリ秒)
static unsigned long INTERVAL = 10000; // デフォルト値は10秒
unsigned long lastUpdate = 0;

// 通信状態監視用変数
int consecutiveFailures = 0;
const int MAX_CONSECUTIVE_FAILURES = 3;
unsigned long lastSuccessfulSend = 0;
const unsigned long CONNECTION_TIMEOUT = 300000; // 5分間通信成功がない場合はリセット

// 回線情報
String subscriberImsi = "Unknown";
String subscriberName = "Unknown";

// SIM7080の設定
#define MODEM_TX 15
#define MODEM_RX 13
#define SerialAT Serial2
#define ENDPOINT "uni.soracom.io"

const char udpServer[] = "uni.soracom.io"; // サーバーのIPアドレス
const uint16_t udpPort = 23080;         // サーバーのポート番号

TinyGsm modem(SerialAT);

// MQTT設定状態
bool mqttEnabled = false;
String mqttTopic = "";
int mqttQos = 0; // 0 or 1
bool mqttConnected = false;
bool mqttConfigValid = false;

// 関数プロトタイプ宣言
bool checkModemStatus();
void hardResetModem();
bool openUdpSocket();
void sendData(uint8_t* payload, size_t payloadSize);
bool sendDataWithStatus(uint8_t* payload, size_t payloadSize);
void resetModem();
void readAndSendData();
void scanI2CDevices();

// MQTT関連プロトタイプ
bool mqttConfigure();
bool mqttConnect();
bool mqttPublish(const String& topic, const String& json, int qos);
void mqttDisconnect();
bool isMqttOnline();
bool isValidMqttTopic(const String& topic);
bool ensurePdp0Active();

// Ensure PDP context #0 is active and has an IP address
bool ensurePdp0Active() {
  // Check current PDP context status
  String pdp = "";
  modem.sendAT("+CNACT?");
  modem.waitResponse(5000L, pdp);
  SerialMon.println("ensurePdp0Active(): current +CNACT?");
  SerialMon.println(pdp);

  auto hasIp = []() -> bool {
    IPAddress ip = modem.localIP();
    return !(ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0);
  };

  // Already active?
  if (pdp.indexOf("+CNACT: 0,1") != -1 && hasIp()) {
    return true;
  }

  const int maxActTries = 3;
  const int baseDelay = 1000;

  for (int attempt = 0; attempt < maxActTries; ++attempt) {
    SerialMon.println("ensurePdp0Active(): activating PDP#0 with AT+CNACT=0,1 ...");
    modem.sendAT("+CNACT=0,1");
    int r = modem.waitResponse(20000L);
    if (r != 1) {
      SerialMon.println("ensurePdp0Active(): AT+CNACT=0,1 returned error/timeout");
    }

    // Re-check status
    pdp = "";
    modem.sendAT("+CNACT?");
    modem.waitResponse(5000L, pdp);
    SerialMon.println("ensurePdp0Active(): +CNACT? after activation attempt:");
    SerialMon.println(pdp);

    if (pdp.indexOf("+CNACT: 0,1") != -1 && hasIp()) {
      SerialMon.println("ensurePdp0Active(): PDP#0 active with IP");
      return true;
    }

    // As a stronger recovery, after the first failed attempt, try GPRS reconnect
    if (attempt >= 1) {
      SerialMon.println("ensurePdp0Active(): trying GPRS reconnect (disconnect -> connect)...");
      modem.gprsDisconnect();
      delay(1000);
      if (!modem.gprsConnect("soracom.io", "sora", "sora")) {
        SerialMon.println("ensurePdp0Active(): gprsConnect failed");
      } else {
        SerialMon.print("ensurePdp0Active(): Local IP after gprsConnect: ");
        SerialMon.println(modem.localIP());
      }
    }

    int jitter = rand() % 1000;
    int delayTime = baseDelay * (1 << attempt) + jitter;
    SerialMon.printf("ensurePdp0Active(): retry %d/%d in %d ms\n", attempt + 1, maxActTries, delayTime);
    delay(delayTime);
  }

  SerialMon.println("ensurePdp0Active(): failed to activate PDP#0");
  return false;
}
// センサーデータの読み取り、送信、画面更新を行う関数
void readAndSendData() {
  unsigned long current = millis();

  // SCD40データの取得
  bool scd40Success = false;
  float co2 = 0, temp = 0, humidity = 0;
  
  if (scd40.readMeasurement()) {
    co2 = scd40.getCO2();
    temp = scd40.getTemperature();
    humidity = scd40.getHumidity();
    scd40Success = true;
  }

  // FS3000データの取得
  bool fs3000Success = false;
  float windSpeed = 0;
  
  // 公式ライブラリの方式を使用
  windSpeed = fs3000.readMetersPerSecond();
  if (windSpeed >= 0) {
    fs3000Success = true;
    SerialMon.printf("FS3000 Raw: %d, Velocity: %.2f m/s\n", fs3000.readRaw(), windSpeed);
  } else {
    SerialMon.println("FS3000 readMetersPerSecond() failed");
  }

  // データをバイナリ形式でパッキング（16バイトに拡張）
  uint8_t payload[16];
  memcpy(payload, &co2, sizeof(co2));
  memcpy(payload + 4, &temp, sizeof(temp));
  memcpy(payload + 8, &humidity, sizeof(humidity));
  memcpy(payload + 12, &windSpeed, sizeof(windSpeed));
  // co2::float:32:little-endian Temp::float:32:little-endian Humi::float:32:little-endian Wind::float:32:little-endian

  bool sendSuccess = false;
  SerialMon.println("Preparing to send data...");

  if (mqttEnabled) {
    if (mqttConfigValid) {
      // JSONペイロードを生成
      String json = String("{\"co2\":") + String(co2, 1)
                  + ",\"temp\":" + String(temp, 1)
                  + ",\"humi\":" + String(humidity, 1)
                  + ",\"wind\":" + String(windSpeed, 2) + "}";
      SerialMon.print("MQTT JSON: ");
      SerialMon.println(json);

      // 接続確認し、未接続なら接続
      if (!isMqttOnline()) {
        SerialMon.println("MQTT not online. Attempting connect...");
        mqttConnect();
      }
      sendSuccess = mqttPublish(mqttTopic, json, mqttQos);
    } else {
      SerialMon.println("MQTT config invalid (topic/qos). Skipping send and waiting for metadata update.");
      // 送信失敗としてカウントしない（仕様）
      sendSuccess = false;
    }
  } else {
    // UDP送信
    SerialMon.println("Sending data via UDP...");
    sendSuccess = sendDataWithStatus(payload, sizeof(payload));
  }
  
  if (sendSuccess) {
    // 送信成功
    consecutiveFailures = 0; // 失敗カウンターをリセット
    lastSuccessfulSend = current; // 最後の成功送信時間を更新
  } else {
    // 送信失敗（ただしMQTT設定不正時はカウントしない）
    if (!(mqttEnabled && !mqttConfigValid)) {
      consecutiveFailures++;
      SerialMon.printf("Consecutive failures: %d/%d\n", consecutiveFailures, MAX_CONSECUTIVE_FAILURES);
      
      // 連続失敗回数が閾値を超えた場合、モデムをリセット
      if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
        SerialMon.println("Too many consecutive failures, resetting modem...");
        resetModem();
      }
    }
  }

  // LCD表示の更新
  M5.Lcd.clear(BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.setTextFont(4);
  M5.Lcd.println("CO2 + Wind Monitor");
  
  if (scd40Success) {
    M5.Lcd.printf("CO2   : %.0f ppm\n", co2);
    M5.Lcd.printf("Temp  : %.2f C\n", temp);
    M5.Lcd.printf("Hum   : %.2f %%\n", humidity);
  } else {
    M5.Lcd.println("SCD40: Error");
  }
  
  if (fs3000Success) {
    M5.Lcd.printf("Wind  : %.2f m/s\n", windSpeed);
  } else {
    M5.Lcd.println("FS3000: Error");
  }
  
  // 通信状態を表示
  M5.Lcd.setTextFont(2);
  if (mqttEnabled) {
    if (mqttConfigValid) {
      M5.Lcd.printf("Mode   : MQTT qos=%d\n", mqttQos);
    } else {
      M5.Lcd.println("Mode   : MQTT CONFIG ERR");
    }
  } else {
    M5.Lcd.println("Mode   : UDP");
  }
  M5.Lcd.printf("Network: %s\n", sendSuccess ? "OK" : "Error");
  M5.Lcd.printf("Fails: %d/%d\n", consecutiveFailures, MAX_CONSECUTIVE_FAILURES);
  M5.Lcd.printf("Interval: %lu sec\n", INTERVAL / 1000); // 送信インターバルを秒単位で表示
  
  // IMSIは長いので後半6桁だけ表示
  String shortImsi = subscriberImsi;
  if (shortImsi.length() > 6) {
    shortImsi = "..." + shortImsi.substring(shortImsi.length() - 6);
  }
  M5.Lcd.printf("IMSI: %s\n", shortImsi.c_str()); // 回線のIMSI（短縮表示）
  
  // 回線名も長い場合は省略
  String shortName = subscriberName;
  if (shortName.length() > 10) {
    shortName = shortName.substring(0, 10) + "...";
  }
  M5.Lcd.printf("Name: %s\n", shortName.c_str()); // 回線の名前（短縮表示）

  // シリアル出力の更新
  if (scd40Success && fs3000Success) {
    SerialMon.printf("CO2: %.0f ppm, Temp: %.2f C, Hum: %.2f %%, Wind: %.2f m/s\n",
                    co2, temp, humidity, windSpeed);
  } else if (scd40Success) {
    SerialMon.printf("CO2: %.0f ppm, Temp: %.2f C, Hum: %.2f %%, Wind: Error\n",
                    co2, temp, humidity);
  } else if (fs3000Success) {
    SerialMon.printf("CO2: Error, Wind: %.2f m/s\n", windSpeed);
  } else {
    SerialMon.println("Both sensors failed to read data");
  }
  
  // 最後の更新時間を記録
  lastUpdate = current;
}

// SORACOMメタデータからインターバル設定を取得する関数
void fetchAndUpdateInterval() {
  SerialMon.println("Fetching interval/MQTT settings from SORACOM metadata...");
  
  // HTTPクライアントの初期化
  TinyGsmClient client(modem);
  HttpClient http(client, "metadata.soracom.io", 80);
  
  // HTTPリクエストの送信
  SerialMon.println("Making HTTP GET request to metadata.soracom.io/v1/userdata");
  int err = http.get("/v1/userdata");
  if (err != 0) {
    SerialMon.printf("HTTP GET failed (error code: %d)\n", err);
    return; // 失敗した場合は現在の設定を維持
  }
  
  // レスポンスコードの確認
  int status = http.responseStatusCode();
  if (status != 200) {
    SerialMon.printf("HTTP response error: %d\n", status);
    return; // 失敗した場合は現在の設定を維持
  }
  
  // レスポンスデータの取得
  String response = http.responseBody();
  SerialMon.println("Response: " + response);
  
  // JSONデータのパース（MQTTキー追加に備えサイズ拡張）
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    SerialMon.print("JSON parsing failed: ");
    SerialMon.println(error.c_str());
    return; // 失敗した場合は現在の設定を維持
  }
  
  // interval_s値の取得
  if (doc.containsKey("interval_s")) {
    unsigned long newInterval = doc["interval_s"].as<unsigned long>() * 1000; // 秒からミリ秒に変換
    if (newInterval != INTERVAL) {
      INTERVAL = newInterval;
      SerialMon.printf("Interval updated to %lu ms\n", INTERVAL);
    } else {
      SerialMon.println("Interval unchanged");
    }
  } else {
    SerialMon.println("interval_s not found in metadata");
  }

  // MQTT設定の取得と検証
  bool prevMqttEnabled = mqttEnabled;
  bool newMqttEnabled = false;
  String newTopic = "";
  int newQos = 0;
  bool newConfigValid = false;

  if (doc.containsKey("mqtt")) {
    newMqttEnabled = doc["mqtt"].as<bool>();
  }

  if (newMqttEnabled) {
    if (doc.containsKey("topic")) {
      newTopic = doc["topic"].as<String>();
    }
    if (doc.containsKey("qos")) {
      newQos = doc["qos"].as<int>();
    }

    // バリデーション: topic非空・可視ASCII（長さ1-256程度）/ qosは0または1
    auto isPrintableAscii = [](char c) {
      return c >= 32 && c <= 126;
    };
    bool topicOk = newTopic.length() > 0 && newTopic.length() <= 256;
    if (topicOk) {
      for (size_t i = 0; i < newTopic.length(); ++i) {
        if (!isPrintableAscii(newTopic[i])) { topicOk = false; break; }
      }
    }
    bool qosOk = (newQos == 0 || newQos == 1);

    newConfigValid = topicOk && qosOk;

    if (!newConfigValid) {
      SerialMon.println("MQTT config invalid in metadata (topic/qos). MQTT send will be disabled until corrected.");
    }
  }

  // 切替時の処理（MQTT→UDP）
  if (prevMqttEnabled && !newMqttEnabled) {
    SerialMon.println("MQTT disabled by metadata. Disconnecting MQTT session immediately.");
    mqttDisconnect();
  }

  mqttEnabled = newMqttEnabled;
  mqttTopic = newTopic;
  mqttQos = newQos;
  mqttConfigValid = newMqttEnabled ? newConfigValid : false;

  SerialMon.printf("MQTT enabled: %s, topic: %s, qos: %d, valid: %s\n",
                   mqttEnabled ? "true" : "false",
                   mqttTopic.c_str(),
                   mqttQos,
                   mqttConfigValid ? "true" : "false");
}

// 回線情報を取得する関数
void fetchSubscriberInfo() {
  SerialMon.println("Fetching subscriber information...");
  
  // HTTPクライアントの初期化
  TinyGsmClient client(modem);
  
  // IMSIの取得
  SerialMon.println("Fetching IMSI...");
  HttpClient httpImsi(client, "metadata.soracom.io", 80);
  int errImsi = httpImsi.get("/v1/subscriber.imsi");
  if (errImsi == 0) {
    int statusImsi = httpImsi.responseStatusCode();
    if (statusImsi == 200) {
      String imsi = httpImsi.responseBody();
      imsi.trim(); // 余分な空白や改行を削除
      if (imsi.length() > 0) {
        subscriberImsi = imsi;
        SerialMon.println("IMSI: " + subscriberImsi);
      }
    } else {
      SerialMon.printf("IMSI HTTP response error: %d\n", statusImsi);
    }
  } else {
    SerialMon.printf("IMSI HTTP GET failed (error code: %d)\n", errImsi);
  }
  
  // 回線名の取得
  SerialMon.println("Fetching subscriber name...");
  HttpClient httpName(client, "metadata.soracom.io", 80);
  int errName = httpName.get("/v1/subscriber.tags.name");
  if (errName == 0) {
    int statusName = httpName.responseStatusCode();
    if (statusName == 200) {
      String name = httpName.responseBody();
      name.trim(); // 余分な空白や改行を削除
      if (name.length() > 0) {
        subscriberName = name;
        SerialMon.println("Subscriber name: " + subscriberName);
      }
    } else {
      SerialMon.printf("Subscriber name HTTP response error: %d\n", statusName);
    }
  } else {
    SerialMon.printf("Subscriber name HTTP GET failed (error code: %d)\n", errName);
  }
}

void setup() {
  // --- M5Stackの初期化 ---
  M5.begin();
  
  // === デバッグ情報の出力（フラッシュサイズ問題の診断用） ===
  SerialMon.println("=== FLASH DEBUG INFO ===");
  SerialMon.printf("Flash chip size: %d bytes (%d KB)\n", ESP.getFlashChipSize(), ESP.getFlashChipSize() / 1024);
  SerialMon.printf("Flash chip speed: %d Hz\n", ESP.getFlashChipSpeed());
  SerialMon.printf("Flash chip mode: %d\n", ESP.getFlashChipMode());
  SerialMon.printf("Sketch size: %d bytes\n", ESP.getSketchSize());
  SerialMon.printf("Free sketch space: %d bytes\n", ESP.getFreeSketchSpace());
  SerialMon.printf("Chip ID: %08X\n", (uint32_t)(ESP.getEfuseMac() >> 32));
  SerialMon.println("========================");

  // --- I2C初期化 (PortAのSDA=21, SCL=22) ---
  Wire.begin(21, 22);

  // --- I2Cデバイススキャン ---
  scanI2CDevices();

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

  // --- FS3000 空気速度センサーの初期化 ---
  if (!fs3000.begin(Wire)) {
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.println("FS3000 init failed! Check wiring/address = 0x28");
    SerialMon.println("FS3000 init failed! Check wiring/address = 0x28");
  } else {
    // FS3000-1005の範囲設定（0-7.23 m/s）
    fs3000.setRange(AIRFLOW_RANGE_7_MPS);
    M5.Lcd.clear(BLACK);
    M5.Lcd.println("FS3000 Air Velocity Sensor Setup Complete.");
    SerialMon.println("FS3000 Air Velocity Sensor Setup Complete.");
    SerialMon.println("FS3000 range set to 0-7.23 m/s (FS3000-1005)");
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
  SerialMon.println(modem.getIMEI());
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

  if (retryCount == maxRetries) {
    SerialMon.println("Failed to register to network after maximum retries");
    return;
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

  // メタデータからインターバル設定と回線情報を取得
  fetchAndUpdateInterval();
  fetchSubscriberInfo();

  if (mqttEnabled && mqttConfigValid) {
    SerialMon.println("MQTT mode enabled by metadata. Configuring MQTT...");
    if (!mqttConfigure()) {
      SerialMon.println("MQTT configure failed. Will attempt again before publish.");
    }
    if (!mqttConnect()) {
      SerialMon.println("MQTT connect failed. Will retry automatically before publish.");
    }
  } else {
    // UDPソケットの初期化（リトライ処理付き）
    int socketRetries = 3;
    int socketBaseDelay = 1000;
    bool socketOpened = false;
    
    for (int attempt = 0; attempt < socketRetries; attempt++) {
      // UDPソケットをクローズ
      SerialMon.println("Closing any existing UDP socket...");
      modem.sendAT("+CACLOSE=0");
      modem.waitResponse(10000L);
      
      // モデムの状態確認
      String cgattStatus = "";
      modem.sendAT("+CGATT?");
      if (modem.waitResponse(5000L, cgattStatus) == 1) {
        SerialMon.print("Network attachment status: ");
        SerialMon.println(cgattStatus);
        
        if (cgattStatus.indexOf("+CGATT: 1") == -1) {
          SerialMon.println("Modem not attached to network, reconnecting...");
          modem.gprsConnect("soracom.io", "sora", "sora");
          delay(2000);
        }
      }

      // UDPソケットを開く（ATコマンド使用）
      SerialMon.println("Opening UDP socket...");
      // ATコマンド送信
      modem.sendAT("+CAOPEN=0,0,\"UDP\",\"" + String(udpServer) + "\"," + String(udpPort));

      // レスポンスを取得して詳細を表示
      String atResponse = "";
      int response = modem.waitResponse(15000L, atResponse); // タイムアウトを15秒に延長

      if (response == 1) {
        SerialMon.println("UDP socket opened successfully!");
        socketOpened = true;
        break; // 成功したのでループを抜ける
      } else {
        SerialMon.println("Failed to open UDP socket. AT Response:");
        SerialMon.println(atResponse);
        
        // バッファをクリア
        while (SerialAT.available()) {
          SerialAT.read();
        }
        
        if (attempt < socketRetries - 1) {
          // 指数バックオフ + ジッター戦略
          int jitter = rand() % 1000;
          int delayTime = socketBaseDelay * (1 << attempt) + jitter;
          SerialMon.printf("Retry %d/%d, waiting for %d ms\n", attempt + 1, socketRetries, delayTime);
          delay(delayTime);
        }
      }
    }
    
    if (!socketOpened) {
      SerialMon.println("Failed to open UDP socket after maximum retries. Restarting...");
      delay(1000);
      ESP.restart(); // 再起動
    }
  }
  
  // setup完了後、すぐに初回のデータ送信と画面更新を行う
  SerialMon.println("Setup completed, performing initial data reading and sending...");
  readAndSendData();
}

// モデムの状態を詳細に確認する関数
bool checkModemStatus() {
  SerialMon.println("Checking modem status in detail...");
  
  // ネットワーク接続状態の確認
  String cgattStatus = "";
  modem.sendAT("+CGATT?");
  if (modem.waitResponse(5000L, cgattStatus) != 1) {
    SerialMon.println("Failed to get network attachment status");
    return false;
  }
  SerialMon.print("Network attachment status: ");
  SerialMon.println(cgattStatus);
  
  // シグナル強度の確認
  int8_t csq = modem.getSignalQuality();
  SerialMon.print("Signal quality: ");
  SerialMon.println(csq);
  if (csq < 5 || csq > 31) {
    SerialMon.println("Poor signal quality detected");
  }
  
  // IPアドレスの確認
  IPAddress ip = modem.localIP();
  SerialMon.print("Local IP: ");
  SerialMon.println(ip);
  if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) {
    SerialMon.println("Invalid IP address");
    return false;
  }
  
  // PDP状態の確認
  String pdpStatus = "";
  modem.sendAT("+CNACT?");
  if (modem.waitResponse(5000L, pdpStatus) == 1) {
    SerialMon.print("PDP context status: ");
    SerialMon.println(pdpStatus);
    bool pdp0Active = pdpStatus.indexOf("+CNACT: 0,1") != -1;
    bool pdp1Active = pdpStatus.indexOf("+CNACT: 1,1") != -1;
    if (!pdp0Active && !pdp1Active) {
      SerialMon.println("PDP context not active");
      return false;
    }
  }
  
  return cgattStatus.indexOf("+CGATT: 1") != -1;
}

// モデムをハードリセットする関数
void hardResetModem() {
  SerialMon.println("Performing hard reset of modem...");
  
  // モデムの電源を切る（ATコマンドでの電源制御）
  modem.sendAT("+CPOWD=1");
  modem.waitResponse(10000L);
  delay(5000);
  
  // モデムを再初期化
  SerialMon.println("Reinitializing modem...");
  modem.init();
  delay(3000);
  
  // ネットワークに再接続
  SerialMon.println("Waiting for network registration...");
  if (!modem.waitForNetwork(60000L)) {
    SerialMon.println("Network registration failed after hard reset");
    ESP.restart();
    return;
  }
  
  SerialMon.println("Network registered successfully");
  SerialMon.print("Network Operator: ");
  SerialMon.println(modem.getOperator());
  
  // GPRSに再接続
  SerialMon.println("Connecting to GPRS...");
  if (!modem.gprsConnect("soracom.io", "sora", "sora")) {
    SerialMon.println("GPRS connection failed after hard reset");
    ESP.restart();
    return;
  }
  
  SerialMon.println("GPRS connected");
  SerialMon.print("Local IP: ");
  SerialMon.println(modem.localIP());
  
  // メタデータからインターバル設定と回線情報を取得
  fetchAndUpdateInterval();
  fetchSubscriberInfo();
}

// UDPソケットを開く関数（リトライ処理付き）
bool openUdpSocket() {
  int socketRetries = 3;
  int socketBaseDelay = 1000;
  
  for (int attempt = 0; attempt < socketRetries; attempt++) {
    // UDPソケットをクローズ
    SerialMon.println("Closing any existing UDP socket...");
    modem.sendAT("+CACLOSE=0");
    modem.waitResponse(10000L);
    
    // バッファをクリア
    while (SerialAT.available()) {
      SerialAT.read();
    }
    
    // UDPソケットを開く
    SerialMon.println("Opening UDP socket...");
    modem.sendAT("+CAOPEN=0,0,\"UDP\",\"" + String(udpServer) + "\"," + String(udpPort));
    
    String atResponse = "";
    int response = modem.waitResponse(20000L, atResponse); // タイムアウトを20秒に延長
    
    if (response == 1) {
      SerialMon.println("UDP socket opened successfully!");
      return true;
    } else {
      SerialMon.println("Failed to open UDP socket. AT Response:");
      SerialMon.println(atResponse);
      
      if (atResponse.length() == 0) {
        SerialMon.println("Empty AT response, checking modem status...");
        if (!checkModemStatus()) {
          SerialMon.println("Modem status check failed, performing hard reset...");
          hardResetModem();
        }
      }
      
      if (attempt < socketRetries - 1) {
        int jitter = rand() % 1000;
        int delayTime = socketBaseDelay * (1 << attempt) + jitter;
        SerialMon.printf("Retry %d/%d, waiting for %d ms\n", attempt + 1, socketRetries, delayTime);
        delay(delayTime);
      }
    }
  }
  
  return false;
}

void sendData(uint8_t* payload, size_t payloadSize) {
  int maxRetries = 5;
  int baseDelay = 1000;
  
  for (int attempt = 0; attempt < maxRetries; attempt++) {
    // データ送信ATコマンド
    modem.sendAT("+CASEND=0," + String(payloadSize));
    if (modem.waitResponse(">") != 1) {
      // 指数バックオフ + ジッター戦略
      int jitter = rand() % 1000;
      int delayTime = baseDelay * (1 << attempt) + jitter;
      
      SerialMon.printf("Failed to initiate data send, retrying...\n");
      SerialMon.printf("Retry %d/%d, waiting for %d ms\n", attempt + 1, maxRetries, delayTime);
      delay(delayTime);
      
      // モデムの詳細な状態確認
      if (!checkModemStatus()) {
        SerialMon.println("Modem status check failed, attempting recovery...");
        
        // 2回目以降のリトライでハードリセットを試みる
        if (attempt >= 1) {
          hardResetModem();
        } else {
          // GPRSに再接続
          modem.gprsDisconnect();
          delay(1000);
          modem.gprsConnect("soracom.io", "sora", "sora");
          delay(2000);
        }
      }
      
      // UDPソケットを再度開く
      if (!openUdpSocket()) {
        SerialMon.println("Failed to reopen UDP socket after multiple attempts");
        
        // 最後のリトライでなければ続行
        if (attempt < maxRetries - 1) {
          continue;
        } else {
          // 最大リトライ回数に達した場合、M5Stackを再起動
          SerialMon.println("Failed to send data after maximum retries. Restarting M5Stack...");
          ESP.restart();
          return;
        }
      }
    } else {
      // データ送信
      SerialAT.write(payload, payloadSize);
      if (modem.waitResponse() != 1) {
        SerialMon.println("Failed to send data, retrying...");
        delay(500);
        continue;
      }

      SerialMon.println("Data sent successfully!");
      return; // 成功したので終了
    }
  }

  // 最大リトライ回数に達した場合、M5Stackを再起動
  SerialMon.println("Failed to send data after maximum retries. Restarting M5Stack...");
  ESP.restart();
}

// データ送信の結果を返す関数（成功:true, 失敗:false）
bool sendDataWithStatus(uint8_t* payload, size_t payloadSize) {
  int maxRetries = 5;
  int baseDelay = 1000;
  
  for (int attempt = 0; attempt < maxRetries; attempt++) {
    // データ送信ATコマンド
    modem.sendAT("+CASEND=0," + String(payloadSize));
    if (modem.waitResponse(">") != 1) {
      // 指数バックオフ + ジッター戦略
      int jitter = rand() % 1000;
      int delayTime = baseDelay * (1 << attempt) + jitter;
      
      SerialMon.printf("Failed to initiate data send, retrying...\n");
      SerialMon.printf("Retry %d/%d, waiting for %d ms\n", attempt + 1, maxRetries, delayTime);
      delay(delayTime);
      
      // モデムの詳細な状態確認
      if (!checkModemStatus()) {
        SerialMon.println("Modem status check failed, attempting recovery...");
        
        // 2回目以降のリトライでハードリセットを試みる
        if (attempt >= 1) {
          hardResetModem();
        } else {
          // GPRSに再接続
          modem.gprsDisconnect();
          delay(1000);
          modem.gprsConnect("soracom.io", "sora", "sora");
          delay(2000);
        }
      }
      
      // UDPソケットを再度開く
      if (!openUdpSocket()) {
        SerialMon.println("Failed to reopen UDP socket after multiple attempts");
        
        // 最後のリトライでなければ続行
        if (attempt < maxRetries - 1) {
          continue;
        } else {
          // 最大リトライ回数に達した場合、M5Stackを再起動
          SerialMon.println("Failed to send data after maximum retries. Restarting M5Stack...");
          ESP.restart();
          return false;
        }
      }
    } else {
      SerialAT.write(payload, payloadSize);
      if (modem.waitResponse() != 1) {
        SerialMon.println("Failed to send data, retrying...");
        delay(500);
        continue;
      }

      SerialMon.println("Data sent successfully!");
      return true; // 成功
    }
  }

  SerialMon.println("Failed to send data after maximum retries. Restarting M5Stack...");
  ESP.restart();
  return false; // 失敗（ここには到達しないはずだが、コンパイルエラー回避のため）
}

// モデムをリセットする関数
void resetModem() {
  SerialMon.println("Resetting modem connection...");
  
  // モデムの状態を詳細に確認
  if (!checkModemStatus()) {
    SerialMon.println("Modem status check failed, performing hard reset");
    hardResetModem();
    return;
  }
  
  // GPRSを切断
  modem.gprsDisconnect();
  delay(1000);
  
  // UDPソケットをクローズ（UDPモード時のみ有効だが、冪等に実行）
  modem.sendAT("+CACLOSE=0");
  modem.waitResponse(5000L);
  
  // MQTTセッションがあるなら明示的に切断（保険）
  mqttDisconnect();

  // モデムをソフトリセット
  modem.restart();
  delay(3000);
  
  // ネットワークに再接続
  SerialMon.println("Reconnecting to network...");
  if (!modem.waitForNetwork(60000L)) {
    SerialMon.println("Network reconnection failed, performing hard reset");
    hardResetModem();
    return;
  }
  
  // GPRSに再接続
  if (!modem.gprsConnect("soracom.io", "sora", "sora")) {
    SerialMon.println("GPRS reconnection failed, performing hard reset");
    hardResetModem();
    return;
  }

  // 最新メタデータを取得（モード/トピック/qosの更新を反映）
  fetchAndUpdateInterval();
  fetchSubscriberInfo();
  
  if (mqttEnabled && mqttConfigValid) {
    SerialMon.println("Reconfiguring MQTT after modem reset...");
    if (!mqttConfigure()) {
      SerialMon.println("MQTT configure failed after reset");
    } else if (!mqttConnect()) {
      SerialMon.println("MQTT connect failed after reset");
    }
  } else {
    // UDPソケットを再度開く
    if (!openUdpSocket()) {
      SerialMon.println("Failed to reopen UDP socket after reset, restarting device");
      ESP.restart();
      return;
    }
  }
  
  SerialMon.println("Modem reset and reconnected successfully");
  consecutiveFailures = 0; // 失敗カウンターをリセット
}

// I2Cデバイススキャン関数
void scanI2CDevices() {
  SerialMon.println("Scanning I2C devices...");
  byte error, address;
  int nDevices = 0;
  
  for(address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    
    if (error == 0) {
      SerialMon.printf("I2C device found at address 0x%02X", address);
      if (address == 0x28) {
        SerialMon.print(" (FS3000 Air Velocity Sensor)");
      } else if (address == 0x62) {
        SerialMon.print(" (SCD40 CO2 Sensor)");
      }
      SerialMon.println();
      nDevices++;
    }
  }
  
  if (nDevices == 0) {
    SerialMon.println("No I2C devices found");
  } else {
    SerialMon.printf("Found %d I2C devices\n", nDevices);
  }
  SerialMon.println();
}

void loop() {
  unsigned long current = millis();
  
  // 長時間通信が成功していない場合、モデムをリセット
  if (lastSuccessfulSend > 0 && (current - lastSuccessfulSend) > CONNECTION_TIMEOUT) {
    SerialMon.println("Communication timeout detected. No successful data transmission for 5 minutes.");
    resetModem();
    lastSuccessfulSend = current; // リセット後にタイムアウトカウンターをリセット
  }
  
  // インターバルに基づいてデータ送信と画面更新
  if (current - lastUpdate >= INTERVAL) {
    readAndSendData();
  }

  M5.update();
}
// ==== MQTT helper implementations (SIM7080 AT commands) ====

// MQTT URL設定のフォールバック実装
static bool mqttConfUrlWithFallback() {
  // 1) 推奨: URL と ポートを分けて設定
  modem.sendAT("+SMCONF=\"URL\",\"beam.soracom.io\",1883");
  if (modem.waitResponse(5000L) == 1) {
    return true;
  }
  SerialMon.println("SMCONF URL with separate port failed, trying single-arg fallback...");

  // 2) 一部FW向け: "beam.soracom.io,1883" を単一引数として渡す
  modem.sendAT("+SMCONF=\"URL\",\"beam.soracom.io,1883\"");
  if (modem.waitResponse(5000L) == 1) {
    return true;
  }
  SerialMon.println("SMCONF URL fallback also failed");
  return false;
}

bool isValidMqttTopic(const String& topic) {
  if (topic.length() == 0 || topic.length() > 256) return false;
  for (size_t i = 0; i < topic.length(); ++i) {
    char c = topic[i];
    if (c < 32 || c > 126) return false; // 可視ASCIIのみ
  }
  return true;
}

bool mqttConfigure() {
  bool ok = true;

  // URL
  if (!mqttConfUrlWithFallback()) ok = false;
  // Force MQTT to use PDP context ID 0
  modem.sendAT("+SMCONF=\"CONNID\",0");
  if (modem.waitResponse(5000L) != 1) { SerialMon.println("SMCONF CONNID failed"); ok = false; }

  // CLIENTID = IMSI（取得済み想定。空なら代替でIMEI）
  String clientId = subscriberImsi;
  if (clientId.length() == 0 || clientId == "Unknown") {
    clientId = modem.getIMEI();
  }
  modem.sendAT("+SMCONF=\"CLIENTID\",\"" + clientId + "\"");
  if (modem.waitResponse(5000L) != 1) { SerialMon.println("SMCONF CLIENTID failed"); ok = false; }

  // セッション/キープ/同期モード
  modem.sendAT("+SMCONF=\"CLEANSS\",1");
  if (modem.waitResponse(5000L) != 1) { SerialMon.println("SMCONF CLEANSS failed"); ok = false; }
  modem.sendAT("+SMCONF=\"KEEPTIME\",60");
  if (modem.waitResponse(5000L) != 1) { SerialMon.println("SMCONF KEEPTIME failed"); ok = false; }
  modem.sendAT("+SMCONF=\"ASYNCMODE\",0");
  if (modem.waitResponse(5000L) != 1) { SerialMon.println("SMCONF ASYNCMODE failed"); ok = false; }

  // 認証なし
  modem.sendAT("+SMCONF=\"USERNAME\",\"\"");
  if (modem.waitResponse(5000L) != 1) { SerialMon.println("SMCONF USERNAME failed"); ok = false; }
  modem.sendAT("+SMCONF=\"PASSWORD\",\"\"");
  if (modem.waitResponse(5000L) != 1) { SerialMon.println("SMCONF PASSWORD failed"); ok = false; }

  // 既定QOSの設定（実送信はSMPUBの引数も使用）
  modem.sendAT("+SMCONF=\"QOS\"," + String(mqttQos));
  if (modem.waitResponse(5000L) != 1) { SerialMon.println("SMCONF QOS failed"); ok = false; }

  return ok;
}

bool isMqttOnline() {
  String resp = "";
  modem.sendAT("+SMSTATE?");
  if (modem.waitResponse(5000L, resp) != 1) {
    mqttConnected = false;
    return false;
  }
  // +SMSTATE: <status>  1 or 2 でオンライン
  if (resp.indexOf("+SMSTATE: 1") != -1 || resp.indexOf("+SMSTATE: 2") != -1) {
    mqttConnected = true;
    return true;
  }
  mqttConnected = false;
  return false;
}

bool mqttConnect() {
  const int maxRetries = 3;
  const int baseDelay = 1000;

  for (int attempt = 0; attempt < maxRetries; ++attempt) {
    // 既にオンラインなら成功
    if (isMqttOnline()) return true;

    // PDP#0 が非アクティブなら再活性化を試行
    if (!ensurePdp0Active()) {
      int jitter = rand() % 1000;
      int delayTime = baseDelay * (1 << attempt) + jitter;
      SerialMon.printf("PDP#0 activation failed, retry %d/%d after %d ms\n", attempt + 1, maxRetries, delayTime);
      delay(delayTime);
      continue;
    }

    // 接続前の診断ログ: PDP と MQTT 状態
    {
      String pdp = "";
      modem.sendAT("+CNACT?");
      modem.waitResponse(5000L, pdp);
      SerialMon.println("PDP status before SMCONN:");
      SerialMon.println(pdp);

      String stBefore = "";
      modem.sendAT("+SMSTATE?");
      modem.waitResponse(5000L, stBefore);
      SerialMon.println("SMSTATE before SMCONN:");
      SerialMon.println(stBefore);
    }

    SerialMon.println("MQTT connecting (AT+SMCONN)...");
    modem.sendAT("+SMCONN");
    if (modem.waitResponse(60000L) == 1) {
      // 接続後の状態を確認
      String stAfter = "";
      modem.sendAT("+SMSTATE?");
      modem.waitResponse(5000L, stAfter);
      SerialMon.println("SMSTATE after SMCONN:");
      SerialMon.println(stAfter);

      if (isMqttOnline()) {
        SerialMon.println("MQTT connected");
        return true;
      }
    }

    // リトライ待機（指数バックオフ + ジッター）
    int jitter = rand() % 1000;
    int delayTime = baseDelay * (1 << attempt) + jitter;
    SerialMon.printf("MQTT connect retry %d/%d, waiting %d ms\n", attempt + 1, maxRetries, delayTime);
    delay(delayTime);
  }

  // 既定回数失敗時のフォールバック: MQTTスタック再初期化
  SerialMon.println("MQTT connect failed after retries - resetting MQTT stack (SMDISC + SMCONF reapply)");
  mqttDisconnect();
  // 再度SMCONF一式を適用（失敗しても続行）
  if (!mqttConfigure()) {
    SerialMon.println("Reapply SMCONF returned error, proceeding anyway");
  }

  // PDP#0を再確認・再活性化
  if (!ensurePdp0Active()) {
    SerialMon.println("PDP#0 still inactive after reconfigure");
    return false;
  }

  // 最終接続試行（1ラウンド）
  {
    String pdp = "";
    modem.sendAT("+CNACT?");
    modem.waitResponse(5000L, pdp);
    SerialMon.println("PDP status before SMCONN (final):");
    SerialMon.println(pdp);

    String stBefore = "";
    modem.sendAT("+SMSTATE?");
    modem.waitResponse(5000L, stBefore);
    SerialMon.println("SMSTATE before SMCONN (final):");
    SerialMon.println(stBefore);
  }

  SerialMon.println("MQTT connecting (AT+SMCONN) final attempt...");
  modem.sendAT("+SMCONN");
  if (modem.waitResponse(60000L) == 1) {
    String stAfter = "";
    modem.sendAT("+SMSTATE?");
    modem.waitResponse(5000L, stAfter);
    SerialMon.println("SMSTATE after SMCONN (final):");
    SerialMon.println(stAfter);

    if (isMqttOnline()) {
      SerialMon.println("MQTT connected (after stack reset)");
      return true;
    }
  }

  SerialMon.println("MQTT connect failed after stack reset");
  return false;
}

void mqttDisconnect() {
  SerialMon.println("MQTT disconnecting (AT+SMDISC)...");
  modem.sendAT("+SMDISC");
  modem.waitResponse(10000L);
  mqttConnected = false;
}

bool mqttPublish(const String& topic, const String& json, int qos) {
  if (!mqttEnabled || !mqttConfigValid) {
    SerialMon.println("MQTT publish skipped: MQTT disabled or config invalid");
    return false;
  }

  if (!isValidMqttTopic(topic)) {
    SerialMon.println("MQTT publish skipped: topic invalid");
    return false;
  }

  if (!isMqttOnline()) {
    SerialMon.println("MQTT not online, trying to reconnect...");
    if (!mqttConnect()) {
      SerialMon.println("MQTT reconnect failed, cannot publish");
      return false;
    }
  }

  int length = json.length();
  SerialMon.printf("Publishing via MQTT: topic=%s len=%d qos=%d\n", topic.c_str(), length, qos);

  modem.sendAT("+SMPUB=\"" + topic + "\"," + String(length) + "," + String(qos) + ",0");
  if (modem.waitResponse(">") != 1) {
    SerialMon.println("SMPUB prompt not received");
    return false;
  }

  // 本文送出
  SerialAT.print(json);

  if (modem.waitResponse(10000L) != 1) {
    SerialMon.println("SMPUB publish failed");
    return false;
  }

  SerialMon.println("SMPUB OK");
  return true;
}