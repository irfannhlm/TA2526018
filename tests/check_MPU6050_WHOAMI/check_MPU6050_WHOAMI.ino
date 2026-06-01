#include <Wire.h>

const int MPU_ADDR_1 = 0x68; // Default MPU I2C address (AD0 low)
const int MPU_ADDR_2 = 0x69; // Alternate MPU I2C address (AD0 high)
const int WHO_AM_I_REG = 0x75;

void setup() {
  Wire.begin();
  Serial.begin(115200);
  while (!Serial); // Wait for serial monitor to open

  Serial.println("\n--- MPU6050 Diagnostic Tool ---");

  // Step 1: Scan for I2C devices
  Serial.println("Scanning I2C bus...");
  int mpuAddress = scanI2C();

  if (mpuAddress == 0) {
    Serial.println("Error: No I2C devices found.");
    Serial.println("Check your wiring: SDA -> A4, SCL -> A5 (on Uno/Nano)");
    return;
  }

  // Step 2: Check WHO_AM_I register
  checkWhoAmI(mpuAddress);
}

void loop() {
  // Run once, halt here. Press Reset on the Arduino to run again.
}

int scanI2C() {
  int foundAddress = 0;
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("Device found at I2C address 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);

      // Prioritize standard MPU addresses, but fallback to whatever is found
      if (address == MPU_ADDR_1 || address == MPU_ADDR_2) {
        foundAddress = address;
      } else if (foundAddress == 0) {
         foundAddress = address; 
      }
    }
  }
  return foundAddress;
}

void checkWhoAmI(int address) {
  Serial.print("\nReading WHO_AM_I register (0x75) from address 0x");
  Serial.println(address, HEX);

  Wire.beginTransmission(address);
  Wire.write(WHO_AM_I_REG);
  byte error = Wire.endTransmission(false); // Send restart condition

  if (error != 0) {
    Serial.println("Error: Failed to communicate with the device.");
    return;
  }

  Wire.requestFrom(address, 1, true);
  if (Wire.available()) {
    byte whoAmI = Wire.read();
    Serial.print("WHO_AM_I Value: 0x");
    if (whoAmI < 16) Serial.print("0");
    Serial.print(whoAmI, HEX);
    Serial.print(" (Decimal: ");
    Serial.print(whoAmI);
    Serial.println(")");

    // Step 3: Analyze the result
    analyzeChip(whoAmI);
  } else {
    Serial.println("Error: No data returned from register 0x75.");
  }
}

void analyzeChip(byte whoAmI) {
  Serial.println("\n--- Diagnosis ---");
  switch (whoAmI) {
    case 0x68:
      Serial.println("Result: Genuine MPU-6050 (or a 1:1 hardware clone).");
      break;
    case 0x70:
      Serial.println("Result: FAKE. This is an MPU-6500 relabeled as an MPU-6050.");
      Serial.println("Note: MPU6050 libraries may behave erratically with this chip.");
      break;
    case 0x71:
      Serial.println("Result: MPU-9250. (Contains an MPU-6500 internally).");
      break;
    case 0x73:
      Serial.println("Result: MPU-9255.");
      break;
    case 0x98:
      Serial.println("Result: FAKE. This is an ICM-20689 relabeled as an MPU-6050.");
      break;
    case 0x12:
      Serial.println("Result: ICM-20602.");
      break;
    case 0xEA:
      Serial.println("Result: ICM-20690.");
      break;
    default:
      Serial.println("Result: Unknown chip or heavily modified clone.");
      break;
  }
}