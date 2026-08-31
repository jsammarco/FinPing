#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <toolbox/level_duration.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define TAG "FinPing"
#define APP_SETTINGS_PATH APP_DATA_PATH("settings.bin")
#define CALL_SIGN_AUDIO_FOLDER APP_DATA_PATH("audio")

#define SCREEN_W 128
#define SCREEN_H 64
#define SAMPLE_RATE 40000u
#define SAMPLE_US (1000000u / SAMPLE_RATE)
#define MAX_SEQUENCE 32
#define MAX_AUDIO_PATH 128
#define MAX_AUDIO_SAMPLES 80000u
#define WAV_MIN_SAMPLE_RATE 8000u
#define WAV_MAX_SOURCE_SAMPLE_RATE 48000u
#define WAV_TARGET_SAMPLE_RATE 8000u
#define WAV_MAX_CHANNELS 2u
#define WAV_RF_PEAK 118
#define AUDIO_KEYUP_MS 600u
#define AUDIO_POST_PAUSE_MS 600u
#define PAUSE_SYMBOL 'p'
#define LONG_PAUSE_SYMBOL 'l'
#define PAUSE_MS 1000u
#define LONG_PAUSE_MS 3000u
#define FREQ_MIN_HZ 387000000u
#define FREQ_MAX_HZ 464000000u
#define FREQ_STEP_COUNT 7u
#define MENU_ITEM_COUNT 5u
#define INTERVAL_OPTION_COUNT 7u
#define FREQUENCY_DIGIT_COUNT 8u
#define SETTINGS_MAGIC 0x44544D46u
#define SETTINGS_VERSION 4u

typedef enum {
    ScreenMenu = 0,
    ScreenMessage,
    ScreenFrequency,
    ScreenInterval,
} AppScreen;

typedef enum {
    TxStateIdle = 0,
    TxStateRunning,
    TxStateDone,
    TxStateEmptyMessage,
    TxStateAudioError,
    TxStateBlocked,
    TxStateRadioError,
    TxStateInvalidFrequency,
    TxStateCancelled,
} TxState;

typedef enum {
    SegLead = 0,
    SegAudio,
    SegAudioGap,
    SegTone,
    SegGap,
    SegTail,
    SegRepeatGap,
    SegDone,
} TxSegment;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t frequency_hz;
    uint16_t tone_ms;
    uint16_t gap_ms;
    uint16_t lead_ms;
    uint16_t tail_ms;
    uint16_t repeat_gap_ms;
    uint8_t repeat_count;
    uint8_t level_percent;
    uint8_t freq_step_index;
    uint8_t interval_index;
    uint8_t reserved;
    char sequence[MAX_SEQUENCE + 1];
    char audio_path[MAX_AUDIO_PATH];
} AppSettings;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t frequency_hz;
    uint16_t tone_ms;
    uint16_t gap_ms;
    uint16_t lead_ms;
    uint16_t tail_ms;
    uint16_t repeat_gap_ms;
    uint8_t repeat_count;
    uint8_t level_percent;
    uint8_t freq_step_index;
    uint8_t reserved;
    char sequence[MAX_SEQUENCE + 1];
    char audio_path[MAX_AUDIO_PATH];
} AppSettingsV3;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t frequency_hz;
    uint16_t tone_ms;
    uint16_t gap_ms;
    uint16_t lead_ms;
    uint16_t tail_ms;
    uint16_t repeat_gap_ms;
    uint8_t repeat_count;
    uint8_t level_percent;
    uint8_t freq_step_index;
    uint8_t reserved;
    char sequence[MAX_SEQUENCE + 1];
} AppSettingsV2;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t frequency_hz;
    uint16_t tone_ms;
    uint16_t gap_ms;
    uint16_t lead_ms;
    uint16_t tail_ms;
    uint16_t repeat_gap_ms;
    uint8_t repeat_count;
    uint8_t level_percent;
    uint8_t freq_step_index;
    uint8_t reserved;
} AppSettingsV1;

typedef struct {
    char sequence[MAX_SEQUENCE + 1];
    uint8_t sequence_len;

    volatile TxSegment segment;
    volatile uint32_t samples_left;
    volatile uint8_t char_index;
    volatile uint8_t repeat_index;
    volatile bool finished;

    uint32_t phase_a;
    uint32_t phase_b;
    uint32_t phase_inc_a;
    uint32_t phase_inc_b;
    uint16_t pdm_acc;

    uint16_t tone_ms;
    uint16_t gap_ms;
    uint16_t lead_ms;
    uint16_t tail_ms;
    uint16_t repeat_gap_ms;
    uint8_t repeat_count;
    uint8_t level_percent;

    const int8_t* audio_samples;
    uint32_t audio_sample_count;
    uint32_t audio_sample_rate;
    uint32_t audio_index;
    uint32_t audio_resample_accumulator;
} DtmfEngine;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* queue;
    DialogsApp* dialogs;

    AppScreen screen;
    AppSettings settings;

    char sequence[MAX_SEQUENCE + 1];
    uint8_t sequence_len;
    uint8_t menu_index;
    uint8_t keypad_x;
    uint8_t keypad_y;
    uint8_t frequency_digit;
    uint32_t frequency_edit_hz;
    uint8_t interval_edit_index;

    TxState tx_state;
    const SubGhzDevice* radio;
    bool radio_registry_ready;
    bool radio_begun;
    bool charge_suppressed;
    bool async_tx_started;

    DtmfEngine engine;
    uint32_t actual_frequency_hz;
    int8_t* audio_samples;
    uint32_t audio_sample_count;
    uint32_t audio_sample_rate;

    bool interval_armed;
    uint32_t next_interval_tick;
} App;

static const int16_t sine_lut[256] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179, 6393, 5602, 4808, 4011, 3212, 2410, 1608, 804,
    0, -804, -1608, -2410, -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804,
};

static const char keypad[4][4] = {
    {'1','2','3','A'},
    {'4','5','6','B'},
    {'7','8','9','C'},
    {'*','0','#','D'},
};

