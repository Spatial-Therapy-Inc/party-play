#include "pp_sound.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pp_synth.h"
#include "yaykit_speak.h"

static const char *TAG = "pp_sound";

#define QUEUE 64        // sounds waiting for the next block (a burst of dice hits is the most)
#define IDLE_END_S 10   // the voice ends after this long with nothing sounding (the next sound starts a new one)

typedef struct {
    uint8_t sound;
    float gain;
    int64_t at_us;
} req_t;

// Single consumer (the voice, on the speaker task); producers serialise on a spinlock (the game, the console).
static req_t s_q[QUEUE];
static volatile uint32_t s_head, s_tail;
static portMUX_TYPE s_qlock = portMUX_INITIALIZER_UNLOCKED;
static pp_synth_t *s_synth;
static SemaphoreHandle_t s_start_lock;
static volatile uint32_t s_owner;  // the token of the voice that may render; an older one ends at its next block
static uint32_t s_token;
static volatile bool s_active, s_muted;
static yk_sfx_id_t s_voice = YK_SFX_BUSY;
static uint32_t s_quiet;            // samples rendered with nothing sounding

static bool render(float *out, size_t n, const yk_sfx_block_t *blk, void *ctx) {
    if ((uint32_t)(uintptr_t)ctx != s_owner) {
        memset(out, 0, n * sizeof(float));
        return false;
    }
    while (s_tail != s_head) {
        const req_t r = s_q[s_tail % QUEUE];
        __sync_synchronize();
        s_tail++;
        const float delay = r.at_us > blk->heard_us ? (r.at_us - blk->heard_us) / 1e6f : 0;
        pp_synth_play_at(s_synth, (pp_sound_t)r.sound, r.gain, delay);
    }
    const int left = pp_synth_render_f(s_synth, out, (int)n);
    s_quiet = left > 0 ? 0 : s_quiet + (uint32_t)n;
    if (s_quiet > blk->rate * IDLE_END_S) {
        s_active = false;  // a producer that sees this starts a new voice; one that pushed first is seen below
        __sync_synchronize();
        if (s_tail == s_head) return false;
        s_active = true;
        s_quiet = 0;
    }
    return true;
}

static void start_voice(void) {
    xSemaphoreTake(s_start_lock, portMAX_DELAY);
    if (!s_active && !s_muted) {
        s_active = true;
        s_quiet = 0;
        s_owner = ++s_token;
        const yk_sfx_opts_t o = {.gain = 1};
        s_voice = yk_sfx_play_synth(render, (void *)(uintptr_t)s_owner, &o);
        if (s_voice < 0) s_active = false;  // no voice free: the next sound tries again
    }
    xSemaphoreGive(s_start_lock);
}

esp_err_t pp_sound_start(void) {
    // Party Play never records, so the speaker can run at 24 kHz (crisper clicks); 5 ms blocks for the dice
    esp_err_t err = yk_speak_init_cfg(&(yk_speak_config_t){.rate = 24000, .block_ms = 5});
    if (err != ESP_OK) return err;
    s_synth = heap_caps_calloc(1, sizeof(pp_synth_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_start_lock = xSemaphoreCreateMutex();
    if (!s_synth || !s_start_lock) return ESP_ERR_NO_MEM;
    pp_synth_init(s_synth, (int)yk_speak_rate(), esp_random());
    ESP_LOGI(TAG, "%u Hz, sounds heard ~%d ms after they are asked for", (unsigned)yk_speak_rate(), yk_speak_latency_ms());
    return ESP_OK;
}

void pp_sound_play(pp_sound_t sound, float gain, int64_t at_us) {
    if (!s_synth || s_muted) return;
    portENTER_CRITICAL(&s_qlock);
    if (s_head - s_tail < QUEUE) {
        s_q[s_head % QUEUE] = (req_t){(uint8_t)sound, gain, at_us};
        __sync_synchronize();
        s_head++;
    }
    portEXIT_CRITICAL(&s_qlock);
    if (!s_active) start_voice();
}

void pp_sound_mute(bool mute) {
    if (!s_synth || mute == s_muted) return;
    s_muted = mute;
    if (!mute) return;
    xSemaphoreTake(s_start_lock, portMAX_DELAY);
    s_owner = 0;
    if (s_voice > 0) yk_sfx_stop(s_voice, 10);  // returns once the voice no longer renders
    s_voice = YK_SFX_BUSY;
    s_active = false;
    s_tail = s_head;  // nobody renders now: drop the queue and what was sounding
    pp_synth_clear(s_synth);
    xSemaphoreGive(s_start_lock);
}

void pp_sound_hold_amp(bool on) { yk_speak_hold_amp(on); }

int pp_sound_latency_ms(void) { return yk_speak_latency_ms(); }
