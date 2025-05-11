# SORACOMメタデータを使用したインターバル制御の実装計画

## 概要

SORACOMのメタデータエンドポイント（http://metadata.soracom.io/v1/userdata）からJSONデータを取得し、`interval_s`の値を使用してデータ送信インターバルを動的に制御する機能を実装します。

## 実装内容

### 1. 必要なライブラリ

- **ArduinoJson**: JSONデータのパース用
  - platformio.iniに追加: `bblanchon/ArduinoJson@^6.21.3`

### 2. 変数の変更

- 現在の定数 `INTERVAL` を変数に変更
```cpp
// 変更前
static const unsigned long INTERVAL = 10000; // 10秒に変更

// 変更後
static unsigned long INTERVAL = 10000; // デフォルト値は10秒
```

### 3. メタデータ取得関数の実装

```cpp
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
```

### 4. 接続箇所への実装追加

1. **setup関数**:
```cpp
// ネットワーク接続成功後
SerialMon.println("GPRS connected");
SerialMon.print("Local IP: ");
SerialMon.println(modem.localIP());

// メタデータからインターバル設定を取得
fetchAndUpdateInterval();
```

2. **resetModem関数**:
```cpp
// UDPソケットを再度開く処理の後
SerialMon.println("Modem reset and reconnected successfully");
consecutiveFailures = 0; // 失敗カウンターをリセット

// メタデータからインターバル設定を取得
fetchAndUpdateInterval();
```

3. **hardResetModem関数**:
```cpp
// GPRS接続成功後
SerialMon.println("GPRS connected");
SerialMon.print("Local IP: ");
SerialMon.println(modem.localIP());

// メタデータからインターバル設定を取得
fetchAndUpdateInterval();
```

## 実装手順

1. platformio.iniにArduinoJsonライブラリを追加
2. `INTERVAL`定数を変数に変更
3. `fetchAndUpdateInterval()`関数を実装
4. 各接続箇所にメタデータ取得処理を追加

## 注意点

- メタデータ取得に失敗した場合は、現在のインターバル設定を維持します
- HTTPリクエストのタイムアウト設定は適切に調整する必要があります
- JSONデータのサイズに応じて、`DynamicJsonDocument`のサイズを調整する必要があります