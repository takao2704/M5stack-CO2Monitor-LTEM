#define SerialMon Serial

#include <M5Stack.h>
#include <Wire.h>
#include "SparkFun_SCD4x_Arduino_Library.h"

#define TINY_GSM_MODEM_SIM7080
#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>

#include <stdlib.h>

// インスタンス生成
SCD4x scd40;

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

// 関数プロトタイプ宣言
bool checkModemStatus();
void hardResetModem();
bool openUdpSocket();
void sendData(uint8_t* payload, size_t payloadSize);
bool sendDataWithStatus(uint8_t* payload, size_t payloadSize);
void resetModem();
void readAndSendData();

// センサーデータの読み取り、送信、画面更新を行う関数
void readAndSendData() {
  unsigned long current = millis();

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
    bool sendSuccess = sendDataWithStatus(payload, sizeof(payload));
    
    if (sendSuccess) {
      // 送信成功
      consecutiveFailures = 0; // 失敗カウンターをリセット
      lastSuccessfulSend = current; // 最後の成功送信時間を更新
    } else {
      // 送信失敗
      consecutiveFailures++;
      SerialMon.printf("Consecutive failures: %d/%d\n", consecutiveFailures, MAX_CONSECUTIVE_FAILURES);
      
      // 連続失敗回数が閾値を超えた場合、モデムをリセット
      if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
        SerialMon.println("Too many consecutive failures, resetting modem...");
        resetModem();
      }
    }

    M5.Lcd.clear(BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.setTextFont(4);
    M5.Lcd.println("UNIT CO2 (SCD40)");
    M5.Lcd.printf("CO2   : %.0f ppm\n", co2);
    M5.Lcd.printf("Temp  : %.2f C\n", temp);
    M5.Lcd.printf("Hum   : %.2f %%\n", humidity);
    
    // 通信状態を表示
    M5.Lcd.setTextFont(2);
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

    SerialMon.printf("CO2: %.0f ppm, Temp: %.2f C, Hum: %.2f %%\n", co2, temp, humidity);
  } else {
    M5.Lcd.clear(BLACK);
    M5.Lcd.println("SCD40 readMeasurement() failed");
    SerialMon.println("SCD40 readMeasurement() failed");
  }
  
  // 最後の更新時間を記録
  lastUpdate = current;
}

// SORACOMメタデータからインターバル設定を取得する関数
void fetchAndUpdateInterval() {
  SerialMon.println("Fetching interval setting from SORACOM metadata...");
  
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
  
  // JSONデータのパース
  DynamicJsonDocument doc(256);
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
    if (pdpStatus.indexOf("+CNACT: 1,1") == -1) {
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
  
  // UDPソケットをクローズ
  modem.sendAT("+CACLOSE=0");
  modem.waitResponse(5000L);
  
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
  
  // UDPソケットを再度開く
  if (!openUdpSocket()) {
    SerialMon.println("Failed to reopen UDP socket after reset, restarting device");
    ESP.restart();
    return;
  }
  
  SerialMon.println("Modem reset and reconnected successfully");
  consecutiveFailures = 0; // 失敗カウンターをリセット
  
  // メタデータからインターバル設定と回線情報を取得
  fetchAndUpdateInterval();
  fetchSubscriberInfo();
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