static const uint16_t dtmf_low[4] = {697, 770, 852, 941};
static const uint16_t dtmf_high[4] = {1209, 1336, 1477, 1633};
static const uint32_t frequency_digit_steps[FREQUENCY_DIGIT_COUNT] = {
    100000000u, 10000000u, 1000000u, 100000u,
    10000u, 1000u, 100u, 10u,
};
static const uint8_t frequency_digit_offsets[FREQUENCY_DIGIT_COUNT] = {
    0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u,
};
static const char* const interval_labels[INTERVAL_OPTION_COUNT] = {
    "One Shot", "1 min", "2 min", "5 min", "10 min", "30 min", "60 min",
};
static const uint8_t interval_minutes[INTERVAL_OPTION_COUNT] = {
    0u, 1u, 2u, 5u, 10u, 30u, 60u,
};

static bool sequence_symbol_is_valid(char c) {
    if(c >= '0' && c <= '9') return true;
    if(c >= 'A' && c <= 'D') return true;
    return c == '*' || c == '#' || c == PAUSE_SYMBOL || c == LONG_PAUSE_SYMBOL;
}

static void settings_defaults(AppSettings* s) {
    memset(s, 0, sizeof(*s));
    s->magic = SETTINGS_MAGIC;
    s->version = SETTINGS_VERSION;
    s->size = sizeof(*s);
    s->frequency_hz = 433920000;
    s->tone_ms = 120;
    s->gap_ms = 80;
    s->lead_ms = 180;
    s->tail_ms = 180;
    s->repeat_gap_ms = 500;
    s->repeat_count = 1;
    s->level_percent = 80;
    s->freq_step_index = 4; // 25 kHz
    s->interval_index = 0;
}

static void settings_sanitize(AppSettings* s) {
    if(s->frequency_hz < FREQ_MIN_HZ) s->frequency_hz = FREQ_MIN_HZ;
    if(s->frequency_hz > FREQ_MAX_HZ) s->frequency_hz = FREQ_MAX_HZ;
    if(s->tone_ms < 40) s->tone_ms = 40;
    if(s->tone_ms > 1000) s->tone_ms = 1000;
    if(s->gap_ms > 1000) s->gap_ms = 1000;
    if(s->lead_ms > 2000) s->lead_ms = 2000;
    if(s->tail_ms > 2000) s->tail_ms = 2000;
    if(s->repeat_gap_ms > 5000) s->repeat_gap_ms = 5000;
    if(s->repeat_count < 1) s->repeat_count = 1;
    if(s->repeat_count > 9) s->repeat_count = 9;
    if(s->level_percent < 20) s->level_percent = 20;
    if(s->level_percent > 100) s->level_percent = 100;
    if(s->freq_step_index >= FREQ_STEP_COUNT) s->freq_step_index = 4;
    if(s->interval_index >= INTERVAL_OPTION_COUNT) s->interval_index = 0;

    uint8_t write_index = 0;
    for(uint8_t read_index = 0; read_index < MAX_SEQUENCE; read_index++) {
        char symbol = s->sequence[read_index];
        if(symbol == '\0') break;
        if(sequence_symbol_is_valid(symbol)) {
            s->sequence[write_index++] = symbol;
        }
    }
    s->sequence[write_index] = '\0';
    s->audio_path[MAX_AUDIO_PATH - 1u] = '\0';
}

