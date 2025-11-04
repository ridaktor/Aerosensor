#include "SensorMS5525.h"
#include "Config.h"
#include "Shared.h"
#include <Wire.h>

static uint16_t Cprom[8];

static bool i2cWrite(uint8_t b, bool stop=true){
  Wire.beginTransmission(MS5525_ADDR);
  Wire.write(b);
  return Wire.endTransmission(stop)==0;
}

static bool readPROM(){
  constexpr uint8_t CMD_PROM = 0xA0;
  for (int i=0;i<8;i++){
    Wire.beginTransmission(MS5525_ADDR);
    Wire.write(CMD_PROM + 2*i);
    if (Wire.endTransmission(false)!=0) return false;
    if (Wire.requestFrom((uint8_t)MS5525_ADDR,(uint8_t)2,true)!=2) return false;
    Cprom[i] = (uint16_t(Wire.read())<<8) | Wire.read();
  }
  return true;
}

// small helper to tolerate a transient NACK
static bool convert(uint8_t opcode, uint32_t &adc) {
  constexpr uint8_t CMD_ADC = 0x00;
  for (int attempt=0; attempt<2; ++attempt) {
    // Start conversion
    Wire.beginTransmission(MS5525_ADDR);
    Wire.write(opcode);
    if (Wire.endTransmission(true) != 0) continue;

    // OSR4096: worst ~9 ms — be generous
    delay(12);

    // Repeated START, then read 3 bytes
    Wire.beginTransmission(MS5525_ADDR);
    Wire.write(CMD_ADC);
    if (Wire.endTransmission(false) != 0) continue;
    if (Wire.requestFrom((uint8_t)MS5525_ADDR, (uint8_t)3, true) != 3) continue;

    uint8_t b0 = Wire.read(), b1 = Wire.read(), b2 = Wire.read();
    adc = (uint32_t(b0) << 16) | (uint32_t(b1) << 8) | b2;
    return true;
  }
  return false;
}

static void compute_fixed(uint32_t D1, uint32_t D2, float &P_Pa, float &T_C){
  // MS56xx-style integer math (datasheet pattern)
  // dT   = D2 - C5*2^8   (*** correct reference ***)
  // TEMP = 2000 + dT*C6 / 2^23      (0.01 °C)
  // OFF  = C2*2^16 + (C4*dT)/2^7
  // SENS = C1*2^15 + (C3*dT)/2^8
  // P    = (D1*SENS/2^21 - OFF)/2^15   (usually hPa)

  int32_t dT   = (int32_t)D2 - ((int32_t)Cprom[5] * 256L);
  int64_t OFF  = ((int64_t)Cprom[2] << 16) + (((int64_t)Cprom[4] * (int64_t)dT) >> 7);
  int64_t SENS = ((int64_t)Cprom[1] << 15) + (((int64_t)Cprom[3] * (int64_t)dT) >> 8);
  int32_t TEMP = 2000 + (int32_t)(((int64_t)dT * (int64_t)Cprom[6]) >> 23);

  // 2nd-order compensation (cold)
  if (TEMP < 2000) {
    int32_t t2 = (TEMP - 2000);
    OFF  -= (5LL * t2 * (int64_t)t2) >> 1;   // /2
    SENS -= (5LL * t2 * (int64_t)t2) >> 2;   // /4
  }

  int32_t P_raw_hPa = (int32_t)(((((int64_t)D1 * SENS) >> 21) - OFF) >> 15); // nominally hPa
  // Configure the scale in Config.h:
  //   #define MS5525_P_SCALE 100.0f   // if P_raw is hPa → Pa
  //   #define MS5525_P_SCALE   1.0f   // if your variant already yields Pa
#ifndef MS5525_P_SCALE
#define MS5525_P_SCALE 100.0f
#endif

  P_Pa = (float)P_raw_hPa * MS5525_P_SCALE;
  T_C  = (float)TEMP / 100.0f;
}

void sensorBegin() {
  Wire.begin(SDA_PIN, SCL_PIN, I2C_HZ);
  Wire.setTimeOut(50);
  delay(20);

  constexpr uint8_t CMD_RESET = 0x1E;
  if (!i2cWrite(CMD_RESET)) { Serial.println("ERR reset"); while(1){} }
  delay(3);

  if (!readPROM()) { Serial.println("ERR PROM"); while(1){} }
  Serial.println("PROM:");
  for (int i=0;i<8;i++) Serial.printf(" C[%d]=0x%04X\n", i, Cprom[i]);
}

bool sensorReadPT(float &P_Pa, float &T_C) {
  constexpr uint8_t OPC_D1=0x48;  // pressure OSR4096
  constexpr uint8_t OPC_D2=0x58;  // temp    OSR4096
  uint32_t D1=0, D2=0;
  if (!convert(OPC_D1, D1)) return false;
  if (!convert(OPC_D2, D2)) return false;
  compute_fixed(D1, D2, P_Pa, T_C);

  // debug once per second
  static uint32_t next = 0;
  uint32_t now = millis();
  if (now - next < (uint32_t)0x80000000UL) { /* keep unsigned rollover sane */ }
  if (now >= next) {
    next = now + 1000;
    Serial.printf("[MS5525] D1=%lu D2=%lu  T=%.2f C  P=%.1f Pa\n",
                  (unsigned long)D1, (unsigned long)D2, (double)T_C, (double)P_Pa);
  }
  return true;
}

bool doZero(uint16_t ms, uint16_t* outSamples){
  uint32_t t0 = millis(); double acc=0; uint16_t n=0;
  while ((uint32_t)(millis() - t0) < ms) {
    float P,T;
    if (sensorReadPT(P,T)) { acc += P; n++; }
    delay(10);
  }
  if (outSamples) *outSamples = n;
  if (n > 5) { dp_zero = (float)(acc/n); return true; }
  return false;
}