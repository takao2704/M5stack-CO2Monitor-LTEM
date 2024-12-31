#include <M5Stack.h>
#include <Wire.h>

// SparkFun SCD4x (SCD40 / SCD41対応) ライブラリ
#include "SparkFun_SCD4x_Arduino_Library.h"

// インスタンス生成
SCD4x scd40;

// センサー読み取り周期 (ミリ秒)
static const unsigned long INTERVAL = 5000;
unsigned long lastUpdate = 0;

void setup()
{
  // --- M5Stackの初期化 ---
  M5.begin();

  // --- I2C初期化 (PortAのSDA=21, SCL=22) ---
  // M5Stackライブラリが裏で呼んでくれますが、念のため明示的に呼んでもOK
  Wire.begin(21, 22);

  // --- SCD40 (SCD4x) の初期化 ---
  if (!scd40.begin(Wire))
  {
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.println("SCD40 init failed! Check wiring/address = 0x62");
  }
  else
  {
    // 定期測定開始
    scd40.startPeriodicMeasurement();
    M5.Lcd.clear(BLACK);
    M5.Lcd.println("SCD40 (SCD4x) Setup Complete.");
  }
}

void loop()
{
  unsigned long current = millis();
  if (current - lastUpdate >= INTERVAL)
  {
    lastUpdate = current;

    // SCD4xライブラリの readMeasurement() は
    // 新しいデータがある場合に true を返します。
    if (scd40.readMeasurement())
    {
      // 取得
      float co2       = scd40.getCO2();
      float temp      = scd40.getTemperature();
      float humidity  = scd40.getHumidity();

      // 画面表示
      M5.Lcd.clear(BLACK);
      M5.Lcd.setCursor(0, 0);
      M5.Lcd.setTextFont(4);  // フォント番号4を指定 (より大きめ)
      M5.Lcd.println("UNIT CO2 (SCD40)");
      M5.Lcd.printf("CO2   : %.0f ppm\n",   co2);
      M5.Lcd.printf("Temp  : %.2f C\n",     temp);
      M5.Lcd.printf("Hum   : %.2f %%\n",    humidity);
    }
    else
    {
      // データがまだ準備できていないか、通信エラー
      M5.Lcd.clear(BLACK);
      M5.Lcd.println("SCD40 readMeasurement() failed");
    }
  }

  M5.update();
}
