# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a portable CO2 and wind speed monitoring system using M5Stack Core with LTE-M communication via SORACOM platform. The device measures CO2 concentration, temperature, humidity (SCD40 sensor) and wind speed (FS3000 sensor), displaying data on LCD and transmitting to cloud via UDP.

## Development Commands

### Build and Upload
```bash
# Build the project
pio run

# Upload to M5Stack device
pio run --target upload

# Monitor serial output (115200 baud)
pio device monitor --baud 115200

# Build and upload in one command
pio run --target upload && pio device monitor --baud 115200
```

### Device Management
```bash
# List connected devices
pio device list

# Clean build files
pio run --target clean
```

## Architecture

### Hardware Configuration
- **M5Stack Core ESP32**: Main controller with LCD display
- **SCD40 CO2 Sensor**: I2C address 0x62 (CO2, temperature, humidity)
- **FS3000 Wind Sensor**: I2C address 0x28 (air velocity 0-7.23 m/s)  
- **SIM7080 LTE-M Module**: UART connection (TX:15, RX:13)
- **I2C Bus**: SDA=21, SCL=22 (M5Stack Port A)

### Software Stack
- **Platform**: PlatformIO with Arduino framework for ESP32
- **Core Libraries**: M5Stack, SparkFun SCD4x, SparkFun FS3000, TinyGSM
- **Communication**: LTE-M via SORACOM APN, UDP binary data transmission
- **Data Format**: 16-byte binary payload (4 floats: CO2, temp, humidity, wind speed)

### Key Components (src/main.cpp)
- `setup()`: Hardware initialization, sensor setup, modem connection, UDP socket creation
- `readAndSendData()`: Main data collection and transmission cycle  
- `fetchAndUpdateInterval()`: Dynamic interval configuration via SORACOM metadata
- `checkModemStatus()`: Network connection health monitoring
- `resetModem()` / `hardResetModem()`: Connection recovery mechanisms
- `scanI2CDevices()`: I2C device discovery and validation

### Error Handling Strategy
- Exponential backoff with jitter for network retries
- Consecutive failure tracking with automatic modem reset after 3 failures
- Comprehensive modem status checking (signal quality, IP, PDP context)
- Device restart as last resort for unrecoverable failures
- 5-minute communication timeout detection

### Configuration Management  
- Measurement interval controlled via SORACOM metadata (`interval_s` field)
- SORACOM APN: "soracom.io" with credentials "sora"/"sora"
- UDP endpoint: uni.soracom.io:23080
- Serial debugging at 115200 baud with detailed status information

### Data Pipeline
1. Sensor readings every INTERVAL milliseconds (default 10s)
2. Binary data packaging (little-endian floats)
3. UDP transmission with retry logic
4. LCD display update with sensor values and connection status
5. Serial logging for debugging and monitoring

## Development Notes

### Sensor Integration
- SCD40 requires `startPeriodicMeasurement()` after initialization
- FS3000 needs range setting `setRange(AIRFLOW_RANGE_7_MPS)` for proper calibration
- Both sensors provide built-in error checking and validation

### Network Debugging
- Monitor serial output for detailed AT command responses
- Check I2C device scan results at startup (should show 0x28 and 0x62)
- SORACOM console shows data transmission and parsing status
- Signal quality < 5 or > 31 indicates poor cellular reception

### Memory Usage
- Flash: ~33.9% (fits comfortably in 4MB)
- RAM: ~7.2% (adequate headroom for ESP32)