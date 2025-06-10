#include "FS3000.h"

FS3000::FS3000() {
}

bool FS3000::begin(TwoWire &wirePort) {
    _i2cPort = &wirePort;
    if (isConnected() == false) {
        return false;
    }
    return true;
}

bool FS3000::isConnected() {
    _i2cPort->beginTransmission((uint8_t)FS3000_DEVICE_ADDRESS);
    if (_i2cPort->endTransmission() != 0) {
        return false;
    }
    return true;
}

void FS3000::setRange(uint8_t range) {
    _range = range;
    const float mpsDataPoint_7_mps[9] = {0, 1.07, 2.01, 3.00, 3.97, 4.96, 5.98, 6.99, 7.23};
    const int rawDataPoint_7_mps[9] = {409, 915, 1522, 2066, 2523, 2908, 3256, 3572, 3686};

    const float mpsDataPoint_15_mps[13] = {0, 2.00, 3.00, 4.00, 5.00, 6.00, 7.00, 8.00, 9.00, 10.00, 11.00, 13.00, 15.00};
    const int rawDataPoint_15_mps[13] = {409, 1203, 1597, 1908, 2187, 2400, 2629, 2801, 3006, 3178, 3309, 3563, 3686};

    if (_range == AIRFLOW_RANGE_7_MPS) {
        for (int i = 0; i < 9; i++) {
            _mpsDataPoint[i] = mpsDataPoint_7_mps[i];
            _rawDataPoint[i] = rawDataPoint_7_mps[i];
        }
    } else if (_range == AIRFLOW_RANGE_15_MPS) {
        for (int i = 0; i < 13; i++) {
            _mpsDataPoint[i] = mpsDataPoint_15_mps[i];
            _rawDataPoint[i] = rawDataPoint_15_mps[i];
        }
    }
}

uint16_t FS3000::readRaw() {
    readData(_buff);
    bool checksum_result = checksum(_buff, true); // デバッグオン
    if (checksum_result == false) {
        Serial.println("FS3000: Checksum failed");
        return 0;
    }
    
    uint16_t airflowRaw = 0;
    uint8_t data_high_byte = _buff[1];
    uint8_t data_low_byte = _buff[2];

    // 上位4ビットのみ有効
    data_high_byte &= 0x0F;
    
    // 結合
    airflowRaw |= data_low_byte;
    airflowRaw |= (data_high_byte << 8);

    Serial.printf("FS3000: Raw value: %d\n", airflowRaw);
    return airflowRaw;
}

float FS3000::readMetersPerSecond() {
    float airflowMps = 0.0;
    int airflowRaw = readRaw();

    // データポイント数を決定
    uint8_t dataPointsNum = 9; // デフォルトはFS3000-1005
    if (_range == AIRFLOW_RANGE_7_MPS) {
        dataPointsNum = 9;
    } else if (_range == AIRFLOW_RANGE_15_MPS) {
        dataPointsNum = 13;
    }

    // データ位置を見つける
    int data_position = 0;
    for (int i = 0; i < dataPointsNum; i++) {
        if (airflowRaw > _rawDataPoint[i]) {
            data_position = i;
        }
    }

    // 最小値・最大値の処理
    if (airflowRaw <= 409) {
        Serial.println("FS3000: Raw value at minimum, returning 0.0");
        return 0;
    }
    if (airflowRaw >= 3686) {
        if (_range == AIRFLOW_RANGE_7_MPS) {
            Serial.println("FS3000: Raw value at maximum, returning 7.23");
            return 7.23;
        }
        if (_range == AIRFLOW_RANGE_15_MPS) {
            Serial.println("FS3000: Raw value at maximum, returning 15.0");
            return 15.00;
        }
    }

    // 線形補間
    int window_size = (_rawDataPoint[data_position + 1] - _rawDataPoint[data_position]);
    int diff = (airflowRaw - _rawDataPoint[data_position]);
    float percentage_of_window = ((float)diff / (float)window_size);

    float window_size_mps = (_mpsDataPoint[data_position + 1] - _mpsDataPoint[data_position]);
    airflowMps = _mpsDataPoint[data_position] + (window_size_mps * percentage_of_window);

    Serial.printf("FS3000: Position: %d, Window: %d, Diff: %d, Percent: %.3f, Velocity: %.3f m/s\n",
                  data_position, window_size, diff, percentage_of_window, airflowMps);

    return airflowMps;
}

float FS3000::readMilesPerHour() {
    return (readMetersPerSecond() * 2.2369362912);
}

void FS3000::readData(uint8_t* buffer_in) {
    _i2cPort->requestFrom(FS3000_DEVICE_ADDRESS, 5);
    
    uint8_t i = 0;
    while (_i2cPort->available()) {
        buffer_in[i] = _i2cPort->read();
        i += 1;
    }
    
    Serial.print("FS3000: Raw data: ");
    for (int j = 0; j < 5; j++) {
        Serial.printf("0x%02X ", buffer_in[j]);
    }
    Serial.println();
}

bool FS3000::checksum(uint8_t* data_in, bool debug) {
    uint8_t sum = 0;
    for (int i = 1; i <= 4; i++) {
        sum += uint8_t(data_in[i]);
    }

    if (debug) {
        Serial.print("FS3000: Checksum debug - Data: ");
        for (int i = 0; i < 5; i++) {
            printHexByte(data_in[i]);
            Serial.print(" ");
        }
        Serial.printf("\nFS3000: Sum of data bytes: 0x%02X\n", sum);
    }

    sum %= 256;
    uint8_t calculated_cksum = (~(sum) + 1);
    uint8_t crcbyte = data_in[0];
    uint8_t overall = sum + crcbyte;

    if (debug) {
        Serial.printf("FS3000: Calculated checksum: 0x%02X\n", calculated_cksum);
        Serial.printf("FS3000: Received checksum: 0x%02X\n", crcbyte);
        Serial.printf("FS3000: Overall sum: 0x%02X\n", overall);
    }

    if (overall != 0x00) {
        return false;
    }
    return true;
}

void FS3000::printHexByte(uint8_t x) {
    Serial.print("0x");
    if (x < 16) {
        Serial.print('0');
    }
    Serial.print(x, HEX);
}

void FS3000::printSensorInfo() {
    Serial.print("FS3000 Air Velocity Sensor - I2C Address: 0x");
    Serial.println(FS3000_DEVICE_ADDRESS, HEX);
    Serial.print("Connection Status: ");
    Serial.println(isConnected() ? "Connected" : "Disconnected");
    
    if (isConnected()) {
        uint16_t rawValue = readRaw();
        float velocity = readMetersPerSecond();
        Serial.print("Current Air Velocity: ");
        Serial.print(velocity, 2);
        Serial.println(" m/s");
    }
}

// 互換性のためのメソッド
bool FS3000::readRawData(uint16_t &rawValue) {
    rawValue = readRaw();
    return (rawValue > 0);
}

float FS3000::convertToVelocity(uint16_t rawValue) {
    // 一時的にrawValueを使用して計算
    // 実際の実装では readMetersPerSecond() を使用
    return readMetersPerSecond();
}

bool FS3000::readVelocity(float &velocity) {
    velocity = readMetersPerSecond();
    return true;
}