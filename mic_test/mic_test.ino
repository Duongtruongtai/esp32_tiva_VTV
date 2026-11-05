/*
 * mic_test.ino - I2S microphone test (INMP441 or similar) for MINI ESP32-S3-N16R8
 *
 * Pins taken from xiaozhi-esp32 board "bread-compact-wifi" (main/boards/bread-compact-wifi/config.h):
 *   MIC WS  (LRCL) -> GPIO 4
 *   MIC SCK (BCLK) -> GPIO 5
 *   MIC SD  (DOUT) -> GPIO 6
 *   MIC L/R        -> GND  (left channel, same as the Xiaozhi firmware)
 *   MIC VDD        -> 3V3, GND -> GND
 *
 * Requirements: Arduino IDE + "esp32" board package by Espressif version 3.x
 * Board: "ESP32S3 Dev Module", PSRAM: "OPI PSRAM", Flash Size: 16MB, USB CDC On Boot: Enabled
 *
 * How to use:
 *   - Open Serial Monitor (115200) to see a level bar, or Serial Plotter to see the waveform levels.
 *   - Speak / clap near the mic: the level of the LEFT channel should jump.
 *   - The program reads both I2S slots, so it also tells you if the mic is on the RIGHT slot
 *     (meaning the L/R pin is tied to 3V3 instead of GND).
 */

#include <Arduino.h>
#include "driver/i2s_std.h"

// ---------------- Pin configuration ----------------
#define MIC_WS_PIN   GPIO_NUM_4
#define MIC_SCK_PIN  GPIO_NUM_5
#define MIC_SD_PIN   GPIO_NUM_6

// ---------------- Audio configuration ----------------
#define SAMPLE_RATE      16000   // same input sample rate as the Xiaozhi firmware
#define FRAME_COUNT      256     // stereo frames per read
#define REPORT_MS        100     // how often to print a result
#define PLOTTER_MODE     false   // true = output only numbers for Serial Plotter

static i2s_chan_handle_t rx_handle = nullptr;
static int32_t i2s_buffer[FRAME_COUNT * 2];   // interleaved L, R

static void setup_microphone() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_PIN,
            .ws   = MIC_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_SD_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

// Print a simple bar like: [##########          ]
static void print_bar(const char *label, float level_db) {
    // Map -70 dB .. 0 dB to 0 .. 40 characters
    int width = (int)((level_db + 70.0f) / 70.0f * 40.0f);
    width = constrain(width, 0, 40);
    Serial.printf("%s %6.1f dB [", label, level_db);
    for (int i = 0; i < 40; i++) {
        Serial.print(i < width ? '#' : ' ');
    }
    Serial.print("] ");
}

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println();
    Serial.println("=== I2S MIC TEST (WS=4, SCK=5, SD=6) ===");
    setup_microphone();
    Serial.println("Microphone ready. Speak or clap near the mic...");
}

void loop() {
    static double sum_sq_left = 0, sum_sq_right = 0;
    static int32_t peak_left = 0, peak_right = 0;
    static uint32_t sample_count = 0;
    static uint32_t raw_nonzero = 0;
    static uint32_t last_report = 0;
    static float dc_left = 0, dc_right = 0;   // DC offset trackers

    size_t bytes_read = 0;
    if (i2s_channel_read(rx_handle, i2s_buffer, sizeof(i2s_buffer), &bytes_read, portMAX_DELAY) != ESP_OK) {
        Serial.println("I2S read failed!");
        return;
    }

    int frames = bytes_read / (2 * sizeof(int32_t));
    for (int i = 0; i < frames; i++) {
        int32_t raw_left  = i2s_buffer[2 * i];
        int32_t raw_right = i2s_buffer[2 * i + 1];
        if (raw_left != 0 || raw_right != 0) raw_nonzero++;

        // INMP441 gives 24-bit data left-aligned in 32 bits -> scale to 16-bit range
        float left  = (float)(raw_left  >> 16);
        float right = (float)(raw_right >> 16);

        // Remove DC offset with a slow low-pass filter
        dc_left  += 0.001f * (left  - dc_left);
        dc_right += 0.001f * (right - dc_right);
        left  -= dc_left;
        right -= dc_right;

        sum_sq_left  += left * left;
        sum_sq_right += right * right;
        peak_left  = max(peak_left,  (int32_t)fabsf(left));
        peak_right = max(peak_right, (int32_t)fabsf(right));
        sample_count++;
    }

    if (millis() - last_report < REPORT_MS || sample_count == 0) return;
    last_report = millis();

    float rms_left  = sqrtf(sum_sq_left  / sample_count);
    float rms_right = sqrtf(sum_sq_right / sample_count);
    float db_left   = 20.0f * log10f(rms_left  / 32768.0f + 1e-9f);
    float db_right  = 20.0f * log10f(rms_right / 32768.0f + 1e-9f);

    if (PLOTTER_MODE) {
        Serial.printf("rms_left:%.0f,rms_right:%.0f,peak_left:%d\n", rms_left, rms_right, peak_left);
    } else {
        print_bar("L", db_left);
        if (raw_nonzero == 0) {
            Serial.print("<- NO DATA: check wiring SD=6, SCK=5, WS=4, VDD=3V3");
        } else if (rms_right > rms_left * 4 && rms_right > 20) {
            Serial.print("<- signal is on RIGHT slot: tie mic L/R pin to GND");
        } else if (peak_left > 30000) {
            Serial.print("<- very loud / clipping");
        }
        Serial.println();
    }

    sum_sq_left = sum_sq_right = 0;
    peak_left = peak_right = 0;
    sample_count = 0;
    raw_nonzero = 0;
}
