/*
 * speaker_test.ino - I2S speaker test (MAX98357A or similar I2S amplifier) for MINI ESP32-S3-N16R8
 *
 * Pins taken from xiaozhi-esp32 board "bread-compact-wifi" (main/boards/bread-compact-wifi/config.h):
 *   AMP DIN  -> GPIO 7
 *   AMP BCLK -> GPIO 15
 *   AMP LRC  -> GPIO 16
 *   AMP VIN  -> 5V (or 3V3), GND -> GND
 *   AMP SD   -> leave unconnected (or tie to VIN) so the amplifier is enabled
 *   AMP GAIN -> leave unconnected (9 dB)
 *   Speaker (4-8 ohm, 2-3 W) -> AMP "+" and "-"
 *
 * Requirements: Arduino IDE + "esp32" board package by Espressif version 3.x
 * Board: "ESP32S3 Dev Module", PSRAM: "OPI PSRAM", Flash Size: 16MB, USB CDC On Boot: Enabled
 *
 * What you should hear (repeated):
 *   1) A 1 kHz beep
 *   2) A scale Do - Re - Mi - Fa - Sol - La - Si - Do
 *   3) A frequency sweep from 200 Hz to 4 kHz
 * Press the BOOT button (GPIO 0) to change volume (10% -> 25% -> 50%).
 */

#include <Arduino.h>
#include <math.h>
#include "driver/i2s_std.h"

// ---------------- Pin configuration ----------------
#define SPK_DOUT_PIN  GPIO_NUM_7
#define SPK_BCLK_PIN  GPIO_NUM_15
#define SPK_LRCK_PIN  GPIO_NUM_16
#define BOOT_BUTTON   GPIO_NUM_0

// ---------------- Audio configuration ----------------
#define SAMPLE_RATE   24000   // same output sample rate as the Xiaozhi firmware
#define BLOCK_FRAMES  256

static i2s_chan_handle_t tx_handle = nullptr;
static int32_t block_buffer[BLOCK_FRAMES * 2];   // interleaved L, R

static const float volume_levels[] = {0.10f, 0.25f, 0.50f};
static int volume_index = 1;
static float phase = 0.0f;

static void setup_speaker() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_PIN,
            .ws   = SPK_LRCK_PIN,
            .dout = SPK_DOUT_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
}

static void check_button() {
    static bool last_state = HIGH;
    bool state = digitalRead(BOOT_BUTTON);
    if (last_state == HIGH && state == LOW) {
        volume_index = (volume_index + 1) % 3;
        Serial.printf("Volume -> %d%%\n", (int)(volume_levels[volume_index] * 100));
    }
    last_state = state;
}

// Play a tone. If end_freq != start_freq the frequency sweeps linearly.
static void play_tone(float start_freq, float end_freq, int duration_ms) {
    int total_frames = SAMPLE_RATE * duration_ms / 1000;
    int fade_frames = SAMPLE_RATE / 200;   // 5 ms fade in/out to avoid clicks
    int written = 0;

    while (written < total_frames) {
        check_button();
        float amplitude = volume_levels[volume_index] * 2147483647.0f;
        int frames = min(BLOCK_FRAMES, total_frames - written);

        for (int i = 0; i < frames; i++) {
            int n = written + i;
            float freq = start_freq + (end_freq - start_freq) * n / total_frames;
            float envelope = 1.0f;
            if (n < fade_frames) envelope = (float)n / fade_frames;
            if (n > total_frames - fade_frames) envelope = (float)(total_frames - n) / fade_frames;

            int32_t sample = (int32_t)(amplitude * envelope * sinf(phase));
            block_buffer[2 * i]     = sample;   // left
            block_buffer[2 * i + 1] = sample;   // right (same signal, works with any MAX98357A mode)

            phase += 2.0f * PI * freq / SAMPLE_RATE;
            if (phase > 2.0f * PI) phase -= 2.0f * PI;
        }

        size_t bytes_written = 0;
        i2s_channel_write(tx_handle, block_buffer, frames * 2 * sizeof(int32_t), &bytes_written, portMAX_DELAY);
        written += frames;
    }
}

static void play_silence(int duration_ms) {
    memset(block_buffer, 0, sizeof(block_buffer));
    int total_frames = SAMPLE_RATE * duration_ms / 1000;
    while (total_frames > 0) {
        check_button();
        int frames = min(BLOCK_FRAMES, total_frames);
        size_t bytes_written = 0;
        i2s_channel_write(tx_handle, block_buffer, frames * 2 * sizeof(int32_t), &bytes_written, portMAX_DELAY);
        total_frames -= frames;
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(BOOT_BUTTON, INPUT_PULLUP);
    delay(1500);
    Serial.println();
    Serial.println("=== I2S SPEAKER TEST (DOUT=7, BCLK=15, LRCK=16) ===");
    setup_speaker();
    Serial.println("Speaker ready. Press BOOT to change volume.");
}

void loop() {
    Serial.println("1) Beep 1 kHz");
    play_tone(1000, 1000, 500);
    play_silence(400);

    Serial.println("2) Scale Do Re Mi Fa Sol La Si Do");
    const float notes[] = {523.25f, 587.33f, 659.25f, 698.46f, 783.99f, 880.00f, 987.77f, 1046.50f};
    for (float note : notes) {
        play_tone(note, note, 250);
        play_silence(40);
    }
    play_silence(400);

    Serial.println("3) Sweep 200 Hz -> 4 kHz");
    play_tone(200, 4000, 2000);
    play_silence(1500);
}