static bool settings_load(AppSettings* s) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool ok = false;
    AppSettings temp = {0};

    if(storage_file_open(file, APP_SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        size_t n = storage_file_read(file, &temp, sizeof(temp));
        if(n == sizeof(temp) &&
           temp.magic == SETTINGS_MAGIC &&
           temp.version == SETTINGS_VERSION &&
           temp.size == sizeof(temp)) {
            *s = temp;
            ok = true;
        } else if(n == sizeof(AppSettingsV3) &&
                  temp.magic == SETTINGS_MAGIC &&
                  temp.version == 3u &&
                  temp.size == sizeof(AppSettingsV3)) {
            AppSettingsV3 legacy;
            memcpy(&legacy, &temp, sizeof(legacy));
            memcpy(s, &legacy, sizeof(legacy));
            s->magic = SETTINGS_MAGIC;
            s->version = SETTINGS_VERSION;
            s->size = sizeof(*s);
            s->interval_index = 0;
            ok = true;
        } else if(n == sizeof(AppSettingsV2) &&
                  temp.magic == SETTINGS_MAGIC &&
                  temp.version == 2u &&
                  temp.size == sizeof(AppSettingsV2)) {
            AppSettingsV2 legacy;
            memcpy(&legacy, &temp, sizeof(legacy));
            memcpy(s, &legacy, sizeof(legacy));
            s->magic = SETTINGS_MAGIC;
            s->version = SETTINGS_VERSION;
            s->size = sizeof(*s);
            s->audio_path[0] = '\0';
            ok = true;
        } else if(n == sizeof(AppSettingsV1) &&
                  temp.magic == SETTINGS_MAGIC &&
                  temp.version == 1u &&
                  temp.size == sizeof(AppSettingsV1)) {
            AppSettingsV1 legacy;
            memcpy(&legacy, &temp, sizeof(legacy));
            s->frequency_hz = legacy.frequency_hz;
            s->tone_ms = legacy.tone_ms;
            s->gap_ms = legacy.gap_ms;
            s->lead_ms = legacy.lead_ms;
            s->tail_ms = legacy.tail_ms;
            s->repeat_gap_ms = legacy.repeat_gap_ms;
            s->repeat_count = legacy.repeat_count;
            s->level_percent = legacy.level_percent;
            s->freq_step_index = legacy.freq_step_index;
            ok = true;
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static bool settings_save(const AppSettings* s) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, APP_DATA_PATH(""));
    File* file = storage_file_alloc(storage);
    bool ok = false;

    if(storage_file_open(file, APP_SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        size_t n = storage_file_write(file, s, sizeof(*s));
        ok = (n == sizeof(*s));
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static void interval_disarm(App* app) {
    app->interval_armed = false;
    app->next_interval_tick = 0;
}

static void interval_schedule_next(App* app) {
    uint8_t interval_index = app->settings.interval_index;
    if(interval_index == 0 || interval_index >= INTERVAL_OPTION_COUNT) {
        interval_disarm(app);
        return;
    }

    app->next_interval_tick = furi_get_tick() +
        (uint32_t)interval_minutes[interval_index] * 60u * 1000u;
}

static bool interval_is_due(const App* app) {
    return app->interval_armed && app->next_interval_tick != 0 &&
           (int32_t)(furi_get_tick() - app->next_interval_tick) >= 0;
}

static uint16_t read_le16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool storage_read_exact(File* file, void* buffer, size_t size) {
    return storage_file_read(file, buffer, size) == size;
}

static void audio_unload(App* app) {
    if(app->audio_samples) free(app->audio_samples);
    app->audio_samples = NULL;
    app->audio_sample_count = 0;
    app->audio_sample_rate = 0;
}

static void audio_ensure_folder(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, APP_DATA_PATH(""));
    storage_common_mkdir(storage, CALL_SIGN_AUDIO_FOLDER);
    furi_record_close(RECORD_STORAGE);
}

static int8_t audio_clamp_sample(int32_t sample) {
    if(sample > 127) return 127;
    if(sample < -127) return -127;
    return (int8_t)sample;
}

static void audio_prepare_for_rf(int8_t* samples, uint32_t sample_count) {
    if(sample_count == 0) return;

    // Remove any DC component so silence remains centered on the RF carrier.
    int64_t sum = 0;
    for(uint32_t index = 0; index < sample_count; index++) {
        sum += samples[index];
    }
    int32_t dc_offset = (int32_t)(sum / (int64_t)sample_count);

    // A short smoothing filter limits harsh high-frequency speech content before
    // it reaches the low-oversampling 1-bit PDM transmitter.
    int32_t previous = 0;
    int32_t peak = 0;
    for(uint32_t index = 0; index < sample_count; index++) {
        int32_t current = (int32_t)samples[index] - dc_offset;
        int32_t filtered = (current + previous) / 2;
        previous = current;
        samples[index] = audio_clamp_sample(filtered);

        int32_t magnitude = filtered < 0 ? -filtered : filtered;
        if(magnitude > peak) peak = magnitude;
    }

    // WAV files commonly have conservative recording levels. Normalize just the
    // audio preamble so it uses the available +/-2.38 kHz deviation without
    // adding nonlinear distortion that could resemble DTMF on a receiver.
    if(peak == 0) return;
    for(uint32_t index = 0; index < sample_count; index++) {
        int32_t scaled = ((int32_t)samples[index] * WAV_RF_PEAK) / peak;
        samples[index] = audio_clamp_sample(scaled);
    }
}

static bool audio_load(App* app) {
    audio_unload(app);
    if(app->settings.audio_path[0] == '\0') return true;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    int8_t* samples = NULL;
    bool ok = false;

    uint8_t riff_header[12];
    uint8_t chunk_header[8];
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t sample_rate = 0;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    bool fmt_found = false;
    bool data_found = false;

    if(!storage_file_open(file, app->settings.audio_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        goto cleanup;
    }
    if(!storage_read_exact(file, riff_header, sizeof(riff_header)) ||
       memcmp(riff_header, "RIFF", 4) != 0 || memcmp(&riff_header[8], "WAVE", 4) != 0) {
        goto cleanup;
    }

    while(storage_file_tell(file) + sizeof(chunk_header) <= storage_file_size(file)) {
        if(!storage_read_exact(file, chunk_header, sizeof(chunk_header))) goto cleanup;

        uint32_t chunk_size = read_le32(&chunk_header[4]);
        uint32_t chunk_padding = chunk_size & 1u;
        if(memcmp(chunk_header, "fmt ", 4) == 0) {
            uint8_t format_data[16];
            if(chunk_size < sizeof(format_data) ||
               !storage_read_exact(file, format_data, sizeof(format_data))) {
                goto cleanup;
            }
            audio_format = read_le16(&format_data[0]);
            channels = read_le16(&format_data[2]);
            sample_rate = read_le32(&format_data[4]);
            bits_per_sample = read_le16(&format_data[14]);
            fmt_found = true;

            if((chunk_size > sizeof(format_data) &&
                !storage_file_seek(file, chunk_size - sizeof(format_data), false)) ||
               (chunk_padding && !storage_file_seek(file, 1, false))) {
                goto cleanup;
            }
        } else if(memcmp(chunk_header, "data", 4) == 0) {
            data_offset = (uint32_t)storage_file_tell(file);
            data_size = chunk_size;
            data_found = true;
            if(fmt_found) break;
            if(!storage_file_seek(file, chunk_size + chunk_padding, false)) goto cleanup;
        } else if(!storage_file_seek(file, chunk_size + chunk_padding, false)) {
            goto cleanup;
        }
    }

    if(!fmt_found || !data_found || audio_format != 1 || channels == 0 ||
       channels > WAV_MAX_CHANNELS ||
       (bits_per_sample != 8 && bits_per_sample != 16) ||
       sample_rate < WAV_MIN_SAMPLE_RATE || sample_rate > WAV_MAX_SOURCE_SAMPLE_RATE) {
        goto cleanup;
    }

    uint32_t bytes_per_sample = bits_per_sample / 8u;
    uint32_t bytes_per_frame = bytes_per_sample * channels;
    if(data_size == 0 || data_size % bytes_per_frame != 0) goto cleanup;
    uint32_t source_frame_count = data_size / bytes_per_frame;
    uint32_t output_sample_rate = sample_rate;
    if(output_sample_rate > WAV_TARGET_SAMPLE_RATE) output_sample_rate = WAV_TARGET_SAMPLE_RATE;
    // One output slot is needed for source frame zero; each later slot begins
    // only when a source frame falls into it. This matches the streaming bin
    // resampler below and preserves the source duration without an empty tail.
    uint32_t output_sample_count = (uint32_t)(
        (((uint64_t)(source_frame_count - 1u) * output_sample_rate) / sample_rate) + 1u);
    if(output_sample_count == 0 || output_sample_count > MAX_AUDIO_SAMPLES) goto cleanup;

    samples = malloc(output_sample_count);
    if(!samples || !storage_file_seek(file, data_offset, true)) goto cleanup;

    uint8_t raw[256];
    uint32_t source_frames_read = 0;
    uint32_t output_samples_written = 0;
    uint32_t output_bin = 0;
    int32_t output_bin_sum = 0;
    uint32_t output_bin_frames = 0;
    while(source_frames_read < source_frame_count) {
        uint32_t frames_to_read = source_frame_count - source_frames_read;
        uint32_t max_chunk_frames = sizeof(raw) / bytes_per_frame;
        if(frames_to_read > max_chunk_frames) frames_to_read = max_chunk_frames;
        size_t bytes_to_read = (size_t)frames_to_read * bytes_per_frame;
        if(!storage_read_exact(file, raw, bytes_to_read)) goto cleanup;

        for(uint32_t frame = 0; frame < frames_to_read; frame++) {
            uint32_t source_frame_index = source_frames_read + frame;
            uint32_t next_output_bin = (uint32_t)(
                ((uint64_t)source_frame_index * output_sample_rate) / sample_rate);

            // Average every source frame in a destination time slot. This is a
            // streaming low-pass resampler, avoiding the aliases caused by simply
            // picking one out of every 44.1 kHz source frames.
            while(next_output_bin > output_bin) {
                if(output_bin_frames == 0 || output_samples_written >= output_sample_count) {
                    goto cleanup;
                }
                samples[output_samples_written++] =
                    audio_clamp_sample((output_bin_sum / (int32_t)output_bin_frames) / 256);
                output_bin++;
                output_bin_sum = 0;
                output_bin_frames = 0;
            }

            int32_t mixed_sample = 0;
            for(uint16_t channel = 0; channel < channels; channel++) {
                uint32_t offset = frame * bytes_per_frame + channel * bytes_per_sample;
                if(bits_per_sample == 8) {
                    mixed_sample += ((int16_t)raw[offset] - 128) * 256;
                } else {
                    mixed_sample += (int16_t)read_le16(&raw[offset]);
                }
            }
            output_bin_sum += mixed_sample / channels;
            output_bin_frames++;
        }
        source_frames_read += frames_to_read;
    }

    if(output_bin_frames == 0 || output_samples_written >= output_sample_count) goto cleanup;
    samples[output_samples_written++] =
        audio_clamp_sample((output_bin_sum / (int32_t)output_bin_frames) / 256);
    if(output_samples_written != output_sample_count) goto cleanup;

    audio_prepare_for_rf(samples, output_sample_count);

    app->audio_samples = samples;
    app->audio_sample_count = output_sample_count;
    app->audio_sample_rate = output_sample_rate;
    samples = NULL;
    ok = true;

cleanup:
    if(samples) free(samples);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static void sequence_format(char* out, size_t out_size, const char* sequence, uint8_t sequence_len) {
    if(out_size == 0) return;

    size_t out_index = 0;
    for(uint8_t index = 0; index < sequence_len && out_index + 1 < out_size; index++) {
        char symbol = sequence[index];
        if(symbol == PAUSE_SYMBOL) {
            out[out_index++] = 'P';
        } else if(symbol == LONG_PAUSE_SYMBOL) {
            if(out_index + 2 >= out_size) break;
            out[out_index++] = 'L';
            out[out_index++] = 'P';
        } else {
            out[out_index++] = symbol;
        }
    }
    out[out_index] = '\0';
}

static void dtmf_lookup(char c, uint16_t* low, uint16_t* high) {
    for(uint8_t y = 0; y < 4; y++) {
        for(uint8_t x = 0; x < 4; x++) {
            if(keypad[y][x] == c) {
                *low = dtmf_low[y];
                *high = dtmf_high[x];
                return;
            }
        }
    }
    *low = 697;
    *high = 1209;
}

static uint32_t ms_to_samples(uint16_t ms) {
    return ((uint32_t)ms * SAMPLE_RATE) / 1000u;
}

static void engine_set_tone(DtmfEngine* e, char c) {
    uint16_t a, b;
    dtmf_lookup(c, &a, &b);
    e->phase_a = 0;
    e->phase_b = 0;
    e->phase_inc_a = (uint32_t)(((uint64_t)a << 32) / SAMPLE_RATE);
    e->phase_inc_b = (uint32_t)(((uint64_t)b << 32) / SAMPLE_RATE);
}

static void engine_enter_segment(DtmfEngine* e, TxSegment seg) {
    e->segment = seg;

    switch(seg) {
    case SegLead: {
        // Audio preambles need a longer steady-carrier period than DTMF alone
        // so a repeater has time to detect and key up before speech starts.
        uint16_t lead_ms = e->lead_ms;
        if(e->audio_sample_count > 0 && lead_ms < AUDIO_KEYUP_MS) {
            lead_ms = AUDIO_KEYUP_MS;
        }
        e->samples_left = ms_to_samples(lead_ms);
        break;
    }
    case SegAudio:
        e->audio_index = 0;
        e->audio_resample_accumulator = 0;
        e->samples_left = (uint32_t)(
            ((uint64_t)e->audio_sample_count * SAMPLE_RATE + e->audio_sample_rate - 1u) /
            e->audio_sample_rate);
        break;
    case SegAudioGap: {
        // Keep the carrier up after speech. If DTMF follows, retain its normal
        // inter-symbol gap as well.
        uint16_t gap_ms = AUDIO_POST_PAUSE_MS;
        if(e->sequence_len > 0) gap_ms += e->gap_ms;
        e->samples_left = ms_to_samples(gap_ms);
        break;
    }
    case SegTone:
        if(e->char_index < e->sequence_len) {
            char symbol = e->sequence[e->char_index];
            if(symbol == PAUSE_SYMBOL) {
                e->samples_left = ms_to_samples(PAUSE_MS);
            } else if(symbol == LONG_PAUSE_SYMBOL) {
                e->samples_left = ms_to_samples(LONG_PAUSE_MS);
            } else {
                engine_set_tone(e, symbol);
                e->samples_left = ms_to_samples(e->tone_ms);
            }
        } else {
            e->samples_left = 0;
        }
        break;
    case SegGap:
        e->samples_left = ms_to_samples(e->gap_ms);
        break;
    case SegTail:
        e->samples_left = ms_to_samples(e->tail_ms);
        break;
    case SegRepeatGap:
        e->samples_left = ms_to_samples(e->repeat_gap_ms);
        break;
    case SegDone:
    default:
        e->samples_left = 0;
        e->finished = true;
        break;
    }
}

static void engine_advance(DtmfEngine* e) {
    switch(e->segment) {
    case SegLead:
        if(e->audio_sample_count > 0) {
            engine_enter_segment(e, SegAudio);
        } else if(e->sequence_len > 0) {
            e->char_index = 0;
            engine_enter_segment(e, SegTone);
        } else {
            engine_enter_segment(e, SegTail);
        }
        break;

    case SegAudio:
        engine_enter_segment(e, SegAudioGap);
        break;

    case SegAudioGap:
        if(e->sequence_len > 0) {
            e->char_index = 0;
            engine_enter_segment(e, SegTone);
        } else {
            engine_enter_segment(e, SegTail);
        }
        break;

    case SegTone:
        if(e->char_index + 1u < e->sequence_len) {
            char current_symbol = e->sequence[e->char_index];
            char next_symbol = e->sequence[e->char_index + 1u];
            if(current_symbol == PAUSE_SYMBOL || current_symbol == LONG_PAUSE_SYMBOL ||
               next_symbol == PAUSE_SYMBOL || next_symbol == LONG_PAUSE_SYMBOL) {
                e->char_index++;
                engine_enter_segment(e, SegTone);
            } else {
                engine_enter_segment(e, SegGap);
            }
        } else {
            engine_enter_segment(e, SegTail);
        }
        break;

    case SegGap:
        e->char_index++;
        engine_enter_segment(e, SegTone);
        break;

    case SegTail:
        if(e->repeat_index + 1u < e->repeat_count) {
            e->repeat_index++;
            engine_enter_segment(e, SegRepeatGap);
        } else {
            engine_enter_segment(e, SegDone);
        }
        break;

    case SegRepeatGap:
        e->char_index = 0;
        engine_enter_segment(e, SegLead);
        break;

    case SegDone:
    default:
        e->finished = true;
        break;
    }
}

static void engine_init(DtmfEngine* e, const App* app) {
    memset(e, 0, sizeof(*e));
    memcpy(e->sequence, app->sequence, app->sequence_len);
    e->sequence[app->sequence_len] = '\0';
    e->sequence_len = app->sequence_len;

    e->tone_ms = app->settings.tone_ms;
    e->gap_ms = app->settings.gap_ms;
    e->lead_ms = app->settings.lead_ms;
    e->tail_ms = app->settings.tail_ms;
    e->repeat_gap_ms = app->settings.repeat_gap_ms;
    e->repeat_count = app->settings.repeat_count;
    e->level_percent = app->settings.level_percent;
    e->audio_samples = app->audio_samples;
    e->audio_sample_count = app->audio_sample_count;
    e->audio_sample_rate = app->audio_sample_rate;

    e->pdm_acc = 0;
    e->repeat_index = 0;
    e->char_index = 0;
    e->finished = false;
    engine_enter_segment(e, SegLead);
}

static int16_t engine_audio_sample(DtmfEngine* e) {
    if(e->segment == SegAudio) {
        if(e->audio_index >= e->audio_sample_count) return 0;

        int16_t sample = (int16_t)e->audio_samples[e->audio_index] * 256;
        e->audio_resample_accumulator += e->audio_sample_rate;
        if(e->audio_resample_accumulator >= SAMPLE_RATE) {
            e->audio_resample_accumulator -= SAMPLE_RATE;
            e->audio_index++;
        }
        return sample;
    }

    if(e->segment != SegTone) return 0;
    if(e->sequence[e->char_index] == PAUSE_SYMBOL ||
       e->sequence[e->char_index] == LONG_PAUSE_SYMBOL) {
        return 0;
    }

    int32_t a = sine_lut[e->phase_a >> 24];
    int32_t b = sine_lut[e->phase_b >> 24];
    e->phase_a += e->phase_inc_a;
    e->phase_b += e->phase_inc_b;

    int32_t mixed = (a + b) / 2;
    mixed = (mixed * e->level_percent) / 100;

    if(mixed > 32767) mixed = 32767;
    if(mixed < -32767) mixed = -32767;
    return (int16_t)mixed;
}

static bool engine_is_steady_carrier_segment(const DtmfEngine* e) {
    switch(e->segment) {
    case SegLead:
    case SegAudioGap:
    case SegGap:
    case SegTail:
    case SegRepeatGap:
        return true;
    case SegTone:
        return e->char_index < e->sequence_len &&
               (e->sequence[e->char_index] == PAUSE_SYMBOL ||
                e->sequence[e->char_index] == LONG_PAUSE_SYMBOL);
    default:
        return false;
    }
}

static LevelDuration dtmf_tx_yield(void* context) {
    DtmfEngine* e = context;

    while(!e->finished && e->samples_left == 0) {
        engine_advance(e);
    }

    if(e->finished) return level_duration_reset();

    if(engine_is_steady_carrier_segment(e)) {
        // Do not generate a 40 kHz PDM pattern for silence. A constant async
        // level keeps the RF carrier present for repeater key-up without making
        // an audible idle tone in the receiver's audio path.
        uint32_t duration = e->samples_left * SAMPLE_US;
        e->samples_left = 0;
        return level_duration_make(true, duration);
    }

    int16_t sample = engine_audio_sample(e);

    // Convert desired analog FM deviation into a 1-bit pulse-density stream.
    // With the CC1101 in 2-FSK async mode, HIGH and LOW select +/- deviation.
    // The FM receiver's IF/audio path averages the fast switching, recovering
    // the DTMF waveform.
    int32_t density = 32768 + (int32_t)sample;
    if(density < 1) density = 1;
    if(density > 65535) density = 65535;

    uint32_t accum = (uint32_t)e->pdm_acc + (uint32_t)density;
    bool level = (accum >= 65536u);
    e->pdm_acc = (uint16_t)(accum & 0xFFFFu);

    e->samples_left--;
    return level_duration_make(level, SAMPLE_US);
}

static bool radio_prepare(App* app) {
    if(!app->radio_registry_ready) {
        subghz_devices_init();
        app->radio_registry_ready = true;
    }

    if(!app->radio) {
        app->radio = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
        if(!app->radio) return false;
    }

    if(!app->radio_begun) {
        // The internal CC1101 intentionally has no begin callback, so this
        // returns false even though the radio is available. Calling it is
        // still correct for external devices; do not treat its return value
        // as an error for the built-in radio.
        subghz_devices_begin(app->radio);
        app->radio_begun = true;
    }

    return true;
}

static void radio_stop(App* app) {
    if(app->async_tx_started) {
        subghz_devices_stop_async_tx(app->radio);
        app->async_tx_started = false;
    }
    if(app->radio && app->radio_begun) {
        subghz_devices_idle(app->radio);
    }
    if(app->charge_suppressed) {
        furi_hal_power_suppress_charge_exit();
        app->charge_suppressed = false;
    }
    audio_unload(app);
}

static bool radio_start(App* app) {
    radio_stop(app);

    if(!audio_load(app)) {
        app->tx_state = TxStateAudioError;
        return false;
    }
    if(app->sequence_len == 0 && app->audio_sample_count == 0) {
        app->tx_state = TxStateEmptyMessage;
        return false;
    }
    if(!radio_prepare(app)) {
        audio_unload(app);
        app->tx_state = TxStateRadioError;
        return false;
    }

    subghz_devices_reset(app->radio);
    subghz_devices_idle(app->radio);

    if(!furi_hal_region_is_frequency_allowed(app->settings.frequency_hz) ||
       !furi_hal_subghz_is_frequency_valid(app->settings.frequency_hz) ||
       !subghz_devices_is_frequency_valid(app->radio, app->settings.frequency_hz)) {
        audio_unload(app);
        app->tx_state = TxStateInvalidFrequency;
        return false;
    }

    // +/- 2.38 kHz 2-FSK is intentionally used as the two-level RF DAC.
    subghz_devices_load_preset(app->radio, FuriHalSubGhzPreset2FSKDev238Async, NULL);
    app->actual_frequency_hz =
        subghz_devices_set_frequency(app->radio, app->settings.frequency_hz);

    engine_init(&app->engine, app);

    furi_hal_power_suppress_charge_enter();
    app->charge_suppressed = true;

    bool ok = subghz_devices_start_async_tx(app->radio, dtmf_tx_yield, &app->engine);
    if(!ok) {
        radio_stop(app);
        app->tx_state = TxStateBlocked;
        return false;
    }

    app->async_tx_started = true;
    app->tx_state = TxStateRunning;
    return true;
}

static void radio_poll(App* app) {
    if(app->tx_state == TxStateRunning && app->async_tx_started) {
        if(subghz_devices_is_async_complete_tx(app->radio)) {
            radio_stop(app);
            app->tx_state = TxStateDone;
            if(app->interval_armed) interval_schedule_next(app);
        }
        return;
    }

    // Keep periodic TX on the main screen, where its status is visible and
    // Back can cancel it. If an editor is open, send when the user returns.
    if(app->screen == ScreenMenu && interval_is_due(app)) {
        if(!radio_start(app)) {
            // A periodic transmission cannot continue safely after a radio,
            // WAV, or regional-permission error. Leave the reason on screen.
            interval_disarm(app);
        }
    }
}

static void radio_cancel(App* app) {
    if(app->tx_state == TxStateRunning) {
        radio_stop(app);
        app->tx_state = TxStateCancelled;
    }
    interval_disarm(app);
}

static void draw_header(Canvas* c, const char* title) {
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 9, title);
    canvas_draw_line(c, 0, 12, 127, 12);
}

static void format_freq(char* out, size_t n, uint32_t hz) {
    snprintf(out, n, "%lu.%05lu",
        (unsigned long)(hz / 1000000u),
        (unsigned long)((hz % 1000000u) / 10u));
}

static const char* audio_filename(const char* path) {
    const char* filename = strrchr(path, '/');
    return filename ? filename + 1 : path;
}

static void audio_clear_selection(App* app) {
    app->settings.audio_path[0] = '\0';
    settings_save(&app->settings);
    app->tx_state = TxStateIdle;
}

static void audio_select_file(App* app) {
    FuriString* path = furi_string_alloc_set_str(app->settings.audio_path);
    DialogsFileBrowserOptions options;
    dialog_file_browser_set_basic_options(&options, ".wav", NULL);
    options.base_path = CALL_SIGN_AUDIO_FOLDER;

    bool selected = dialog_file_browser_show(app->dialogs, path, path, &options);
    if(selected) {
        if(furi_string_size(path) >= MAX_AUDIO_PATH) {
            app->tx_state = TxStateAudioError;
        } else {
            strncpy(app->settings.audio_path, furi_string_get_cstr(path), MAX_AUDIO_PATH - 1u);
            app->settings.audio_path[MAX_AUDIO_PATH - 1u] = '\0';
            settings_save(&app->settings);
            app->tx_state = TxStateIdle;
        }
    }

    furi_string_free(path);
}

static const char* tx_status_text(TxState state) {
    switch(state) {
    case TxStateRunning:
        return "TX: Back stops";
    case TxStateDone:
        return "Sent";
    case TxStateEmptyMessage:
        return "Set a message or WAV first";
    case TxStateAudioError:
        return "WAV invalid or too large";
    case TxStateBlocked:
        return "TX blocked by region";
    case TxStateRadioError:
        return "Radio unavailable";
    case TxStateInvalidFrequency:
        return "Frequency not supported";
    case TxStateCancelled:
        return "Transmission cancelled";
    case TxStateIdle:
    default:
        return "OK: select";
    }
}

static void draw_menu_item(Canvas* c, uint8_t row, uint8_t selected, const char* text) {
    int y = 20 + (int)row * 8;
    if(row == selected) {
        canvas_draw_box(c, 1, y - 6, 126, 8);
        canvas_set_color(c, ColorWhite);
    }
    canvas_draw_str(c, 4, y, text);
    if(row == selected) canvas_set_color(c, ColorBlack);
}

static void draw_menu(Canvas* c, App* app) {
    draw_header(c, "FinPing");
    canvas_set_font(c, FontSecondary);

    char formatted_message[66];
    char message[28];
    char frequency[20];
    format_freq(frequency, sizeof(frequency), app->settings.frequency_hz);
    sequence_format(formatted_message, sizeof(formatted_message), app->sequence, app->sequence_len);
    snprintf(
        message,
        sizeof(message),
        "Message: %.18s",
        app->sequence_len ? formatted_message : "(empty)");

    draw_menu_item(c, 0, app->menu_index, "Send RF");
    draw_menu_item(c, 1, app->menu_index, message);

    char frequency_item[32];
    snprintf(frequency_item, sizeof(frequency_item), "Frequency: %s", frequency);
    draw_menu_item(c, 2, app->menu_index, frequency_item);

    char interval_item[32];
    snprintf(
        interval_item,
        sizeof(interval_item),
        "Interval: %s",
        interval_labels[app->settings.interval_index]);
    draw_menu_item(c, 3, app->menu_index, interval_item);

    draw_menu_item(c, 4, app->menu_index, "Call Sign Audio");

    canvas_draw_line(c, 0, 56, 127, 56);
    if(app->tx_state == TxStateIdle && app->menu_index == 4) {
        if(app->settings.audio_path[0]) {
            canvas_draw_str(c, 2, 63, audio_filename(app->settings.audio_path));
        } else {
            canvas_draw_str(c, 2, 63, "OK choose; LEFT clear");
        }
    } else if(app->tx_state == TxStateDone && app->interval_armed) {
        char schedule_status[32];
        snprintf(
            schedule_status,
            sizeof(schedule_status),
            "Repeats: %s",
            interval_labels[app->settings.interval_index]);
        canvas_draw_str(c, 2, 63, schedule_status);
    } else if(app->tx_state == TxStateIdle && app->menu_index == 0 && app->interval_armed) {
        canvas_draw_str(c, 2, 63, "LEFT stops schedule");
    } else {
        canvas_draw_str(c, 2, 63, tx_status_text(app->tx_state));
    }
}

static void draw_message(Canvas* c, App* app) {
    draw_header(c, "DTMF Message");
    canvas_set_font(c, FontSecondary);

    char formatted_message[66];
    char message[80];
    sequence_format(formatted_message, sizeof(formatted_message), app->sequence, app->sequence_len);
    snprintf(message, sizeof(message), ">%s_", formatted_message);
    canvas_draw_str(c, 2, 22, message);
    canvas_draw_line(c, 0, 25, 127, 25);

    for(uint8_t y = 0; y < 4; y++) {
        for(uint8_t x = 0; x < 4; x++) {
            int bx = 5 + (int)x * 30;
            int by = 34 + (int)y * 6;
            if(x == app->keypad_x && y == app->keypad_y) {
                canvas_draw_box(c, bx - 2, by - 6, 20, 8);
                canvas_set_color(c, ColorWhite);
            }
            char key[2] = {keypad[y][x], 0};
            canvas_draw_str(c, bx + 4, by, key);
            if(x == app->keypad_x && y == app->keypad_y) {
                canvas_set_color(c, ColorBlack);
            }
        }
    }

    static const char* actions[] = {"P", "LP", "OK", "DEL"};
    for(uint8_t x = 0; x < 4; x++) {
        int bx = 5 + (int)x * 30;
        int by = 62;
        if(x == app->keypad_x && app->keypad_y == 4) {
            canvas_draw_box(c, bx - 2, by - 7, 25, 9);
            canvas_set_color(c, ColorWhite);
        }
        canvas_draw_str(c, bx + 2, by, actions[x]);
        if(x == app->keypad_x && app->keypad_y == 4) {
            canvas_set_color(c, ColorBlack);
        }
    }
}

static void draw_frequency(Canvas* c, App* app) {
    draw_header(c, "Set Frequency");

    char frequency[20];
    format_freq(frequency, sizeof(frequency), app->frequency_edit_hz);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 24, 32, frequency);

    int marker_x = 24 + (int)frequency_digit_offsets[app->frequency_digit] * 8;
    canvas_draw_line(c, marker_x, 35, marker_x + 6, 35);

    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 2, 45, "UP/DOWN: change digit");
    canvas_draw_str(c, 2, 53, "LEFT/RIGHT: choose digit");
    canvas_draw_str(c, 2, 63, "OK save  BACK cancel");
}

static void draw_interval(Canvas* c, App* app) {
    draw_header(c, "Intervals");
    canvas_set_font(c, FontSecondary);

    uint8_t first_visible = 0;
    if(app->interval_edit_index >= 5u) first_visible = app->interval_edit_index - 4u;

    for(uint8_t row = 0; row < 5u && first_visible + row < INTERVAL_OPTION_COUNT; row++) {
        uint8_t option = first_visible + row;
        int y = 20 + (int)row * 8;
        if(option == app->interval_edit_index) {
            canvas_draw_box(c, 1, y - 6, 126, 8);
            canvas_set_color(c, ColorWhite);
        }
        canvas_draw_str(c, 4, y, interval_labels[option]);
        if(option == app->interval_edit_index) canvas_set_color(c, ColorBlack);
    }

    canvas_draw_line(c, 0, 56, 127, 56);
    canvas_draw_str(c, 2, 63, "OK save  BACK cancel");
}

static void draw_callback(Canvas* c, void* context) {
    App* app = context;
    canvas_clear(c);
    if(app->screen == ScreenMenu) draw_menu(c, app);
    else if(app->screen == ScreenMessage) draw_message(c, app);
    else if(app->screen == ScreenFrequency) draw_frequency(c, app);
    else draw_interval(c, app);
}

static void input_callback(InputEvent* event, void* context) {
    FuriMessageQueue* q = context;
    furi_message_queue_put(q, event, 0);
}

static void sequence_add(App* app, char c) {
    if(app->sequence_len >= MAX_SEQUENCE) return;
    app->sequence[app->sequence_len++] = c;
    app->sequence[app->sequence_len] = '\0';
    memcpy(app->settings.sequence, app->sequence, sizeof(app->settings.sequence));
    settings_save(&app->settings);
    app->tx_state = TxStateIdle;
}

static void sequence_delete(App* app) {
    if(app->sequence_len == 0) return;
    app->sequence_len--;
    app->sequence[app->sequence_len] = '\0';
    memcpy(app->settings.sequence, app->sequence, sizeof(app->settings.sequence));
    settings_save(&app->settings);
    app->tx_state = TxStateIdle;
}

static void frequency_edit_adjust(App* app, int direction) {
    int64_t next = (int64_t)app->frequency_edit_hz +
        (int64_t)direction * (int64_t)frequency_digit_steps[app->frequency_digit];
    if(next < FREQ_MIN_HZ) next = FREQ_MIN_HZ;
    if(next > FREQ_MAX_HZ) next = FREQ_MAX_HZ;
    app->frequency_edit_hz = (uint32_t)next;
}

static bool handle_menu(App* app, const InputEvent* e) {
    if(app->tx_state == TxStateRunning) {
        if(e->key == InputKeyBack &&
           (e->type == InputTypeShort || e->type == InputTypeLong)) {
            radio_cancel(app);
        }
        return true;
    }

    if(e->type == InputTypeShort || e->type == InputTypeRepeat) {
        switch(e->key) {
        case InputKeyUp:
            if(app->menu_index > 0) app->menu_index--;
            break;
        case InputKeyDown:
            if(app->menu_index + 1u < MENU_ITEM_COUNT) app->menu_index++;
            break;
        case InputKeyLeft:
            if(app->menu_index == 0) interval_disarm(app);
            if(app->menu_index == 4) audio_clear_selection(app);
            break;
        case InputKeyOk:
            if(app->menu_index == 0) {
                interval_disarm(app);
                if(radio_start(app) && app->settings.interval_index != 0) {
                    app->interval_armed = true;
                }
            } else if(app->menu_index == 1) {
                app->screen = ScreenMessage;
            } else if(app->menu_index == 2) {
                app->frequency_edit_hz = app->settings.frequency_hz;
                app->frequency_digit = 2;
                app->screen = ScreenFrequency;
            } else if(app->menu_index == 3) {
                app->interval_edit_index = app->settings.interval_index;
                app->screen = ScreenInterval;
            } else {
                audio_select_file(app);
            }
            break;
        case InputKeyBack:
            return false;
        default:
            break;
        }
    }
    return true;
}

static bool handle_message(App* app, const InputEvent* e) {
    if(e->type == InputTypeLong && e->key == InputKeyBack) {
        app->screen = ScreenMenu;
        return true;
    }
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return true;

    switch(e->key) {
    case InputKeyUp:
        app->keypad_y = (app->keypad_y + 4u) % 5u;
        break;
    case InputKeyDown:
        app->keypad_y = (app->keypad_y + 1u) % 5u;
        break;
    case InputKeyLeft:
        app->keypad_x = (app->keypad_x + 3u) % 4u;
        break;
    case InputKeyRight:
        app->keypad_x = (app->keypad_x + 1u) % 4u;
        break;
    case InputKeyOk:
        if(app->keypad_y < 4) {
            sequence_add(app, keypad[app->keypad_y][app->keypad_x]);
        } else if(app->keypad_x == 0) {
            sequence_add(app, PAUSE_SYMBOL);
        } else if(app->keypad_x == 1) {
            sequence_add(app, LONG_PAUSE_SYMBOL);
        } else if(app->keypad_x == 2) {
            app->screen = ScreenMenu;
        } else {
            sequence_delete(app);
        }
        break;
    case InputKeyBack:
        sequence_delete(app);
        break;
    default:
        break;
    }
    return true;
}

static bool handle_frequency(App* app, const InputEvent* e) {
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return true;

    switch(e->key) {
    case InputKeyUp:
        frequency_edit_adjust(app, +1);
        break;
    case InputKeyDown:
        frequency_edit_adjust(app, -1);
        break;
    case InputKeyLeft:
        if(app->frequency_digit > 0) app->frequency_digit--;
        break;
    case InputKeyRight:
        if(app->frequency_digit + 1u < FREQUENCY_DIGIT_COUNT) app->frequency_digit++;
        break;
    case InputKeyOk:
        app->settings.frequency_hz = app->frequency_edit_hz;
        settings_save(&app->settings);
        app->tx_state = TxStateIdle;
        app->screen = ScreenMenu;
        break;
    case InputKeyBack:
        app->screen = ScreenMenu;
        break;
    default:
        break;
    }
    return true;
}

static bool handle_interval(App* app, const InputEvent* e) {
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return true;

    switch(e->key) {
    case InputKeyUp:
        if(app->interval_edit_index > 0) app->interval_edit_index--;
        break;
    case InputKeyDown:
        if(app->interval_edit_index + 1u < INTERVAL_OPTION_COUNT) {
            app->interval_edit_index++;
        }
        break;
    case InputKeyOk:
        app->settings.interval_index = app->interval_edit_index;
        settings_save(&app->settings);
        interval_disarm(app);
        app->tx_state = TxStateIdle;
        app->screen = ScreenMenu;
        break;
    case InputKeyBack:
        app->screen = ScreenMenu;
        break;
    default:
        break;
    }
    return true;
}

static void app_cleanup(App* app) {
    radio_stop(app);

    if(app->radio && app->radio_begun) {
        subghz_devices_sleep(app->radio);
        subghz_devices_end(app->radio);
        app->radio_begun = false;
    }

    if(app->radio_registry_ready) {
        subghz_devices_deinit();
        app->radio_registry_ready = false;
    }

    if(app->gui && app->view_port) {
        view_port_enabled_set(app->view_port, false);
        gui_remove_view_port(app->gui, app->view_port);
    }

    if(app->view_port) view_port_free(app->view_port);
    if(app->gui) furi_record_close(RECORD_GUI);
    if(app->dialogs) furi_record_close(RECORD_DIALOGS);
    if(app->queue) furi_message_queue_free(app->queue);
}

int32_t finping_app(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));

    settings_defaults(&app->settings);
    settings_load(&app->settings);
    settings_sanitize(&app->settings);
    audio_ensure_folder();

    while(app->sequence_len < MAX_SEQUENCE &&
          app->settings.sequence[app->sequence_len] != '\0') {
        app->sequence_len++;
    }
    memcpy(app->sequence, app->settings.sequence, app->sequence_len);
    app->sequence[app->sequence_len] = '\0';

    app->screen = ScreenMenu;
    app->tx_state = TxStateIdle;
    app->actual_frequency_hz = app->settings.frequency_hz;

    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->gui = furi_record_open(RECORD_GUI);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->view_port = view_port_alloc();

    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app->queue);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    bool running = true;
    while(running) {
        radio_poll(app);

        InputEvent e;
        if(furi_message_queue_get(app->queue, &e, 25) == FuriStatusOk) {
            if(app->screen == ScreenMenu) running = handle_menu(app, &e);
            else if(app->screen == ScreenMessage) running = handle_message(app, &e);
            else if(app->screen == ScreenFrequency) running = handle_frequency(app, &e);
            else running = handle_interval(app, &e);
        }

        view_port_update(app->view_port);
    }

    app_cleanup(app);
    free(app);
    return 0;
}
