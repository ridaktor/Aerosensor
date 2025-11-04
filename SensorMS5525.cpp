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
    Wire.beginTransmission(MS5525_ADDR); Wire.write(CMD_PROM + 2*i);
    if (Wire.endTransmission(false)!=0) return false;
    if (Wire.requestFrom((uint8_t)MS5525_ADDR,(uint8_t)2,true)!=2) return false;
    Cprom[i] = (uint16_t(Wire.read())<<8) | Wire.read();
  }
  return true;
}
static bool convert(uint8_t opcode, uint32_t &adc) {
  constexpr uint8_t CMD_ADC = 0x00;

  // Start conversion
  Wire.beginTransmission(MS5525_ADDR);
  Wire.write(opcode);
  if (Wire.endTransmission(true) != 0) return false;
  delay(20); // OSR4096 worst ~9ms; generous

  // Repeated START, then read 3 bytes
  Wire.beginTransmission(MS5525_ADDR);
  Wire.write(CMD_ADC);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)MS5525_ADDR, (uint8_t)3, true) != 3) return false;

  uint8_t b0 = Wire.read(), b1 = Wire.read(), b2 = Wire.read();
  adc = (uint32_t(b0) << 16) | (uint32_t(b1) << 8) | b2;
  return true;
}
static void compute_fixed(uint32_t D1, uint32_t D2, float &P_Pa, float &T_C){
  // Variant uses C5 * 128 (important)
  int32_t dT   = (int32_t)D2 - ((int32_t)Cprom[5] * 128L);
  int64_t OFF  = (int64_t)Cprom[2] * 65536LL + ((int64_t)Cprom[4] * (int64_t)dT) / 128LL;
  int64_t SENS = (int64_t)Cprom[1] * 32768LL + ((int64_t)Cprom[3] * (int64_t)dT) / 256LL;
  int32_t TEMP = 2000 + ( ((int64_t)dT * (int64_t)Cprom[6]) / 8388608LL );
  if (TEMP < 2000) { int64_t d = (TEMP - 2000); OFF -= 5LL*d*d/2; SENS -= 5LL*d*d/4; }
  int32_t P = (int32_t)((((int64_t)D1 * SENS)/2097152LL - OFF)/32768LL);
  P_Pa = (float)P; T_C  = (float)TEMP/100.0f;
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
  return true;
}

bool doZero(uint16_t ms, uint16_t* outSamples){
  uint32_t t0 = millis(); double acc=0; uint16_t n=0;
  while (millis() - t0 < ms) {
    float P,T;
    if (sensorReadPT(P,T)) { acc += P; n++; }
    delay(10);
  }
  if (outSamples) *outSamples = n;
  if (n > 5) { dp_zero = (float)(acc/n); return true; }
  return false;
}