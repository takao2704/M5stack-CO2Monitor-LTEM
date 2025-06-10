#ifndef FS3000_H
#define FS3000_H

#include <Wire.h>
#include <Arduino.h>

#define FS3000_DEVICE_ADDRESS 0x28
#define AIRFLOW_RANGE_7_MPS 0x00
#define AIRFLOW_RANGE_15_MPS 0x01

class FS3000 {
private:
    TwoWire* _i2cPort;
    uint8_t _buff[5];
    uint8_t _range = AIRFLOW_RANGE_7_MPS;
    float _mpsDataPoint[13] = {0, 1.07, 2.01, 3.00, 3.97, 4.96, 5.98, 6.99, 7.23};
    int _rawDataPoint[13] = {409, 915, 1522, 2066, 2523, 2908, 3256, 3572, 3686};
    
    void readData(uint8_t* buffer_in);
    bool checksum(uint8_t* data_in, bool debug = false);
    void printHexByte(uint8_t x);
    
public:
    FS3000();
    bool begin(TwoWire &wirePort = Wire);
    bool isConnected();
    uint16_t readRaw();
    float readMetersPerSecond();
    float readMilesPerHour();
    void setRange(uint8_t range);
    
    // デバッグ用
    void printSensorInfo();
    
    // 互換性のため
    bool readRawData(uint16_t &rawValue);
    float convertToVelocity(uint16_t rawValue);
    bool readVelocity(float &velocity);
};

#endif