# FS3000空気速度センサー統合実装計画

## 概要

M5Stack CO2モニターシステムに、FS3000空気速度センサー（I2Cアドレス: 0x28）を追加し、風速値（m/s）の取得・表示・送信機能を実装します。

## 現在のシステム構成

- **ハードウェア**: M5Stack Core + SIM7080 LTE通信モジュール
- **既存センサー**: SCD40 CO2センサー（I2Cアドレス: 0x62）
- **通信**: SORACOM経由でのUDPデータ送信
- **表示**: LCD画面 + シリアル出力

## 実装目標

1. FS3000センサーからの風速データ取得
2. シリアル出力への風速値追加表示
3. LCD画面への風速情報表示
4. SORACOM経由での風速データ送信

## 実装計画詳細

### 1. ライブラリとヘッダーファイルの追加

#### platformio.ini の更新
```ini
lib_deps =
    m5stack/M5Stack@^0.4.6
    sparkfun/SparkFun SCD4x Arduino Library@^1.1.2
    sparkfun/SparkFun FS3000 Arduino Library@^1.0.0  ; 追加
    https://github.com/Seeed-Studio/Seeed_Arduino_BME68x.git
    https://github.com/vshymanskyy/TinyGSM.git
    arduino-libraries/ArduinoHttpClient@^0.4.0
    bblanchon/ArduinoJson@^6.21.3
```

#### main.cpp ヘッダー追加
```cpp
#include <M5Stack.h>
#include <Wire.h>
#include "SparkFun_SCD4x_Arduino_Library.h"
#include "SparkFun_FS3000_Arduino_Library.h"  // 追加
```

### 2. センサーインスタンスの作成

```cpp
// インスタンス生成
SCD4x scd40;
FS3000 fs3000;  // 追加
```

### 3. センサー初期化の実装

#### setup()関数への追加
```cpp
// --- FS3000 空気速度センサーの初期化 ---
if (!fs3000.begin(Wire)) {
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.println("FS3000 init failed! Check wiring/address = 0x28");
    SerialMon.println("FS3000 init failed! Check wiring/address = 0x28");
} else {
    M5.Lcd.clear(BLACK);
    M5.Lcd.println("FS3000 Air Velocity Sensor Setup Complete.");
    SerialMon.println("FS3000 Air Velocity Sensor Setup Complete.");
}
```

### 4. データ取得機能の拡張

#### readAndSendData()関数の更新

```cpp
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
    
    if (fs3000.readSensor()) {
        windSpeed = fs3000.getWindSpeed();  // m/s単位で取得
        fs3000Success = true;
    }
    
    // データをバイナリ形式でパッキング（16バイトに拡張）
    uint8_t payload[16];
    memcpy(payload, &co2, sizeof(co2));
    memcpy(payload + 4, &temp, sizeof(temp));
    memcpy(payload + 8, &humidity, sizeof(humidity));
    memcpy(payload + 12, &windSpeed, sizeof(windSpeed));  // 風速データ追加
    
    // バイナリパーサー設定
    // co2::float:32:little-endian Temp::float:32:little-endian Humi::float:32:little-endian Wind::float:32:little-endian
    
    SerialMon.println("Sending data...");
    bool sendSuccess = sendDataWithStatus(payload, sizeof(payload));
    
    if (sendSuccess) {
        consecutiveFailures = 0;
        lastSuccessfulSend = current;
    } else {
        consecutiveFailures++;
        SerialMon.printf("Consecutive failures: %d/%d\n", consecutiveFailures, MAX_CONSECUTIVE_FAILURES);
        
        if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
            SerialMon.println("Too many consecutive failures, resetting modem...");
            resetModem();
        }
    }
    
    // LCD表示の更新
    M5.Lcd.clear(BLACK);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.setTextFont(4);
    M5.Lcd.println("UNIT CO2 + Wind Monitor");
    
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
    M5.Lcd.printf("Network: %s\n", sendSuccess ? "OK" : "Error");
    M5.Lcd.printf("Fails: %d/%d\n", consecutiveFailures, MAX_CONSECUTIVE_FAILURES);
    M5.Lcd.printf("Interval: %lu sec\n", INTERVAL / 1000);
    
    // IMSI表示（短縮）
    String shortImsi = subscriberImsi;
    if (shortImsi.length() > 6) {
        shortImsi = "..." + shortImsi.substring(shortImsi.length() - 6);
    }
    M5.Lcd.printf("IMSI: %s\n", shortImsi.c_str());
    
    // 回線名表示（短縮）
    String shortName = subscriberName;
    if (shortName.length() > 10) {
        shortName = shortName.substring(0, 10) + "...";
    }
    M5.Lcd.printf("Name: %s\n", shortName.c_str());
    
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
    
    lastUpdate = current;
}
```

