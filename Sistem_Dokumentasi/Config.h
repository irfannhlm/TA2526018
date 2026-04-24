// Config.h
#ifndef CONFIG_H
#define CONFIG_H

// I2C Pins (Shared: LCD, MPU6050, PN532)
#define I2C_SDA 21
#define I2C_SCL 22

// I2S Pins (INMP441)
#define I2S_WS 26
#define I2S_SD 27
#define I2S_SCK 25

// SPI Pins (SD Card Module)
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS 5

// Extra Pins
#define MPU_INT_PIN 32 // External interrupt MPU6050
#define BUZZER_PIN 13 // Pin untuk indikator audio
#define BUTTON_PIN 4  // Button untuk switching mode

// Audio & SD Config
#define SAMPLE_RATE       16000 
#define I2S_READ_LEN      1024 
#define BIT_SHIFT         14    
#define SILENCE_THRESHOLD 300   
#define SILENCE_LIMIT_MS  5000
#define MAX_DURASI_DETIK  300   
#define PRE_BUFFER_SEC    3     
#define PRE_BUFFER_SIZE   (SAMPLE_RATE * PRE_BUFFER_SEC)
#define MAX_RECORD_SECONDS 300
#define MAX_RECORD_MS (MAX_RECORD_SECONDS * 1000UL)
#define DOSEN_COUNTDOWN 8 // Jeda 8 detik

// Accelerometer Config
#define FLYING_THRESHOLD 0.35f  // Di bawah 0.35g dianggap dilempar
#define FLYING_DURATION_MS 100 // Minimal durasi melayang dalam ms

#endif