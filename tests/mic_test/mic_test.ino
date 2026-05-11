#include <driver/i2s.h>

// --- Safe Pin Definitions ---
#define I2S_WS 26
#define I2S_SCK 25
#define I2S_SD 27

#define I2S_PORT I2S_NUM_0

void i2s_install() {
  const i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,      // Let the ESP32 downsample to 16-bit automatically
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
}

void i2s_setpin() {
  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_set_pin(I2S_PORT, &pin_config);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting 16-bit I2S Mic Test...");
  delay(1000);

  i2s_install();
  i2s_setpin();
  i2s_start(I2S_PORT);
}

void loop() {
  int16_t sample = 0; // Read directly into a standard 16-bit signed integer
  size_t bytes_read = 0;

  esp_err_t result = i2s_read(I2S_PORT, &sample, sizeof(sample), &bytes_read, portMAX_DELAY);

  if (result == ESP_OK && bytes_read > 0) {
    // Print the raw 16-bit value straight to the plotter
    Serial.println(sample);
  }
}