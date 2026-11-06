/*
 * voice_assistant.ino - "Tiva" Robot Lab Tour Guide voice client
 * Board: MINI ESP32-S3-N16R8 (XH-S3E-AI), Arduino core esp32 3.x
 *
 * Flow:  say "Hello Tiva" / "Chao Tiva" (or hold BOOT) -> chime -> conversation starts
 *        -> ask as many questions as you like (no wake word needed between them)
 *        -> say "OK Tiva" / "Cam on Tiva" -> robot says goodbye and sleeps again
 *
 * Wake/stop phrases run 100% on the chip (Espressif ESP-SR MultiNet, free).
 *
 * Hardware:
 *   Mic         : WS=4, SCK=5, SD=6 (left channel)
 *   Amplifier   : DIN=7, BCLK=15, LRC=16
 *   OLED 0.96"  : SDA=41, SCL=42 (SSD1306 128x64 I2C)
 *   BOOT button : GPIO 0 -> click = volume up, double click = volume down,
 *                 hold 1 s = talk without wake word / stop the robot while it is talking
 *   Volume keys : GPIO 40 = up, GPIO 39 = down (optional external buttons to GND)
 *   RGB LED     : GPIO 48
 *
 * Arduino IDE -> Tools:
 *   Board: ESP32S3 Dev Module        Flash Size: 16MB (128Mb)
 *   PSRAM: OPI PSRAM                 USB CDC On Boot: Enabled
 *   Partition Scheme: ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)   <-- required for the wake word
 * Library: "U8g2" by oliver
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Preferences.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <U8g2lib.h>
#include "driver/i2s_std.h"
#include "ESP_SR.h"   // also makes the IDE upload the speech models (srmodels.bin)
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "esp_mn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

// Declared here, before any function, so the Arduino IDE's automatic prototypes cannot clash with it
extern "C" char *flite_g2p(const char *graphemes, int map_phonemes);

// ===================== USER SETTINGS =====================
// Known Wi-Fi networks: the robot connects to whichever is available (strongest first).
// Networks added from the phone setup page are tried too, so you rarely need to edit this.
struct KnownWifi { const char *ssid; const char *password; };
static const KnownWifi KNOWN_WIFI[] = {
    {"YOUR_WIFI_NAME",   "YOUR_WIFI_PASSWORD"},
    {"YOUR_WIFI_NAME_2", "YOUR_WIFI_PASSWORD_2"},
};
// If no network works (or BOOT is held while powering on), the robot opens its own
// Wi-Fi "Tiva-Setup": join it with a phone, a page opens, pick the new network.
const char *SETUP_AP_NAME     = "Tiva-Setup";
const char *SETUP_AP_PASSWORD = "12345678";    // at least 8 characters
#define WIFI_CONNECT_TIMEOUT_MS 20000          // try known networks this long before opening the setup Wi-Fi
#define SETUP_PORTAL_TIMEOUT_MS 300000         // close the setup Wi-Fi after 5 minutes and try again

// Server addresses, tried in order at start-up; the first one that answers is used.
// Inside the lab, the public address often does NOT work (the router cannot loop back to
// itself), so also put the LAN address of the server PC here (run "ipconfig" on that PC).
static const char *SERVER_CANDIDATES[] = {
    "http://YOUR_SERVER_IP:8009",      // public address (works from outside the lab)
    // "http://192.168.1.100:8009",    // LAN address of the server PC: fill in and remove the //
};
static String SERVER_BASE = SERVER_CANDIDATES[0];
const char *INTERACT_PATH = "/api/v1/voice/interact";
const char *HEALTH_PATH   = "/api/v1/health";
const char *VOICE_NAME    = "";        // empty = server default voice
const char *ROBOT_NAME    = "Tiva";
const char *TTS_PATH      = "/api/v1/tts";        // optional text-to-speech endpoint on the server
const char *CHAT_PATH     = "/api/v1/chat/text";  // used if /api/v1/tts does not exist

// Spoken when the conversation ends ("OK Tiva" / "Cam on Tiva"). The robot asks the server
// to say it once with its normal voice and keeps the sound in flash memory.
const char *GOODBYE_TEXT  = "Tạm biệt bạn nhé! Nếu cần Tiva hỗ trợ gì, bạn cứ gọi Chào Tiva nha.";

// ---- Wake word ----
#define USE_WAKE_WORD      true        // false = old behaviour (start recording whenever someone speaks)
// Wake phrases: { name shown in the log, sounds (ESP-SR phoneme letters, one group per word) }.
// Two-word phrases are much more reliable than a single word.
// Phoneme letters: T=t  m=ee(i)  V=v  n=a(ah, open)  c=uh/ơ  h=h  d=ay(ây)  i=ai  L=l  b=oh(ô)
//                  K=k  p=ch  t=ao  e=aw(o)  o=oo(u)  a=a(ae)  N=n  M=m  S=s  W=w  Y=y  g=ih  f=eh(e)
// Example: "Tiva" = T + m + V + n -> "TmVn"
#define PHRASE_START 0               // wakes the robot up and starts a conversation
#define PHRASE_STOP  1               // ends the conversation
struct WakePhrase { const char *text; const char *phonemes; int kind; };
static const WakePhrase WAKE_PHRASES[] = {
    {"hello tiva",  "hcLb TmVn",   PHRASE_START},   // "Hello Tiva"
    {"chao tiva",   "pt TmVn",     PHRASE_START},   // "Chào Tiva"
    {"ok tiva",     "bKd TmVn",    PHRASE_STOP},    // "OK Tiva"
    {"cam on tiva", "KnM cN TmVn", PHRASE_STOP},    // "Cảm ơn Tiva"
};
#define WAKE_HI_ESP        false       // true = also accept Espressif's built-in "Hi ESP"
#define WAKE_MIN_PROB      0.0f        // 0.0 .. 1.0: raise (e.g. 0.3) if the robot wakes up by mistake
#define WAKE_CHIME         true        // play a "ting" after the wake word
#define WAIT_SPEECH_MS     6000        // after the wake word, wait this long for the question
#define CONVERSATION_IDLE_MS 60000     // safety: go back to sleep if nobody asks anything for 60 s (0 = never)
#define CONVERSATION_STRICT  1.6f      // in a conversation, a question must be this much louder than room noise

// ---- Audio / behaviour ----
#define DEFAULT_VOLUME     100         // percent, used the first time (then the saved value is used)
#define MIN_VOLUME         10
#define MAX_VOLUME         200         // above 100% = digital boost
#define VOLUME_STEP        10
#define MIC_GAIN_SHIFT     15          // smaller = louder mic (14 = x2 louder, 16 = x2 quieter)
#define VAD_RATIO          2.5f        // speech must be this many times louder than background noise
#define VAD_MIN_RMS        400.0f      // absolute minimum level to count as speech
#define END_SILENCE_MS     900         // stop recording after this much silence
#define MIN_SPEECH_MS      400         // ignore sounds shorter than this
#define MAX_RECORD_SECONDS 10
#define HTTP_TIMEOUT_MS    65000       // server (STT + LLM + TTS) can be slow

#define OLED_SH1106        false       // set true if your OLED is a 1.3" SH1106
#define OLED_FLIP          false       // set true to rotate the picture 180 degrees
#define CAPTION_FONT       u8g2_font_unifont_t_vietnamese1
// =========================================================

#if USE_WAKE_WORD && !defined(ARDUINO_PARTITION_esp_sr_16) && !defined(ARDUINO_PARTITION_esp_sr_32) && !defined(ARDUINO_PARTITION_esp_sr_8)
#error "Wake word needs: Tools -> Partition Scheme -> ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)"
#endif

// ---------------- Pins ----------------
#define MIC_WS_PIN    GPIO_NUM_4
#define MIC_SCK_PIN   GPIO_NUM_5
#define MIC_SD_PIN    GPIO_NUM_6
#define SPK_DOUT_PIN  GPIO_NUM_7
#define SPK_BCLK_PIN  GPIO_NUM_15
#define SPK_LRCK_PIN  GPIO_NUM_16
#define OLED_SDA_PIN  41
#define OLED_SCL_PIN  42
#define BOOT_BUTTON   0
#define VOL_UP_PIN    40               // set to -1 if not used
#define VOL_DOWN_PIN  39               // set to -1 if not used
#define RGB_LED_PIN   48

// ---------------- Audio constants ----------------
#define MIC_SAMPLE_RATE   16000
#define FRAME_SAMPLES     320                  // 20 ms per frame
#define FRAME_MS          20
#define PRE_ROLL_FRAMES   15                   // keep 300 ms before speech starts
#define START_FRAMES      3                    // 60 ms of loud sound triggers recording (no-wake-word mode)
#define MAX_RECORD_SAMPLES (MAX_RECORD_SECONDS * MIC_SAMPLE_RATE)
#define WAKE_PHRASE_COUNT (sizeof(WAKE_PHRASES) / sizeof(WAKE_PHRASES[0]))

// ===================== Types (declared before any function for the Arduino preprocessor) =====================
enum FaceState { FACE_BOOT, FACE_WIFI, FACE_SETUP, FACE_LISTENING, FACE_RECORDING, FACE_THINKING, FACE_SPEAKING, FACE_ERROR };
enum ButtonEvent { BTN_NONE, BTN_CLICK, BTN_DOUBLE_CLICK, BTN_HOLD };
enum Trigger { TRIGGER_NONE, TRIGGER_WAKE_WORD, TRIGGER_STOP_WORD, TRIGGER_BUTTON, TRIGGER_VOICE };

// ===================== Globals =====================
static i2s_chan_handle_t rx_handle = nullptr;
static i2s_chan_handle_t tx_handle = nullptr;
static uint32_t speaker_rate = 0;

static int32_t mic_raw[FRAME_SAMPLES * 2];     // stereo 32-bit from I2S
static int16_t mic_frame[FRAME_SAMPLES];
static int16_t pre_roll[PRE_ROLL_FRAMES][FRAME_SAMPLES];
static int pre_head = 0, pre_filled = 0;
static float dc_offset = 0.0f;
static float noise_floor = 300.0f;

// Multipart upload buffer (in PSRAM): [prefix][WAV header][PCM ...][tail]
static const char *BOUNDARY = "----TivaRobotBoundary7MA4YWxk";
static String multipart_prefix;
static String multipart_tail;
static uint8_t *upload_buffer = nullptr;
static size_t pcm_offset = 0;

static Preferences prefs;

// Shared with the display task
static volatile FaceState face_state = FACE_BOOT;
static const char *volatile listen_caption = "Mời bạn nói";
static const char *volatile record_caption = "Đang nghe...";
static const char *volatile speak_caption = "Đang trả lời";
static volatile uint8_t mic_level = 0;          // 0..10
static volatile uint8_t mouth_level = 0;        // 0..10
static volatile int volume_percent = DEFAULT_VOLUME;
static volatile uint32_t volume_overlay_until = 0;

// Wake word engine (ESP-SR)
static bool wake_ready = false;
static bool wakenet_enabled = false;
static srmodel_list_t *sr_models = nullptr;
static const esp_afe_sr_iface_t *afe_handle = nullptr;
static esp_afe_sr_data_t *afe_data = nullptr;
static esp_mn_iface_t *multinet = nullptr;
static model_iface_data_t *mn_data = nullptr;
static int16_t *afe_feed_buf = nullptr;
static int afe_feed_chunk = 0;
static int afe_feed_fill = 0;
static volatile bool wake_listening = false;     // main loop is feeding audio to the engine
static volatile bool wake_reset_request = false; // ask the wake task to clear old audio
static volatile int wake_event = 0;              // 0 = none, 1 = Tiva phrase, 2 = Hi ESP
static volatile int wake_accept_kind = PHRASE_START; // which phrases count right now
static volatile int wake_phrase_id = -1;
static volatile float wake_prob = 0.0f;

static bool conversation_active = false;
static uint32_t last_activity = 0;

// ===================== LED & state =====================
static void set_led(uint8_t r, uint8_t g, uint8_t b) {
    rgbLedWrite(RGB_LED_PIN, r, g, b);
}

static void set_state(FaceState state) {
    face_state = state;
    switch (state) {
        case FACE_BOOT:      set_led(10, 10, 10); break;
        case FACE_WIFI:      set_led(40, 0, 0);   break;
        case FACE_SETUP:     set_led(0, 30, 30);  break;
        case FACE_LISTENING: set_led(0, 0, 20);   break;
        case FACE_RECORDING: set_led(0, 40, 0);   break;
        case FACE_THINKING:  set_led(30, 20, 0);  break;
        case FACE_SPEAKING:  set_led(25, 0, 30);  break;
        case FACE_ERROR:     set_led(40, 0, 0);   break;
    }
}

// ===================== OLED face =====================
#if OLED_SH1106
static U8G2_SH1106_128X64_NONAME_F_HW_I2C display(OLED_FLIP ? U8G2_R2 : U8G2_R0, U8X8_PIN_NONE);
#else
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(OLED_FLIP ? U8G2_R2 : U8G2_R0, U8X8_PIN_NONE);
#endif

static const int EYE_X[2] = {40, 88};   // eye centers
static const int EYE_Y = 18;

static void draw_caption(const char *text) {
    display.setFont(CAPTION_FONT);
    int width = display.getUTF8Width(text);
    display.drawUTF8((128 - width) / 2, 62, text);
}

static void draw_round_eyes(int height, bool blink) {
    for (int i = 0; i < 2; i++) {
        if (blink) {
            display.drawBox(EYE_X[i] - 10, EYE_Y - 1, 20, 3);
        } else {
            display.drawRBox(EYE_X[i] - 10, EYE_Y - height / 2, 20, height, 6);
        }
    }
}

static void draw_happy_eyes() {   // ^  ^
    for (int i = 0; i < 2; i++) {
        for (int r = 7; r <= 9; r++) {
            display.drawCircle(EYE_X[i], EYE_Y + 4, r, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
        }
    }
}

static void draw_smile() {
    for (int r = 10; r <= 12; r++) {
        display.drawCircle(64, 30, r, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
    }
}

static void draw_frown() {
    for (int r = 8; r <= 9; r++) {
        display.drawCircle(64, 44, r, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    }
}

static void draw_volume_overlay() {
    char text[16];
    int vol = volume_percent;
    display.setFont(CAPTION_FONT);
    const char *title = "Âm lượng";
    display.drawUTF8((128 - display.getUTF8Width(title)) / 2, 16, title);
    display.drawRFrame(4, 24, 120, 16, 4);
    int fill = 116 * (vol - MIN_VOLUME) / (MAX_VOLUME - MIN_VOLUME);
    if (fill > 0) display.drawRBox(6, 26, max(fill, 8), 12, 3);
    snprintf(text, sizeof(text), "%d%%", vol);
    display.drawUTF8((128 - display.getUTF8Width(text)) / 2, 60, text);
}

static void draw_face(uint32_t frame) {
    display.clearBuffer();

    if ((int32_t)(volume_overlay_until - millis()) > 0) {
        draw_volume_overlay();
        display.sendBuffer();
        return;
    }

    bool blink = (frame % 40) < 2;   // blink about every 3 seconds
    int dots = (frame / 4) % 4;
    char caption[40];

    switch (face_state) {
        case FACE_BOOT:
            draw_round_eyes(24, blink);
            draw_smile();
            snprintf(caption, sizeof(caption), "%s xin chào!", ROBOT_NAME);
            draw_caption(caption);
            break;

        case FACE_WIFI:
            draw_round_eyes(3, true);                     // sleepy eyes
            display.drawHLine(58, 38, 12);
            snprintf(caption, sizeof(caption), "Kết nối WiFi%.*s", dots, "...");
            draw_caption(caption);
            break;

        case FACE_LISTENING:
            draw_round_eyes(24, blink);
            draw_smile();
            draw_caption(listen_caption);
            break;

        case FACE_RECORDING:
            draw_round_eyes(28, false);                   // wide open eyes
            display.drawCircle(64, 40, 5);
            display.drawCircle(64, 40, 4);
            for (int i = 0; i < 3; i++) {                 // sound bars follow the microphone
                int h = constrain(mic_level * 4 - i * 8, 2, 30);
                display.drawBox(4 + i * 6, EYE_Y - h / 2, 3, h);
                display.drawBox(121 - i * 6, EYE_Y - h / 2, 3, h);
            }
            draw_caption(record_caption);
            break;

        case FACE_THINKING: {
            int look = ((frame / 6) % 2) ? 0 : -4;       // pupils move left and right
            for (int i = 0; i < 2; i++) {
                display.drawRFrame(EYE_X[i] - 10, EYE_Y - 12, 20, 24, 6);
                display.drawRFrame(EYE_X[i] - 9, EYE_Y - 11, 18, 22, 5);
                display.drawBox(EYE_X[i] + 1 + look, EYE_Y - 9, 6, 8);
            }
            display.drawHLine(56, 40, 16);
            display.drawHLine(56, 41, 16);
            snprintf(caption, sizeof(caption), "Suy nghĩ%.*s", dots, "...");
            draw_caption(caption);
            break;
        }

        case FACE_SPEAKING: {
            draw_happy_eyes();
            int open = 1 + mouth_level * 7 / 10;          // lip sync with the voice
            display.drawFilledEllipse(64, 38, 11, open);
            draw_caption(speak_caption);
            break;
        }

        case FACE_SETUP:
            display.setFont(u8g2_font_6x12_tf);
            display.drawStr(0, 11, "Cai dat WiFi:");
            display.drawStr(0, 25, "1. Dien thoai vao WiFi");
            display.drawStr(0, 37, SETUP_AP_NAME);
            display.drawStr(0, 49, "   mat khau 12345678");
            display.drawStr(0, 62, "2. Mo 192.168.4.1");
            break;

        case FACE_ERROR:
            for (int i = 0; i < 2; i++) {                 // X X eyes
                for (int t = 0; t < 2; t++) {
                    display.drawLine(EYE_X[i] - 8 + t, 10, EYE_X[i] + 8 + t, 26);
                    display.drawLine(EYE_X[i] - 8 + t, 26, EYE_X[i] + 8 + t, 10);
                }
            }
            draw_frown();
            draw_caption("Có lỗi rồi!");
            break;
    }
    display.sendBuffer();
}

static void display_task(void *arg) {
    uint32_t frame = 0;
    while (true) {
        draw_face(frame++);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

static void setup_display() {
    Wire.setPins(OLED_SDA_PIN, OLED_SCL_PIN);
    display.setBusClock(400000);
    display.begin();
    display.clearBuffer();
    display.sendBuffer();
    xTaskCreatePinnedToCore(display_task, "display", 4096, nullptr, 1, nullptr, 0);
}

// ===================== I2S setup =====================
static void setup_microphone() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 320;    // 160 ms of buffering, survives short CPU spikes
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
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

static void setup_speaker(uint32_t sample_rate) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   // send silence when no data, avoids buzzing after playback
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
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
    speaker_rate = sample_rate;
}

static void set_speaker_rate(uint32_t sample_rate) {
    if (sample_rate == speaker_rate) return;
    i2s_channel_disable(tx_handle);
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
    i2s_channel_enable(tx_handle);
    speaker_rate = sample_rate;
}

// Converts one 16-bit sample to a 32-bit I2S sample with the current volume (clipped)
static inline int32_t apply_volume(int32_t sample) {
    int32_t value = sample * volume_percent / 100;
    if (value > 32767) value = 32767;
    if (value < -32767) value = -32767;
    return value << 16;
}

static void play_silence(int duration_ms) {
    static int32_t zeros[256 * 2] = {0};
    int frames_left = speaker_rate * duration_ms / 1000;
    while (frames_left > 0) {
        int frames = min(frames_left, 256);
        size_t written = 0;
        i2s_channel_write(tx_handle, zeros, frames * 2 * sizeof(int32_t), &written, portMAX_DELAY);
        frames_left -= frames;
    }
}

// Plays a short tone with fade in/out (no clicks)
static void play_tone(float freq, int duration_ms, int amplitude = 8000) {
    static int32_t buf[256 * 2];
    int total = speaker_rate * duration_ms / 1000;
    int fade = speaker_rate / 200;   // 5 ms
    float phase = 0;
    int done = 0;
    while (done < total) {
        int frames = min(total - done, 256);
        for (int i = 0; i < frames; i++) {
            int n = done + i;
            float env = 1.0f;
            if (n < fade) env = (float)n / fade;
            if (n > total - fade) env = (float)(total - n) / fade;
            int32_t out = apply_volume((int32_t)(amplitude * env * sinf(phase)));
            buf[2 * i] = out;
            buf[2 * i + 1] = out;
            phase += 2.0f * PI * freq / speaker_rate;
            if (phase > 2.0f * PI) phase -= 2.0f * PI;
        }
        size_t written = 0;
        i2s_channel_write(tx_handle, buf, frames * 2 * sizeof(int32_t), &written, portMAX_DELAY);
        done += frames;
    }
}

static void play_wake_chime() {      // "ting-ting" going up
    play_tone(880, 70);
    play_tone(1320, 90);
    play_silence(40);
}

static void play_goodbye_sound() {   // "ting-tong" going down
    play_tone(1320, 90);
    play_tone(880, 140);
    play_silence(40);
}

static void play_error_sound() {     // two tones going down
    play_tone(520, 150);
    play_tone(370, 220);
    play_silence(40);
}

// ===================== Buttons & volume =====================
// Non-blocking BOOT button reader: call it often (every ~20 ms)
static ButtonEvent poll_boot_button() {
    static bool last = HIGH;
    static uint32_t press_time = 0, release_time = 0;
    static int clicks = 0;
    static bool hold_sent = false;

    bool state = digitalRead(BOOT_BUTTON);
    uint32_t now = millis();

    if (last == HIGH && state == LOW) {
        press_time = now;
        hold_sent = false;
    }
    if (state == LOW && !hold_sent && now - press_time > 1000) {
        hold_sent = true;
        clicks = 0;
        last = state;
        return BTN_HOLD;
    }
    if (last == LOW && state == HIGH && !hold_sent && now - press_time > 30) {
        clicks++;
        release_time = now;
    }
    last = state;

    if (clicks > 0 && state == HIGH && now - release_time > 350) {
        int count = clicks;
        clicks = 0;
        return count >= 2 ? BTN_DOUBLE_CLICK : BTN_CLICK;
    }
    return BTN_NONE;
}

// Returns +1 / -1 when an external volume button is pressed
static int poll_volume_keys() {
    static bool last_up = HIGH, last_down = HIGH;
    int result = 0;
    if (VOL_UP_PIN >= 0) {
        bool s = digitalRead(VOL_UP_PIN);
        if (last_up == HIGH && s == LOW) result = +1;
        last_up = s;
    }
    if (VOL_DOWN_PIN >= 0) {
        bool s = digitalRead(VOL_DOWN_PIN);
        if (last_down == HIGH && s == LOW) result = -1;
        last_down = s;
    }
    return result;
}

static void change_volume(int direction, bool beep) {
    int vol = constrain(volume_percent + direction * VOLUME_STEP, MIN_VOLUME, MAX_VOLUME);
    volume_percent = vol;
    volume_overlay_until = millis() + 1500;
    prefs.putInt("volume", vol);
    Serial.printf("[VOLUME] %d%%\n", vol);
    if (beep) {
        play_tone(1000, 80);
        play_silence(30);
    }
}

// Handles all buttons. Returns true if BOOT was held for 1 second.
static bool handle_buttons(bool beep) {
    int key = poll_volume_keys();
    if (key != 0) change_volume(key, beep);

    switch (poll_boot_button()) {
        case BTN_CLICK:        change_volume(+1, beep); break;
        case BTN_DOUBLE_CLICK: change_volume(-1, beep); break;
        case BTN_HOLD:         return true;
        default: break;
    }
    return false;
}

// ===================== Microphone =====================
// Reads one 20 ms frame (left channel) into out[], returns its RMS level
static float read_mic_frame(int16_t *out) {
    size_t bytes_read = 0;
    i2s_channel_read(rx_handle, mic_raw, sizeof(mic_raw), &bytes_read, portMAX_DELAY);
    int frames = bytes_read / (2 * sizeof(int32_t));

    double sum_sq = 0;
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        float value = (i < frames) ? (float)(mic_raw[2 * i] >> MIC_GAIN_SHIFT) : 0.0f;
        dc_offset += 0.0005f * (value - dc_offset);
        value -= dc_offset;
        if (value > 32767) value = 32767;
        if (value < -32767) value = -32767;
        out[i] = (int16_t)value;
        sum_sq += value * value;
    }
    float rms = sqrtf(sum_sq / FRAME_SAMPLES);
    mic_level = (uint8_t)constrain((int)(rms / 400), 0, 10);
    return rms;
}

// Discard old audio (after playback, a beep or a long HTTP request)
static void flush_mic(int duration_ms) {
    for (int t = 0; t < duration_ms; t += FRAME_MS) {
        read_mic_frame(mic_frame);
    }
}

static void calibrate_noise() {
    float sum = 0;
    flush_mic(200);
    for (int i = 0; i < 50; i++) {
        sum += read_mic_frame(mic_frame);
    }
    noise_floor = sum / 50;
    Serial.printf("[MIC] Background noise level: %.0f\n", noise_floor);
}

// Re-measures the room noise (e.g. after the robot has spoken), 300 ms
static void update_noise_floor() {
    float sum = 0;
    for (int i = 0; i < 15; i++) {
        sum += read_mic_frame(mic_frame);
    }
    float measured = sum / 15;
    noise_floor = max(measured, noise_floor * 0.5f);
    Serial.printf("[MIC] Room noise now: %.0f (speech threshold %.0f)\n", noise_floor, max(noise_floor * VAD_RATIO, VAD_MIN_RMS));
}

static float speech_threshold() {
    return max(noise_floor * VAD_RATIO, VAD_MIN_RMS);
}

// ===================== Wake word engine (ESP-SR) =====================
// The wake phrases above come with ready-made phonemes, so the big English
// text-to-phoneme dictionary (about 560 KB) is not needed. This small replacement
// keeps it out of the firmware so everything fits in the 3 MB app partition.
extern "C" char *flite_g2p(const char *graphemes, int map_phonemes) {
    (void)graphemes;
    (void)map_phonemes;
    return nullptr;
}
static void wake_task(void *arg) {
    while (true) {
        if (wake_reset_request) {
            afe_handle->reset_buffer(afe_data);
            multinet->clean(mn_data);
            if (wakenet_enabled) afe_handle->enable_wakenet(afe_data);
            wake_reset_request = false;
        }

        if (!wake_listening) {
            vTaskDelay(pdMS_TO_TICKS(20));   // main loop is recording / talking: nothing to do
            continue;
        }

        afe_fetch_result_t *res = afe_handle->fetch_with_delay(afe_data, pdMS_TO_TICKS(100));
        if (res == nullptr || res->ret_value == ESP_FAIL || res->data == nullptr) {
            continue;
        }
        if (!wake_listening || wake_event != 0) {
            continue;   // a wake word was already reported: ignore the rest
        }

        if (wakenet_enabled && res->wakeup_state == WAKENET_DETECTED && wake_accept_kind == PHRASE_START) {
            wake_prob = 1.0f;
            wake_phrase_id = -1;
            wake_event = 2;
            continue;
        }

        esp_mn_state_t state = multinet->detect(mn_data, res->data);
        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *result = multinet->get_results(mn_data);
            if (result->num > 0) {
                int index = result->command_id[0] - 1;   // command ids start at 1
                bool right_kind = index >= 0 && index < (int)WAKE_PHRASE_COUNT && WAKE_PHRASES[index].kind == wake_accept_kind;
                if (right_kind && result->prob[0] >= WAKE_MIN_PROB) {
                    wake_prob = result->prob[0];
                    wake_phrase_id = result->command_id[0];
                    wake_event = 1;
                } else if (right_kind) {
                    Serial.printf("[WAKE] Ignored \"%s\" (prob %.2f < %.2f)\n", result->string, result->prob[0], WAKE_MIN_PROB);
                }
            }
            multinet->clean(mn_data);
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            multinet->clean(mn_data);   // start a new listening window
        }
    }
}

static bool setup_wake_word() {
#if USE_WAKE_WORD
    Serial.println("[WAKE] Loading speech models...");
    sr_models = esp_srmodel_init("model");
    if (sr_models == nullptr || sr_models->num == 0) {
        Serial.println("[WAKE] ERROR: no speech models found. Check Tools -> Partition Scheme -> ESP SR 16M");
        return false;
    }
    for (int i = 0; i < sr_models->num; i++) {
        Serial.printf("[WAKE]   model: %s\n", sr_models->model_name[i]);
    }

    // Audio front end: 1 microphone, no echo reference
    afe_config_t *cfg = afe_config_init("M", sr_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (cfg == nullptr) {
        Serial.println("[WAKE] ERROR: afe_config_init failed");
        return false;
    }
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;   // keep internal RAM for Wi-Fi
    cfg->aec_init = false;
    cfg->se_init = false;
    cfg->vad_init = false;                                  // the sketch has its own voice detector
    cfg->wakenet_init = WAKE_HI_ESP && cfg->wakenet_model_name != nullptr;
    wakenet_enabled = cfg->wakenet_init;
    afe_handle = esp_afe_handle_from_config(cfg);
    afe_data = afe_handle->create_from_config(cfg);
    afe_config_free(cfg);
    if (afe_data == nullptr) {
        Serial.println("[WAKE] ERROR: could not create the audio front end");
        return false;
    }

    // MultiNet English command recognizer for the "Tiva" phrases
    char *mn_name = esp_srmodel_filter(sr_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (mn_name == nullptr) {
        Serial.println("[WAKE] ERROR: English MultiNet model not found");
        return false;
    }
    multinet = esp_mn_handle_from_name(mn_name);
    mn_data = multinet->create(mn_name, 5760);
    if (mn_data == nullptr) {
        Serial.println("[WAKE] ERROR: could not create MultiNet");
        return false;
    }

    // Command IDs must start at 1 (ESP-SR rule): phrase i -> command id i + 1
    esp_mn_commands_alloc(multinet, mn_data);
    int added = 0;
    for (size_t i = 0; i < WAKE_PHRASE_COUNT; i++) {
        if (esp_mn_commands_phoneme_add(i + 1, WAKE_PHRASES[i].text, WAKE_PHRASES[i].phonemes) == ESP_OK) {
            Serial.printf("[WAKE]   phrase %d: \"%s\"  [%s]\n", (int)i + 1, WAKE_PHRASES[i].text, WAKE_PHRASES[i].phonemes);
            added++;
        } else {
            Serial.printf("[WAKE]   phrase \"%s\" could not be added\n", WAKE_PHRASES[i].text);
        }
    }
    esp_mn_error_t *errors = esp_mn_commands_update();
    if (errors != nullptr) {
        for (int i = 0; i < errors->num; i++) {
            Serial.printf("[WAKE]   phrase rejected: \"%s\"\n", errors->phrases[i]->string);
            added--;
        }
    }
    if (added == 0) {
        Serial.println("[WAKE] ERROR: no wake phrase could be added");
        return false;
    }

    int fetch_chunk = afe_handle->get_fetch_chunksize(afe_data);
    int mn_chunk = multinet->get_samp_chunksize(mn_data);
    int channels = afe_handle->get_feed_channel_num(afe_data);
    afe_feed_chunk = afe_handle->get_feed_chunksize(afe_data);
    Serial.printf("[WAKE] feed chunk %d x %d ch, fetch chunk %d, multinet chunk %d\n", afe_feed_chunk, channels, fetch_chunk, mn_chunk);
    if (fetch_chunk != mn_chunk || channels != 1) {
        Serial.println("[WAKE] ERROR: unexpected audio chunk sizes");
        return false;
    }

    afe_feed_buf = (int16_t *)heap_caps_malloc(afe_feed_chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (afe_feed_buf == nullptr) return false;

    xTaskCreatePinnedToCore(wake_task, "wake", 8192, nullptr, 2, nullptr, 1);
    Serial.printf("[WAKE] Ready. Say \"hello tiva\" or \"chao tiva\" to start, \"ok tiva\" or \"cam on tiva\" to stop%s\n", wakenet_enabled ? " (\"Hi ESP\" also starts)" : "");
    return true;
#else
    return false;
#endif
}

// Sends one 20 ms mic frame to the wake word engine
static void feed_wake_engine(const int16_t *frame) {
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        afe_feed_buf[afe_feed_fill++] = frame[i];
        if (afe_feed_fill == afe_feed_chunk) {
            afe_handle->feed(afe_data, afe_feed_buf);
            afe_feed_fill = 0;
        }
    }
}

static void stop_wake_listening() {
    wake_listening = false;
    wake_event = 0;
}

static void start_wake_listening(int kind) {
    wake_listening = false;
    wake_accept_kind = kind;
    wake_event = 0;
    afe_feed_fill = 0;
    wake_reset_request = true;
    uint32_t start = millis();
    while (wake_reset_request && millis() - start < 500) delay(5);
    wake_listening = true;
}

// ===================== Listening =====================
// Sleep mode (conversation = false): waits for "Hello Tiva" / "Chao Tiva" or a held button.
// Conversation mode: waits for a question (voice) or "OK Tiva" / "Cam on Tiva".
// Without a working wake word engine, any speech starts a recording.
// timeout_ms = 0 means wait forever.
static Trigger wait_for_trigger(bool conversation, uint32_t timeout_ms) {
    bool use_sr = wake_ready;
    bool use_voice = conversation || !use_sr;
    int loud_frames = 0;
    uint32_t start = millis();
    uint32_t last_debug = millis();
    pre_head = 0;
    pre_filled = 0;

    if (use_sr) start_wake_listening(conversation ? PHRASE_STOP : PHRASE_START);
    if (!use_sr) {
        listen_caption = "Mời bạn nói";
    } else if (conversation) {
        listen_caption = "Mời bạn hỏi";
    } else {
        listen_caption = "Gọi: Chào Tiva";
    }
    set_state(FACE_LISTENING);
    Serial.println(conversation ? "\n[LISTEN] Conversation: ask a question or say \"OK Tiva\"..." :
                   (use_sr ? "\n[LISTEN] Sleeping: say \"Hello Tiva\" or \"Chao Tiva\"..." : "\n[LISTEN] Speak now..."));

    Trigger result = TRIGGER_NONE;
    while (result == TRIGGER_NONE) {
        float rms = read_mic_frame(mic_frame);
        memcpy(pre_roll[pre_head], mic_frame, sizeof(mic_frame));
        pre_head = (pre_head + 1) % PRE_ROLL_FRAMES;
        if (pre_filled < PRE_ROLL_FRAMES) pre_filled++;

        float threshold = speech_threshold();
        if (rms <= threshold) {
            noise_floor = 0.98f * noise_floor + 0.02f * rms;   // slowly follow background noise
        }

        int old_volume = volume_percent;
        if (handle_buttons(true)) {
            result = TRIGGER_BUTTON;
            break;
        }
        if (volume_percent != old_volume) {      // a beep was played: drop that audio
            flush_mic(200);
            pre_filled = 0;
            loud_frames = 0;
            if (use_sr) start_wake_listening(conversation ? PHRASE_STOP : PHRASE_START);
            continue;
        }

        if (use_sr) {
            feed_wake_engine(mic_frame);
            int event = wake_event;
            if (event != 0) {
                int id = wake_phrase_id - 1;   // command ids start at 1
                Serial.printf("[WAKE] Heard \"%s\" (prob %.2f)\n",
                              (event == 2) ? "hi esp" : ((id >= 0 && id < (int)WAKE_PHRASE_COUNT) ? WAKE_PHRASES[id].text : "?"),
                              wake_prob);
                result = conversation ? TRIGGER_STOP_WORD : TRIGGER_WAKE_WORD;
                break;
            }
        }

        if (use_voice) {
            // In a conversation the question must be louder and longer (200 ms) so room noise does not count
            float needed = conversation ? threshold * CONVERSATION_STRICT : threshold;
            int frames_needed = conversation ? 10 : START_FRAMES;
            if (rms > needed) {
                if (++loud_frames >= frames_needed) result = TRIGGER_VOICE;
            } else {
                loud_frames = 0;
            }
        }

        if (timeout_ms > 0 && millis() - start > timeout_ms) break;

        if (millis() - last_debug > 10000) {
            last_debug = millis();
            Serial.printf("[LISTEN] noise=%.0f threshold=%.0f now=%.0f\n", noise_floor, threshold, rms);
        }
    }

    // Keep listening for "OK Tiva" while the question is being recorded
    if (!(use_sr && conversation && result == TRIGGER_VOICE)) stop_wake_listening();
    return result;
}

// Records the question into upload_buffer. Returns the number of samples (0 = nobody spoke).
// use_pre_roll: keep the 300 ms before the trigger (speech already started)
// wait_for_speech: the person has not started speaking yet (after wake word / button)
// listen_stop: the wake engine is running and "OK Tiva" / "Cam on Tiva" ends the conversation
// stop_heard: set to true if a stop phrase was said (nothing is sent to the server then)
static size_t record_question(bool use_pre_roll, bool wait_for_speech, bool listen_stop, bool *stop_heard) {
    *stop_heard = false;
    int16_t *pcm = (int16_t *)(upload_buffer + pcm_offset);
    size_t count = 0;

    if (use_pre_roll) {
        int start = (pre_head - pre_filled + PRE_ROLL_FRAMES) % PRE_ROLL_FRAMES;
        for (int i = 0; i < pre_filled; i++) {
            memcpy(pcm + count, pre_roll[(start + i) % PRE_ROLL_FRAMES], sizeof(mic_frame));
            count += FRAME_SAMPLES;
        }
    }

    record_caption = wait_for_speech ? "Mình nghe đây!" : "Đang nghe...";
    set_state(FACE_RECORDING);
    Serial.println("[REC] Recording...");

    float threshold = speech_threshold();
    bool started = !wait_for_speech;        // has the person started speaking?
    uint32_t speech_ms = started ? START_FRAMES * FRAME_MS : 0;
    uint32_t silence_ms = 0;
    uint32_t waited_ms = 0;
    int loud_run = 0;
    const size_t keep_before = PRE_ROLL_FRAMES * FRAME_SAMPLES;   // 300 ms before the speech starts

    while (count + FRAME_SAMPLES <= MAX_RECORD_SAMPLES) {
        float rms = read_mic_frame(pcm + count);
        if (listen_stop) {
            feed_wake_engine(pcm + count);
            if (wake_event != 0) {
                *stop_heard = true;
                break;
            }
        }
        count += FRAME_SAMPLES;
        bool loud = rms > threshold;

        if (!started) {
            // Wait for 60 ms of continuous speech, so a click or a short noise does not count
            loud_run = loud ? loud_run + 1 : 0;
            if (loud_run >= START_FRAMES) {
                started = true;
                speech_ms = START_FRAMES * FRAME_MS;
                record_caption = "Đang nghe...";
                continue;
            }
            waited_ms += FRAME_MS;
            if (waited_ms > WAIT_SPEECH_MS) break;
            // Do not send seconds of silence: keep only the last 300 ms before the speech
            if (count >= 2 * keep_before) {
                memmove(pcm, pcm + count - keep_before, keep_before * sizeof(int16_t));
                count = keep_before;
            }
            continue;
        }

        if (loud) {
            speech_ms += FRAME_MS;
            silence_ms = 0;
        } else {
            silence_ms += FRAME_MS;
            if (silence_ms >= END_SILENCE_MS) break;
        }
    }
    mic_level = 0;

    if (listen_stop) {
        // The recognizer runs a little behind the microphone: give it 300 ms to finish
        for (int i = 0; i < 15 && !*stop_heard; i++) {
            if (wake_event != 0) *stop_heard = true;
            delay(20);
        }
        if (*stop_heard) {
            int id = wake_phrase_id - 1;
            Serial.printf("[WAKE] Heard \"%s\" (prob %.2f)\n",
                          (id >= 0 && id < (int)WAKE_PHRASE_COUNT) ? WAKE_PHRASES[id].text : "?", wake_prob);
        }
        stop_wake_listening();
        if (*stop_heard) return 0;
    }

    Serial.printf("[REC] Recorded %.1f s (speech %.1f s)\n", count / (float)MIC_SAMPLE_RATE, speech_ms / 1000.0f);
    if (speech_ms < MIN_SPEECH_MS) {
        Serial.println("[REC] No question heard.");
        return 0;
    }
    return count;
}

// ===================== WAV helpers =====================
static void put_u32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put_u16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint32_t get_u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t get_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static void write_wav_header(uint8_t *h, uint32_t data_bytes, uint32_t sample_rate) {
    memcpy(h, "RIFF", 4);         put_u32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8); put_u32(h + 16, 16);
    put_u16(h + 20, 1);           // PCM
    put_u16(h + 22, 1);           // mono
    put_u32(h + 24, sample_rate);
    put_u32(h + 28, sample_rate * 2);
    put_u16(h + 32, 2);           // block align
    put_u16(h + 34, 16);          // bits per sample
    memcpy(h + 36, "data", 4);    put_u32(h + 40, data_bytes);
}

// Reads exactly n bytes from the stream (returns false on timeout / disconnect)
// Works for a network stream (net != nullptr) and for a file (net == nullptr: no data = end of file)
static bool read_exact(Stream *stream, WiFiClient *net, uint8_t *buf, size_t n, uint32_t timeout_ms = 15000) {
    size_t got = 0;
    uint32_t last = millis();
    while (got < n) {
        int avail = stream->available();
        if (avail > 0) {
            got += stream->readBytes(buf + got, min((size_t)avail, n - got));
            last = millis();
        } else if (net == nullptr || !net->connected() || millis() - last > timeout_ms) {
            return false;
        } else {
            delay(1);
        }
    }
    return true;
}

static bool skip_bytes(Stream *stream, WiFiClient *net, uint32_t n) {
    uint8_t tmp[64];
    while (n > 0) {
        uint32_t chunk = min(n, (uint32_t)sizeof(tmp));
        if (!read_exact(stream, net, tmp, chunk)) return false;
        n -= chunk;
    }
    return true;
}

// Parses a WAV stream (HTTP response or file) and plays it on the speaker
static bool play_wav(Stream *stream, WiFiClient *net) {
    uint8_t hdr[40];
    if (!read_exact(stream, net, hdr, 12) || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        Serial.println("[PLAY] Response is not a WAV file.");
        return false;
    }

    uint32_t sample_rate = 24000;
    uint16_t channels = 1, bits = 16;
    while (true) {
        if (!read_exact(stream, net, hdr, 8)) return false;
        uint32_t chunk_size = get_u32(hdr + 4);
        if (memcmp(hdr, "fmt ", 4) == 0) {
            uint32_t to_read = min(chunk_size, (uint32_t)sizeof(hdr));
            if (!read_exact(stream, net, hdr, to_read)) return false;
            channels = get_u16(hdr + 2);
            sample_rate = get_u32(hdr + 4);
            bits = get_u16(hdr + 14);
            if (!skip_bytes(stream, net, chunk_size - to_read + (chunk_size & 1))) return false;
        } else if (memcmp(hdr, "data", 4) == 0) {
            break;   // audio starts here; play until the stream ends
        } else {
            if (!skip_bytes(stream, net, chunk_size + (chunk_size & 1))) return false;
        }
    }

    Serial.printf("[PLAY] %u Hz, %u ch, %u bit\n", (unsigned)sample_rate, (unsigned)channels, (unsigned)bits);
    if (bits != 16 || channels < 1 || channels > 2 || sample_rate < 8000 || sample_rate > 48000) {
        Serial.println("[PLAY] Unsupported WAV format (need 16-bit PCM).");
        return false;
    }

    set_speaker_rate(sample_rate);
    set_state(FACE_SPEAKING);

    static uint8_t in_buf[1024 + 4];
    static int32_t out_buf[512 * 2];
    const int frame_bytes = channels * 2;
    size_t leftover = 0;
    size_t total = 0;
    uint32_t last_data = millis();

    while (true) {
        if (handle_buttons(false)) {      // hold BOOT = stop talking
            Serial.println("[PLAY] Stopped by button.");
            break;
        }

        int avail = stream->available();
        if (avail > 0) {
            size_t n = stream->readBytes(in_buf + leftover, min((size_t)avail, sizeof(in_buf) - 4 - leftover));
            last_data = millis();
            size_t bytes = leftover + n;
            size_t frames = bytes / frame_bytes;
            uint32_t level_sum = 0;

            for (size_t i = 0; i < frames; i++) {
                int32_t sample = (int16_t)get_u16(in_buf + i * frame_bytes);
                if (channels == 2) {
                    sample = (sample + (int16_t)get_u16(in_buf + i * frame_bytes + 2)) / 2;
                }
                level_sum += abs(sample);
                int32_t out = apply_volume(sample);
                out_buf[2 * i] = out;
                out_buf[2 * i + 1] = out;
            }
            if (frames > 0) {
                mouth_level = (uint8_t)constrain((int)(level_sum / frames / 800), 0, 10);
            }
            size_t written = 0;
            i2s_channel_write(tx_handle, out_buf, frames * 2 * sizeof(int32_t), &written, portMAX_DELAY);
            total += frames * frame_bytes;

            leftover = bytes - frames * frame_bytes;
            if (leftover) memmove(in_buf, in_buf + frames * frame_bytes, leftover);
        } else if (net == nullptr || !net->connected() || millis() - last_data > 8000) {
            break;
        } else {
            delay(1);
        }
    }

    mouth_level = 0;
    play_silence(150);
    Serial.printf("[PLAY] Done, %.1f s of audio\n", total / (float)(sample_rate * frame_bytes));
    return total > 0;
}

// ===================== Server communication =====================
static void build_multipart_parts() {
    String b = String("--") + BOUNDARY + "\r\n";
    multipart_prefix = b + "Content-Disposition: form-data; name=\"format\"\r\n\r\nwav\r\n";
    if (strlen(VOICE_NAME) > 0) {
        multipart_prefix += b + "Content-Disposition: form-data; name=\"voice\"\r\n\r\n" + VOICE_NAME + "\r\n";
    }
    multipart_prefix += b + "Content-Disposition: form-data; name=\"audio_file\"; filename=\"recording.wav\"\r\n"
                            "Content-Type: audio/wav\r\n\r\n";
    multipart_tail = String("\r\n--") + BOUNDARY + "--\r\n";
}

// If the server answers with JSON, try to find an audio link like /api/v1/audio/xxx.wav
static String find_audio_path(const String &json) {
    int pos = json.indexOf("/api/v1/audio/");
    if (pos < 0) return "";
    int end = pos;
    while (end < (int)json.length() && json[end] != '"' && json[end] != '\'' && json[end] != ' ') end++;
    return json.substring(pos, end);
}

static bool fetch_and_play(const String &url) {
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.begin(url);
    int code = http.GET();
    bool ok = false;
    if (code == 200) {
        ok = play_wav(http.getStreamPtr(), http.getStreamPtr());
    } else {
        Serial.printf("[HTTP] GET %s failed: %d\n", url.c_str(), code);
    }
    http.end();
    return ok;
}

static bool send_and_play(size_t samples) {
    size_t pcm_bytes = samples * sizeof(int16_t);
    size_t prefix_len = multipart_prefix.length();
    write_wav_header(upload_buffer + prefix_len, pcm_bytes, MIC_SAMPLE_RATE);
    memcpy(upload_buffer + pcm_offset + pcm_bytes, multipart_tail.c_str(), multipart_tail.length());
    size_t total = pcm_offset + pcm_bytes + multipart_tail.length();

    set_state(FACE_THINKING);
    Serial.printf("[HTTP] Uploading %u bytes to server...\n", (unsigned)total);
    uint32_t t0 = millis();

    HTTPClient http;
    http.useHTTP10(true);              // no chunked encoding -> simple raw stream
    http.setConnectTimeout(10000);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.begin(SERVER_BASE + INTERACT_PATH);
    http.addHeader("Content-Type", String("multipart/form-data; boundary=") + BOUNDARY);
    const char *wanted_headers[] = {"Content-Type", "X-Transcript", "X-Response-Text"};
    http.collectHeaders(wanted_headers, 3);

    int code = http.POST(upload_buffer, total);
    Serial.printf("[HTTP] Status %d after %.1f s\n", code, (millis() - t0) / 1000.0f);

    bool ok = false;
    if (code == 200) {
        String content_type = http.header("Content-Type");
        Serial.printf("[HTTP] Content-Type: %s\n", content_type.c_str());
        if (http.header("X-Transcript").length()) Serial.println("[YOU]   " + http.header("X-Transcript"));
        if (http.header("X-Response-Text").length()) Serial.println("[ROBOT] " + http.header("X-Response-Text"));

        if (content_type.indexOf("json") >= 0) {
            String body = http.getString();
            Serial.println("[HTTP] JSON: " + body);
            http.end();
            String path = find_audio_path(body);
            if (path.length()) {
                ok = fetch_and_play(SERVER_BASE + path);
            } else {
                Serial.println("[HTTP] No audio link found in JSON.");
            }
            return ok;
        }
        ok = play_wav(http.getStreamPtr(), http.getStreamPtr());
    } else if (code > 0) {
        Serial.println("[HTTP] Error body: " + http.getString());
    } else {
        Serial.printf("[HTTP] Request failed: %s\n", http.errorToString(code).c_str());
        http.end();
        select_server();   // the network may have changed: look for an address that answers
        return false;
    }
    http.end();
    return ok;
}

// ===================== Wi-Fi =====================
static WiFiMulti wifi_multi;

static void add_known_networks() {
    static bool added = false;
    if (added) return;
    added = true;
    // A network chosen on the phone setup page comes first
    String saved_ssid = prefs.getString("wifi_ssid", "");
    String saved_pass = prefs.getString("wifi_pass", "");
    if (saved_ssid.length() > 0) {
        static char ssid_buf[33], pass_buf[65];
        strlcpy(ssid_buf, saved_ssid.c_str(), sizeof(ssid_buf));
        strlcpy(pass_buf, saved_pass.c_str(), sizeof(pass_buf));
        wifi_multi.addAP(ssid_buf, pass_buf);
        Serial.printf("[WIFI] Saved network: \"%s\"\n", ssid_buf);
    }
    for (size_t i = 0; i < sizeof(KNOWN_WIFI) / sizeof(KNOWN_WIFI[0]); i++) {
        wifi_multi.addAP(KNOWN_WIFI[i].ssid, KNOWN_WIFI[i].password);
        Serial.printf("[WIFI] Known network: \"%s\"\n", KNOWN_WIFI[i].ssid);
    }
}

// Tries all known networks. Returns true when connected.
static bool try_known_networks(uint32_t timeout_ms) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    // Mini ESP32-S3 boards have a tiny antenna / weak regulator:
    // full TX power often makes Wi-Fi fail or resets the board (brownout).
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    add_known_networks();

    uint32_t start = millis();
    bool led_on = false;
    Serial.print("[WIFI] Connecting");
    while (millis() - start < timeout_ms) {
        if (wifi_multi.run(4000) == WL_CONNECTED) {
            WiFi.setTxPower(WIFI_POWER_8_5dBm);
            Serial.printf("\n[WIFI] Connected to \"%s\", IP: %s, signal %d dBm\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
            return true;
        }
        led_on = !led_on;
        if (led_on) set_led(40, 0, 0); else set_led(0, 0, 0);   // blinking red = connecting
        Serial.print(".");
    }
    Serial.println("\n[WIFI] None of the known networks is available.");
    return false;
}

// ---- Phone setup page ("Tiva-Setup" Wi-Fi) ----
static String html_escape(const String &text) {
    String out;
    for (size_t i = 0; i < text.length(); i++) {
        char c = text[i];
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if (c == '&') out += "&amp;";
        else out += c;
    }
    return out;
}

static String setup_page(const String &options) {
    String page =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Tiva WiFi</title><style>"
        "body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px;background:#0f172a;color:#e2e8f0}"
        "h2{color:#38bdf8}select,input,button{width:100%;padding:12px;margin:8px 0;font-size:16px;border-radius:8px;border:1px solid #334155;box-sizing:border-box}"
        "button{background:#0ea5e9;color:#fff;border:none;font-weight:bold}"
        "</style></head><body><h2>Cài đặt WiFi cho Tiva</h2>"
        "<form method='POST' action='/save'>"
        "<label>Chọn mạng WiFi (2.4GHz)</label><select name='ssid'>" + options + "</select>"
        "<label>Hoặc gõ tên mạng</label><input name='ssid_manual' placeholder='Tên WiFi'>"
        "<label>Mật khẩu</label><input name='pass' type='password' placeholder='Mật khẩu WiFi'>"
        "<button type='submit'>Lưu và kết nối</button></form></body></html>";
    return page;
}

static void run_setup_portal() {
    Serial.printf("[SETUP] Open Wi-Fi \"%s\" (password %s) on your phone, then go to 192.168.4.1\n",
                  SETUP_AP_NAME, SETUP_AP_PASSWORD);
    set_state(FACE_SETUP);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    // List nearby networks for the drop-down
    String options;
    int count = WiFi.scanNetworks();
    for (int i = 0; i < count; i++) {
        String name = html_escape(WiFi.SSID(i));
        if (name.length() == 0 || options.indexOf("'" + name + "'") >= 0) continue;
        options += "<option value='" + name + "'>" + name + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
    WiFi.scanDelete();
    if (options.length() == 0) options = "<option value=''>(không thấy mạng nào)</option>";

    IPAddress ap_ip(192, 168, 4, 1);
    WiFi.softAPConfig(ap_ip, ap_ip, IPAddress(255, 255, 255, 0));
    WiFi.softAP(SETUP_AP_NAME, SETUP_AP_PASSWORD);

    DNSServer dns;
    dns.start(53, "*", ap_ip);   // every web address leads to the setup page
    WebServer server(80);
    bool saved = false;

    server.on("/", HTTP_GET, [&]() { server.send(200, "text/html", setup_page(options)); });
    server.on("/save", HTTP_POST, [&]() {
        String ssid = server.arg("ssid_manual");
        ssid.trim();
        if (ssid.length() == 0) ssid = server.arg("ssid");
        String pass = server.arg("pass");
        if (ssid.length() == 0) {
            server.send(200, "text/html", "<meta charset='utf-8'><h3>Chưa chọn mạng WiFi.</h3><a href='/'>Quay lại</a>");
            return;
        }
        prefs.putString("wifi_ssid", ssid);
        prefs.putString("wifi_pass", pass);
        Serial.printf("[SETUP] Saved network \"%s\". Restarting...\n", ssid.c_str());
        server.send(200, "text/html",
                    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<h3>Đã lưu! Tiva đang khởi động lại và kết nối WiFi mới.</h3>");
        saved = true;
    });
    server.onNotFound([&]() {
        server.sendHeader("Location", "http://192.168.4.1/", true);
        server.send(302, "text/plain", "");
    });
    server.begin();

    uint32_t start = millis();
    while (!saved && millis() - start < SETUP_PORTAL_TIMEOUT_MS) {
        dns.processNextRequest();
        server.handleClient();
        delay(5);
    }
    if (saved) {
        delay(1500);   // let the phone receive the page
        ESP.restart();
    }
    Serial.println("[SETUP] Nobody used the setup page. Trying the known networks again.");
    server.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
}

static void connect_wifi() {
    if (WiFi.status() == WL_CONNECTED) return;
    set_state(FACE_WIFI);
    while (!try_known_networks(WIFI_CONNECT_TIMEOUT_MS)) {
        run_setup_portal();
        set_state(FACE_WIFI);
    }
}

// Finds a server address that answers. Returns true if one was found.
static bool select_server() {
    size_t count = sizeof(SERVER_CANDIDATES) / sizeof(SERVER_CANDIDATES[0]);
    for (size_t i = 0; i < count; i++) {
        HTTPClient http;
        http.setConnectTimeout(4000);
        http.setTimeout(6000);
        http.begin(String(SERVER_CANDIDATES[i]) + HEALTH_PATH);
        int code = http.GET();
        String body = code > 0 ? http.getString() : http.errorToString(code);
        http.end();
        Serial.printf("[SERVER] %s -> %d %s\n", SERVER_CANDIDATES[i], code, body.substring(0, 120).c_str());
        if (code == 200) {
            SERVER_BASE = SERVER_CANDIDATES[i];
            Serial.printf("[SERVER] Using %s\n", SERVER_BASE.c_str());
            return true;
        }
    }
    SERVER_BASE = SERVER_CANDIDATES[0];
    Serial.println("[SERVER] WARNING: no server address answers. Check that the server is on, and add its LAN address to SERVER_CANDIDATES.");
    return false;
}

// ===================== Spoken prompts (goodbye) =====================
static bool littlefs_ok = false;
static const char *GOODBYE_FILE = "/goodbye.wav";
static const char *GOODBYE_TEXT_FILE = "/goodbye.txt";

static String json_escape(const char *text) {
    String out;
    for (const char *p = text; *p; p++) {
        if (*p == '"' || *p == '\\') out += '\\';
        out += *p;
    }
    return out;
}

static bool file_is_wav(const char *path) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    uint8_t hdr[12];
    bool ok = f.read(hdr, 12) == 12 && memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0 && f.size() > 1000;
    f.close();
    return ok;
}

// Downloads a WAV from the server into a file. Returns true if a real WAV was saved.
static bool download_wav(const String &url, const char *path) {
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.begin(url);
    int code = http.GET();
    bool ok = false;
    if (code == 200) {
        File f = LittleFS.open(path, "w");
        if (f) {
            http.writeToStream(&f);
            f.close();
            ok = file_is_wav(path);
        }
    } else {
        Serial.printf("[VOICE] GET %s -> %d\n", url.c_str(), code);
    }
    http.end();
    if (!ok) LittleFS.remove(path);
    return ok;
}

// Asks the server to speak `text` and saves the sound in `path`
static bool fetch_prompt(const char *text, const char *path) {
    String voice = strlen(VOICE_NAME) > 0 ? String("\"") + VOICE_NAME + "\"" : String("null");

    // 1) Dedicated text-to-speech endpoint (exact words)
    {
        HTTPClient http;
        http.useHTTP10(true);
        http.setTimeout(HTTP_TIMEOUT_MS);
        http.begin(SERVER_BASE + TTS_PATH);
        http.addHeader("Content-Type", "application/json");
        const char *wanted_headers[] = {"Content-Type"};
        http.collectHeaders(wanted_headers, 1);
        String body = String("{\"text\":\"") + json_escape(text) + "\",\"voice\":" + voice + ",\"format\":\"wav\"}";
        int code = http.POST(body);
        if (code == 200 && http.header("Content-Type").indexOf("audio") >= 0) {
            File f = LittleFS.open(path, "w");
            if (f) {
                http.writeToStream(&f);
                f.close();
            }
            http.end();
            if (file_is_wav(path)) {
                Serial.println("[VOICE] Goodbye sound made with /api/v1/tts");
                return true;
            }
            LittleFS.remove(path);
        } else {
            Serial.printf("[VOICE] %s not available (%d), trying the chat endpoint\n", TTS_PATH, code);
            http.end();
        }
    }

    // 2) Chat endpoint: ask the robot to repeat the sentence word for word
    {
        HTTPClient http;
        http.setTimeout(HTTP_TIMEOUT_MS);
        http.begin(SERVER_BASE + CHAT_PATH);
        http.addHeader("Content-Type", "application/json");
        String message = String("Hãy đọc lại nguyên văn câu sau, không thêm hay bớt chữ nào: ") + text;
        String body = String("{\"message\":\"") + json_escape(message.c_str()) + "\",\"synthesize_speech\":true,\"voice\":" + voice + "}";
        int code = http.POST(body);
        String reply = code > 0 ? http.getString() : String("");
        http.end();
        Serial.printf("[VOICE] chat -> %d: %s\n", code, reply.substring(0, 400).c_str());
        if (code != 200) return false;
        String audio = find_audio_path(reply);
        if (audio.length() == 0) {
            Serial.println("[VOICE] No audio link in the chat reply.");
            return false;
        }
        if (download_wav(SERVER_BASE + audio, path)) {
            Serial.println("[VOICE] Goodbye sound made with the chat endpoint");
            return true;
        }
    }
    return false;
}

// Makes sure the goodbye sound is stored in flash (asks the server only when the text changed)
static void prepare_prompts() {
    littlefs_ok = LittleFS.begin(true, "/littlefs", 10, "spiffs");
    if (!littlefs_ok) {
        Serial.println("[VOICE] Flash storage not available -> goodbye will be a tone");
        return;
    }
    String wanted = String(GOODBYE_TEXT) + "|" + VOICE_NAME;
    String stored;
    File t = LittleFS.open(GOODBYE_TEXT_FILE, "r");
    if (t) {
        stored = t.readString();
        t.close();
    }
    if (stored == wanted && file_is_wav(GOODBYE_FILE)) {
        Serial.println("[VOICE] Goodbye sound ready (from flash)");
        return;
    }
    Serial.println("[VOICE] Asking the server to record the goodbye sentence...");
    if (fetch_prompt(GOODBYE_TEXT, GOODBYE_FILE)) {
        File w = LittleFS.open(GOODBYE_TEXT_FILE, "w");
        if (w) {
            w.print(wanted);
            w.close();
        }
    } else {
        Serial.println("[VOICE] Could not get the goodbye sound -> a tone will be used (will retry next start)");
    }
}

// Plays the goodbye sentence; falls back to a tone if it is not available
static void say_goodbye() {
    speak_caption = "Tạm biệt!";
    if (littlefs_ok && file_is_wav(GOODBYE_FILE)) {
        File f = LittleFS.open(GOODBYE_FILE, "r");
        if (f) {
            play_wav(&f, nullptr);
            f.close();
            speak_caption = "Đang trả lời";
            return;
        }
    }
    listen_caption = "Tạm biệt!";
    set_state(FACE_LISTENING);
    play_goodbye_sound();
    delay(1500);
    speak_caption = "Đang trả lời";
}

// ===================== Main =====================
void setup() {
    Serial.begin(115200);
    pinMode(BOOT_BUTTON, INPUT_PULLUP);
    if (VOL_UP_PIN >= 0) pinMode(VOL_UP_PIN, INPUT_PULLUP);
    if (VOL_DOWN_PIN >= 0) pinMode(VOL_DOWN_PIN, INPUT_PULLUP);

    uint32_t wait_start = millis();
    while (!Serial && millis() - wait_start < 3000) delay(10);   // give Serial Monitor time to reconnect
    delay(500);
    Serial.println("\n=== TIVA - ROBOT LAB TOUR GUIDE ===");

    esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("[BOOT] Reset reason: %d%s\n", (int)reason,
                  reason == ESP_RST_BROWNOUT ? "  <-- BROWNOUT: power supply too weak, use a better USB cable/port" : "");

    prefs.begin("robot", false);
    volume_percent = constrain(prefs.getInt("volume", DEFAULT_VOLUME), MIN_VOLUME, MAX_VOLUME);
    Serial.printf("[VOLUME] %d%%\n", (int)volume_percent);

    setup_display();
    set_state(FACE_BOOT);

    if (!psramFound()) {
        Serial.println("[ERROR] PSRAM not found. Set Tools -> PSRAM -> OPI PSRAM");
        set_state(FACE_ERROR);
        while (true) delay(1000);
    }

    build_multipart_parts();
    pcm_offset = multipart_prefix.length() + 44;
    size_t buffer_size = pcm_offset + MAX_RECORD_SAMPLES * sizeof(int16_t) + multipart_tail.length() + 16;
    upload_buffer = (uint8_t *)ps_malloc(buffer_size);
    if (upload_buffer == nullptr) {
        Serial.println("[ERROR] Not enough PSRAM for the recording buffer");
        set_state(FACE_ERROR);
        while (true) delay(1000);
    }
    memcpy(upload_buffer, multipart_prefix.c_str(), multipart_prefix.length());

    setup_microphone();
    setup_speaker(24000);
    play_silence(50);

    wake_ready = setup_wake_word();
    if (USE_WAKE_WORD && !wake_ready) {
        Serial.println("[WAKE] Wake word disabled -> robot will listen to any speech instead.");
    }
    Serial.printf("[MEM] Free internal RAM: %u bytes, free PSRAM: %u bytes\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)ESP.getFreePsram());

    // Open the Wi-Fi setup page on demand: press RST, and as soon as the face appears
    // hold BOOT until the screen shows the Wi-Fi instructions (about 3 seconds).
    // (Holding BOOT *while* pressing RST would put the chip in download mode instead.)
    if (digitalRead(BOOT_BUTTON) == LOW) {
        Serial.println("[SETUP] BOOT held during start -> Wi-Fi setup");
        run_setup_portal();
    }
    connect_wifi();
    if (!select_server()) {
        set_state(FACE_ERROR);
        play_error_sound();
        delay(1500);
    }
    prepare_prompts();
    calibrate_noise();
}

static void end_conversation(bool say_goodbye_now) {
    conversation_active = false;
    if (say_goodbye_now) {
        Serial.println("[CHAT] Conversation ended. Bye!");
        say_goodbye();
    } else {
        Serial.println("[CHAT] Nobody asked anything for a while -> sleeping.");
    }
}

// Lets the person know the robot is listening, then records the question
static size_t start_question(bool *stop_heard) {
    record_caption = "Mình nghe đây!";
    set_state(FACE_RECORDING);
    if (WAKE_CHIME) play_wake_chime();
    flush_mic(200);   // drop the chime picked up by the mic (longer than the 160 ms mic buffer)
    if (wake_ready) start_wake_listening(PHRASE_STOP);
    return record_question(false, true, wake_ready, stop_heard);
}

void loop() {
    connect_wifi();

    bool stop_heard = false;
    size_t samples = 0;

    if (!conversation_active) {
        // ---- Sleeping: wait for "Hello Tiva" / "Chao Tiva" ----
        Trigger trigger = wait_for_trigger(false, 0);
        if (trigger == TRIGGER_VOICE) {
            samples = record_question(true, false, false, &stop_heard);   // no wake word engine
        } else {
            conversation_active = wake_ready;
            last_activity = millis();
            Serial.println("[CHAT] Conversation started.");
            samples = start_question(&stop_heard);
        }
    } else {
        // ---- Conversation: any question, until "OK Tiva" / "Cam on Tiva" ----
        uint32_t timeout = 0;
        if (CONVERSATION_IDLE_MS > 0) {
            uint32_t idle = millis() - last_activity;
            if (idle >= CONVERSATION_IDLE_MS) {
                end_conversation(false);
                return;
            }
            timeout = CONVERSATION_IDLE_MS - idle;
        }
        Trigger trigger = wait_for_trigger(true, timeout);
        if (trigger == TRIGGER_NONE) {
            end_conversation(false);
            return;
        }
        if (trigger == TRIGGER_STOP_WORD) {
            end_conversation(true);
            return;
        }
        if (trigger == TRIGGER_BUTTON) {
            samples = start_question(&stop_heard);
        } else {
            samples = record_question(true, false, wake_ready, &stop_heard);
        }
    }

    if (stop_heard) {
        end_conversation(true);
        return;
    }

    if (samples > 0) {
        last_activity = millis();
        if (send_and_play(samples)) {
            last_activity = millis();
        } else {
            set_state(FACE_ERROR);
            play_error_sound();
            delay(1500);
        }
    }

    // Ignore the robot's own voice and anything collected while busy, then re-measure the room
    flush_mic(300);
    if (samples > 0) update_noise_floor();
    while (digitalRead(BOOT_BUTTON) == LOW) delay(10);
}