### 5. エラーハンドリングの実装

#### センサー読み取りエラー処理
- SCD40とFS3000の読み取りを独立して処理
- 一方のセンサーが失敗しても、もう一方のデータは送信
- エラー状態をLCD画面とシリアル出力に表示

#### I2Cアドレス競合チェック
```cpp
// setup()関数内でI2Cデバイススキャンを実行
void scanI2CDevices() {
    SerialMon.println("Scanning I2C devices...");
    byte error, address;
    int nDevices = 0;
    
    for(address = 1; address < 127; address++) {
        Wire.beginTransmission(address);
        error = Wire.endTransmission();
        
        if (error == 0) {
            SerialMon.printf("I2C device found at address 0x%02X\n", address);
            nDevices++;
        }
    }
    
    if (nDevices == 0) {
        SerialMon.println("No I2C devices found");
    } else {
        SerialMon.printf("Found %d I2C devices\n", nDevices);
    }
}
```

### 6. データ送信の拡張

#### ペイロード構造の更新
- **現在**: 12バイト（CO2 + 温度 + 湿度）
- **更新後**: 16バイト（CO2 + 温度 + 湿度 + 風速）

#### SORACOMバイナリパーサー設定
```
co2::float:32:little-endian Temp::float:32:little-endian Humi::float:32:little-endian Wind::float:32:little-endian
```

## 実装手順

### Phase 1: 基本実装
1. `platformio.ini`にFS3000ライブラリを追加
2. `main.cpp`にヘッダーファイルとインスタンスを追加
3. `setup()`関数にFS3000初期化コードを追加

### Phase 2: データ取得実装
4. `readAndSendData()`関数を拡張してFS3000データ取得を追加
5. シリアル出力に風速値を追加
6. LCD表示を更新

### Phase 3: データ送信実装
7. ペイロード構造を16バイトに拡張
8. 風速データをバイナリ形式で追加
9. SORACOMバイナリパーサー設定を更新

### Phase 4: テストと検証
10. I2Cデバイススキャンでセンサー検出確認
11. センサーデータ取得テスト
12. データ送信テスト
13. エラーハンドリングテスト

## 技術的考慮事項

### ハードウェア
- **I2Cアドレス**: 0x28（FS3000）と0x62（SCD40）で競合なし
- **電力消費**: FS3000の追加による消費電力増加を監視
- **配線**: M5Stack PortA（SDA=21, SCL=22）に両センサーを接続

### ソフトウェア
- **メモリ使用量**: FS3000ライブラリ追加による影響を確認
- **処理時間**: センサー読み取り時間の増加を考慮
- **エラー処理**: 各センサーの独立したエラーハンドリング

### 通信
- **データサイズ**: ペイロードが12→16バイトに増加
- **送信頻度**: 風速データの変動特性を考慮した適切な間隔
- **パーサー設定**: SORACOMでの新しいバイナリ形式対応

## 期待される結果

### シリアル出力例
```
FS3000 Air Velocity Sensor Setup Complete.
I2C device found at address 0x28
I2C device found at address 0x62
Found 2 I2C devices
CO2: 412 ppm, Temp: 24.8 C, Hum: 45.2 %, Wind: 1.25 m/s
Sending data...
Data sent successfully!
```

### LCD表示例
```
UNIT CO2 + Wind Monitor
CO2   : 412 ppm
Temp  : 24.8 C
Hum   : 45.2 %
Wind  : 1.25 m/s
Network: OK
Fails: 0/3
Interval: 10 sec
IMSI: ...123456
Name: TestSIM...
```

## 注意事項

1. **ライブラリ互換性**: SparkFun FS3000ライブラリのバージョン確認
2. **センサー較正**: FS3000の較正が必要な場合の対応
3. **データ精度**: 風速測定の精度と有効範囲の確認
4. **環境条件**: 温度・湿度がFS3000測定値に与える影響

## トラブルシューティング

### よくある問題と対処法

1. **FS3000初期化失敗**
   - I2Cアドレス確認（0x28）
   - 配線確認（SDA/SCL）
   - 電源供給確認

2. **データ読み取りエラー**
   - センサーの応答時間確認
   - I2C通信速度の調整
   - プルアップ抵抗の確認

3. **データ送信エラー**
   - ペイロードサイズの確認
   - バイナリ形式の検証
   - SORACOMパーサー設定の確認

この実装計画に基づいて、段階的にFS3000空気速度センサーの統合を進めることができます。