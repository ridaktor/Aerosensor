#pragma once
#define USE_ADAFRUIT_BME280 1

#include <Arduino.h>
#include <IPAddress.h>

// I2C pins/speed
constexpr int SDA_PIN = 21;
constexpr int SCL_PIN = 22;
constexpr uint32_t I2C_HZ = 100000;  // safe; try 400000 on short clean wiring

// Addresses
constexpr uint8_t MS5525_ADDR  = 0x76; // Holybro/MS5525DSO
constexpr uint8_t ENV_ADDR_PRI = 0x77; // BME/BMP280 preferred (SDO→3V3)
constexpr uint8_t ENV_ADDR_ALT = 0x76; // fallback

// Wi-Fi AP/mDNS
static const char* AP_SSID   = "AeroSensor";
static const char* AP_PASS   = "aero1234";
static const char* MDNS_NAME = "aerosensor";
const IPAddress AP_IP  (192,168,4,1);
const IPAddress AP_GW  (192,168,4,1);
const IPAddress AP_MASK(255,255,255,0);

// Defaults
constexpr float    DEFAULT_RHO    = 1.225f; // only used as fallback
constexpr uint32_t DEFAULT_LOG_MS = 1000;   // 1 s#define FW_VERSION "0.1.0"
