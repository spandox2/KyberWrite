#include "kyberwrite_scene.h"
#include <notification/notification_messages.h>
#include <notification/notification.h>
#include <string.h>

// ── LED helpers ───────────────────────────────────────────────────────────────
// Use module-level static messages so pointers are always valid.
// notification_message() on Flipper is synchronous for LED messages,
// but keeping them static is safest.

static NotificationMessage s_msg_r = { .type = NotificationMessageTypeLedRed };
static NotificationMessage s_msg_g = { .type = NotificationMessageTypeLedGreen };
static NotificationMessage s_msg_b = { .type = NotificationMessageTypeLedBlue };
// message_do_not_reset prevents the notification service from zeroing all
// LED channels after the sequence finishes.
static const NotificationMessage* s_rgb_seq[] = {
    &s_msg_r, &s_msg_g, &s_msg_b, &message_do_not_reset, NULL
};

static void set_rgb_led(KyberApp* app, uint8_t r, uint8_t g, uint8_t b) {
    s_msg_r.data.led.value = r;
    s_msg_g.data.led.value = g;
    s_msg_b.data.led.value = b;
    notification_message(app->notifications, (const NotificationSequence*)s_rgb_seq);
}

static void set_crystal_led(KyberApp* app, int crystal_index) {
    const KyberCrystal* crystal = &KYBER_CRYSTALS[crystal_index];
    set_rgb_led(app, crystal->r, crystal->g, crystal->b);
}

static void clear_led(KyberApp* app) {
    set_rgb_led(app, 0, 0, 0);
}

// ── Crystal lookup ────────────────────────────────────────────────────────────

static int kyber_match_series1(uint32_t addr6) {
    for(int i = 0; i < (int)KYBER_CRYSTAL_COUNT; i++) {
        if(KYBER_CRYSTALS[i].addr9 == 0 && KYBER_CRYSTALS[i].addr6 == addr6) return i;
    }
    return -1;
}

static int kyber_match_addr9(uint32_t addr9) {
    if(addr9 == 0) return -1;
    for(int i = 0; i < (int)KYBER_CRYSTAL_COUNT; i++) {
        if(KYBER_CRYSTALS[i].addr9 == addr9) return i;
    }
    return -1;
}

// "Green  (Luke Skywalker)" -> color "Green", voice "Luke Skywalker".
static void kyber_split_name(
    const char* name,
    char* color,
    size_t color_size,
    char* voice,
    size_t voice_size) {
    color[0] = '\0';
    voice[0] = '\0';
    if(!name) return;

    const char* open = strchr(name, '(');
    size_t color_len = open ? (size_t)(open - name) : strlen(name);
    while(color_len > 0 && name[color_len - 1] == ' ') color_len--;
    if(color_len >= color_size) color_len = color_size - 1;
    memcpy(color, name, color_len);
    color[color_len] = '\0';

    if(!open) return;
    open++;
    size_t voice_len = strlen(open);
    if(voice_len > 0 && open[voice_len - 1] == ')') voice_len--;
    if(voice_len >= voice_size) voice_len = voice_size - 1;
    memcpy(voice, open, voice_len);
    voice[voice_len] = '\0';
}

// ── EM4305 word read ──────────────────────────────────────────────────────────
// Address 9 is not part of the EM4100 broadcast. After the read command the
// chip sits still for about 586 us, then sends one Manchester frame at RF/64:
// preamble 00001010, then 45 bits (4 rows of 8 data bits + even parity, 8
// column parity bits, and a trailing 0). A half-bit is 256 us. The stock
// Manchester decoder never locks onto that single burst, so the reply is
// sliced into half-bits instead. A pause merged with the first low half is
// about 842 us and has to count as three halves, not as a gap.

#define KYBER_PULSE_MAX 512
#define KYBER_HALF_MAX  768
#define KYBER_BIT_MAX   320

typedef struct {
    bool high;
    uint16_t us;
} KyberPulse;

static KyberPulse s_pulses[KYBER_PULSE_MAX];
static uint8_t s_halves[KYBER_HALF_MAX];
static uint8_t s_bits[KYBER_BIT_MAX];
static volatile uint16_t s_pulse_count;
static volatile uint16_t s_raw_count;
static volatile uint32_t s_min_us;
static volatile bool s_have_high;
static volatile bool s_ignore_next;
static volatile uint32_t s_high_us;

// Capture ISR. Falling reports the high width. Rising reports the full
// rise-to-rise period. A rise with no high before it is the low the capture
// opened in the middle of. Spikes of 16 us or less are dropped, matching the
// stock EM4100 reader. Every interrupt is counted, including those spikes,
// so a flat line can be told apart from edges the decoder ignores.
static void kyber_capture(bool level, uint32_t duration, void* context) {
    UNUSED(context);
    if(s_raw_count < 65535) s_raw_count++;
    if(duration < s_min_us) s_min_us = duration;
    if(s_ignore_next) {
        s_ignore_next = false;
        s_have_high = false;
        return;
    }
    if(duration <= 16) {
        if(level) s_ignore_next = true;
        s_have_high = false;
        return;
    }
    // Longer than a merged pause. This is the command, or the one stale
    // capture IRQ left pending while that command masked the timer. Break
    // the segment so it cannot pair with the first reply half.
    if(duration > 2000) {
        s_have_high = false;
        uint16_t count = s_pulse_count;
        if(count >= KYBER_PULSE_MAX) return;
        s_pulses[count].high = false;
        s_pulses[count].us = 65535;
        s_pulse_count = (uint16_t)(count + 1);
        return;
    }

    if(level) {
        s_high_us = duration;
        s_have_high = true;
        return;
    }

    uint16_t count = s_pulse_count;
    if(!s_have_high) {
        if(count >= KYBER_PULSE_MAX) return;
        s_pulses[count].high = false;
        s_pulses[count].us = (duration > 65535) ? 65535 : (uint16_t)duration;
        s_pulse_count = (uint16_t)(count + 1);
        return;
    }
    if(duration < s_high_us) {
        s_have_high = false;
        return;
    }
    if((uint16_t)(count + 2) > KYBER_PULSE_MAX) return;

    uint32_t low = duration - s_high_us;
    s_pulses[count].high = true;
    s_pulses[count].us = (s_high_us > 65535) ? 65535 : (uint16_t)s_high_us;
    s_pulses[count + 1].high = false;
    s_pulses[count + 1].us = (low > 65535) ? 65535 : (uint16_t)low;
    s_pulse_count = (uint16_t)(count + 2);
    s_have_high = false;
}

static void kyber_em_bit(bool value) {
    if(value) {
        furi_delay_us(32 * 8);
    } else {
        furi_hal_rfid_tim_read_pause();
        furi_delay_us(23 * 8);
        furi_hal_rfid_tim_read_continue();
        furi_delay_us(18 * 8);
    }
}

static void kyber_em_field_stop(void) {
    furi_hal_rfid_tim_read_pause();
    furi_delay_us(55 * 8);
    furi_hal_rfid_tim_read_continue();
    furi_delay_us(18 * 8);
}

static void kyber_em_opcode(uint8_t value) {
    // MSB first. WRITE is 010, so the firmware writer's LSB-first loop
    // still hits these chips. READ is 100, and the reversed form is ignored.
    bool parity = false;
    for(int8_t bit = 2; bit >= 0; bit--) {
        bool value_bit = ((value >> bit) & 1) != 0;
        kyber_em_bit(value_bit);
        parity ^= value_bit;
    }
    kyber_em_bit(parity);
}

static void kyber_em_address(uint8_t address) {
    bool parity = false;
    for(uint8_t i = 0; i < 4; i++) {
        bool bit = (address >> i) & 1;
        kyber_em_bit(bit);
        parity ^= bit;
    }
    kyber_em_bit(false);
    kyber_em_bit(false);
    kyber_em_bit(parity);
}

// 45-bit payload -> 32-bit word. Data bits are LSB first inside each byte.
static bool kyber_unpack_word(const uint8_t* bits, uint32_t* word) {
    uint32_t value = 0;
    for(uint8_t row = 0; row < 4; row++) {
        uint8_t parity = 0;
        for(uint8_t col = 0; col < 8; col++) {
            uint8_t bit = bits[(row * 9) + col];
            parity ^= bit;
            if(bit) value |= (1UL << ((row * 8) + col));
        }
        if(parity != bits[(row * 9) + 8]) return false;
    }
    for(uint8_t col = 0; col < 8; col++) {
        uint8_t parity = 0;
        for(uint8_t row = 0; row < 4; row++) {
            parity ^= bits[(row * 9) + col];
        }
        if(parity != bits[36 + col]) return false;
    }
    *word = value;
    return true;
}

// How many 256 us half-bits this pulse holds. 0 means a gap between frames.
// 0xFF is a spike: shorter than a half, so it must not end the frame.
// 586 us is the processing pause (two halves). Merged with the preamble's
// first half it is about 842 us (three halves).
static uint8_t kyber_halves_for(uint16_t us) {
    if(us < 140) return 0xFF;
    if(us <= 380) return 1;
    if(us <= 660) return 2;
    if(us <= 920) return 3;
    if(us <= 1180) return 4;
    if(us <= 1440) return 5;
    if(us <= 1700) return 6;
    return 0;
}

static bool kyber_preamble_at(uint16_t start, const uint8_t* pattern, uint8_t length) {
    for(uint8_t i = 0; i < length; i++) {
        if(s_bits[start + i] != pattern[i]) return false;
    }
    return true;
}

// Preamble plus 44 payload bits is enough: the last payload bit is a fixed 0
// and is only checked when the capture actually has it. The reply can sit
// anywhere in the listen. Row and column parity reject an EM4100 frame.
// The 6-bit preamble is only for the start, where the pause can swallow the
// first two zeros.
static bool kyber_scan_bits(uint16_t count, uint32_t* word) {
    static const uint8_t pre8[8] = {0, 0, 0, 0, 1, 0, 1, 0};
    static const uint8_t pre6[6] = {0, 0, 1, 0, 1, 0};

    for(uint16_t start = 0; (uint16_t)(start + 8 + 44) <= count; start++) {
        if(!kyber_preamble_at(start, pre8, 8)) continue;
        uint16_t available = (uint16_t)(count - (start + 8));
        if(available >= 45 && s_bits[start + 8 + 44] != 0) continue;
        if(kyber_unpack_word(&s_bits[start + 8], word)) return true;
    }
    for(uint16_t start = 0; start < 10; start++) {
        if((uint16_t)(start + 6 + 44) > count) break;
        if(!kyber_preamble_at(start, pre6, 6)) continue;
        uint16_t available = (uint16_t)(count - (start + 6));
        if(available >= 45 && s_bits[start + 6 + 44] != 0) continue;
        if(kyber_unpack_word(&s_bits[start + 6], word)) return true;
    }
    return false;
}

// phase 0 and 1 cover an even or odd run of idle halves before the preamble.
// The two polarities cover an inverted comparator. Even row parity accepts
// only one of them.
static bool kyber_decode_halves(uint16_t half_count, uint32_t* word) {
    for(uint8_t pol = 0; pol < 2; pol++) {
        for(uint8_t phase = 0; phase < 2; phase++) {
            uint16_t count = 0;
            uint16_t i = phase;
            while((uint16_t)(i + 1) < half_count && count < KYBER_BIT_MAX) {
                uint8_t first = s_halves[i];
                uint8_t second = s_halves[i + 1];
                if(first == second) {
                    i = (uint16_t)(i + 2);
                    continue;
                }
                bool high_then_low = (first == 1);
                bool one = (pol == 0) ? high_then_low : !high_then_low;
                s_bits[count++] = one ? 1 : 0;
                i = (uint16_t)(i + 2);
            }
            if(kyber_scan_bits(count, word)) return true;
        }
    }
    return false;
}

static bool kyber_flush_halves(uint16_t* half_count, uint32_t* word) {
    bool found = (*half_count >= 64) && kyber_decode_halves(*half_count, word);
    *half_count = 0;
    return found;
}

static bool kyber_demod_pulses(uint32_t* word) {
    uint16_t pulses = s_pulse_count;
    uint16_t half_count = 0;

    for(uint16_t pulse = 0; pulse < pulses; pulse++) {
        uint8_t n = kyber_halves_for(s_pulses[pulse].us);
        // A spike such as the 89 us low seen on hardware is not a frame end.
        if(n == 0xFF) continue;
        if(n == 0) {
            if(kyber_flush_halves(&half_count, word)) return true;
            continue;
        }
        if((uint16_t)(half_count + n) > KYBER_HALF_MAX) {
            if(kyber_flush_halves(&half_count, word)) return true;
        }
        uint8_t level = s_pulses[pulse].high ? 1 : 0;
        for(uint8_t h = 0; h < n; h++) {
            s_halves[half_count++] = level;
        }
    }
    return kyber_flush_halves(&half_count, word);
}

// The default broadcast is an EM4100 frame: nine 1s, ten rows of 4 data bits
// plus even parity, four column parities, and a stop 0. Words 5 and 6 are
// that frame, first bit in the low bit. Address 6 is the color word.
static bool kyber_em4100_frame(uint16_t start, uint16_t count, uint32_t* addr6) {
    if((uint16_t)(start + 64) > count) return false;
    for(uint8_t i = 0; i < 9; i++) {
        if(s_bits[start + i] != 1) return false;
    }
    for(uint8_t row = 0; row < 10; row++) {
        uint16_t base = (uint16_t)(start + 9u + (uint16_t)row * 5u);
        uint8_t parity = 0;
        for(uint8_t bit = 0; bit < 4; bit++) {
            parity = (uint8_t)(parity ^ s_bits[base + bit]);
        }
        if(parity != s_bits[base + 4]) return false;
    }
    for(uint8_t col = 0; col < 4; col++) {
        uint8_t parity = 0;
        for(uint8_t row = 0; row < 10; row++) {
            parity = (uint8_t)(parity ^ s_bits[start + 9u + (uint16_t)row * 5u + col]);
        }
        if(parity != s_bits[start + 59u + col]) return false;
    }
    if(s_bits[start + 63] != 0) return false;

    uint32_t word = 0;
    for(uint8_t i = 0; i < 32; i++) {
        if(s_bits[start + 32u + i]) word |= (1UL << i);
    }
    if(word == 0) return false;
    *addr6 = word;
    return true;
}

static bool kyber_scan_em4100(uint16_t count, uint32_t* addr6) {
    if(count < 64) return false;
    for(uint16_t start = 0; (uint16_t)(start + 64) <= count; start++) {
        if(s_bits[start] != 1) continue;
        if(kyber_em4100_frame(start, count, addr6)) return true;
    }
    return false;
}

static bool kyber_em4100_halves(uint16_t half_count, uint32_t* addr6) {
    if(half_count < 80) return false;
    for(uint8_t pol = 0; pol < 2; pol++) {
        for(uint8_t phase = 0; phase < 2; phase++) {
            uint16_t count = 0;
            uint16_t i = phase;
            while((uint16_t)(i + 1) < half_count && count < KYBER_BIT_MAX) {
                uint8_t first = s_halves[i];
                uint8_t second = s_halves[i + 1];
                if(first == second) {
                    i = (uint16_t)(i + 2);
                    continue;
                }
                bool high_then_low = (first == 1);
                bool one = (pol == 0) ? high_then_low : !high_then_low;
                s_bits[count++] = one ? 1 : 0;
                i = (uint16_t)(i + 2);
            }
            if(kyber_scan_em4100(count, addr6)) return true;
        }
    }
    return false;
}

static bool kyber_demod_em4100(uint32_t* addr6) {
    uint16_t pulses = s_pulse_count;
    uint16_t half_count = 0;
    for(uint16_t pulse = 0; pulse < pulses; pulse++) {
        uint8_t n = kyber_halves_for(s_pulses[pulse].us);
        if(n == 0xFF) continue;
        if(n == 0) {
            if(kyber_em4100_halves(half_count, addr6)) return true;
            half_count = 0;
            continue;
        }
        if((uint16_t)(half_count + n) > KYBER_HALF_MAX) {
            if(kyber_em4100_halves(half_count, addr6)) return true;
            half_count = 0;
        }
        uint8_t level = s_pulses[pulse].high ? 1 : 0;
        for(uint8_t h = 0; h < n; h++) s_halves[half_count++] = level;
    }
    return kyber_em4100_halves(half_count, addr6);
}

static void kyber_capture_reset(void) {
    s_pulse_count = 0;
    s_raw_count = 0;
    s_min_us = 0xFFFFFFFF;
    s_have_high = false;
    s_ignore_next = false;
}

// Proxmark waits this long after SendForward, inside the 586 us pause,
// and only then samples. 6000 samples at the 125 kHz carrier is 48 ms.
#define KYBER_REPLY_WAIT_US  250
#define KYBER_LISTEN_MS      80
#define KYBER_RETRY_GAP_MS   5
// One EM4100 frame is about 32 ms. This covers a frame that starts mid-listen.
#define KYBER_BROADCAST_MS 60
// Silence this many broadcasts in a row means the crystal left the field.
#define KYBER_SILENT_LIMIT 16

// Cleared when the read scene is left, so a listen on the worker thread
// can end without waiting out the whole window.
static volatile bool s_read_alive;

// False ends the wait early. Capture is stopped by the caller either way.
static bool kyber_wait_ms(uint16_t ms) {
    uint16_t left = ms;
    while(left > 0) {
        if(!s_read_alive) return false;
        uint16_t step = left > 10 ? 10 : left;
        furi_delay_ms(step);
        left = (uint16_t)(left - step);
    }
    return s_read_alive;
}

// A wayfinder keeps the field up and sends Read Word twice: address 6, then
// address 9. Address 6 is already known from the EM4100 broadcast, so that
// reply has to match before address 9 is trusted.
//
// Call this only while TIM1 is already the field that just decoded that ID.
// Do not start a second carrier. The pull stays grounded, which is how that
// ID was heard. The comparator is started after the command, with the
// carrier already back on.
static bool kyber_command_read(uint8_t address, uint32_t* word) {
    FURI_CRITICAL_ENTER();
    kyber_em_field_stop();
    kyber_em_bit(false);
    kyber_em_opcode(EM4x05_OPCODE_READ);
    kyber_em_address(address);
    furi_delay_us(KYBER_REPLY_WAIT_US);
    FURI_CRITICAL_EXIT();

    // 53 bits at RF/64 are about 27 ms, then the chip returns to addresses
    // 5 and 6. 80 ms covers that and a slow front-end recovery.
    kyber_capture_reset();
    furi_hal_rfid_tim_read_capture_start(kyber_capture, NULL);
    bool full = kyber_wait_ms(KYBER_LISTEN_MS);
    furi_hal_rfid_tim_read_capture_stop();
    if(!full) return false;
    return kyber_demod_pulses(word);
}

// Listen on the carrier that is already running. No command.
static void kyber_listen(uint16_t ms) {
    kyber_capture_reset();
    furi_hal_rfid_tim_read_capture_start(kyber_capture, NULL);
    kyber_wait_ms(ms);
    furi_hal_rfid_tim_read_capture_stop();
}

// Half-bits and doubled half-bits land in real. Spikes and the 4 us lows
// from a chattering comparator land in tiny. Gaps are neither.
static void kyber_count_shape(uint16_t* real, uint16_t* tiny) {
    uint16_t real_n = 0;
    uint16_t tiny_n = 0;
    uint16_t total = s_pulse_count;
    for(uint16_t i = 0; i < total; i++) {
        uint16_t us = s_pulses[i].us;
        if(us >= 160 && us <= 1400) real_n++;
        else if(us < 160) tiny_n++;
    }
    *real = real_n;
    *tiny = tiny_n;
}

// Here: the default broadcast. Silent: the crystal is gone. Anything else
// is the spike chatter a carrier break leaves behind, which is not a reason
// to break the carrier again or to clear the screen.
typedef enum {
    KyberAirSilent,
    KyberAirChatter,
    KyberAirHere,
} KyberAir;

static KyberAir kyber_air(void) {
    uint16_t real = 0;
    uint16_t tiny = 0;
    kyber_count_shape(&real, &tiny);
    if(real >= 16 && real > tiny) return KyberAirHere;
    if((uint16_t)(real + tiny) < 8) return KyberAirSilent;
    return KyberAirChatter;
}

static bool kyber_modulation_ok(void) {
    return kyber_air() == KyberAirHere;
}

// What the screen is showing. The worker fills a slot, then publishes the
// index. The GUI copies the published slot and does not read the other one.
typedef struct {
    bool present;
    bool have_addr6;
    bool have_addr9;
    uint32_t addr6;
    uint32_t addr9;
} KyberLiveView;

static KyberLiveView s_live_slot[2];
static volatile uint8_t s_live_slot_i;
static KyberLiveView s_live_last;

static bool kyber_view_same(const KyberLiveView* a, const KyberLiveView* b) {
    return a->present == b->present && a->have_addr6 == b->have_addr6 &&
           a->have_addr9 == b->have_addr9 && a->addr6 == b->addr6 && a->addr9 == b->addr9;
}

static void kyber_view_clear(KyberLiveView* view) {
    view->present = false;
    view->have_addr6 = false;
    view->have_addr9 = false;
    view->addr6 = 0;
    view->addr9 = 0;
}

// Post only when the picture changed. Back clears s_read_alive, and the
// scene is already leaving, so a late event is dropped.
static void kyber_publish(KyberApp* app, const KyberLiveView* view) {
    if(kyber_view_same(view, &s_live_last)) return;
    s_live_last = *view;
    uint8_t next = (uint8_t)(s_live_slot_i ^ 1);
    s_live_slot[next] = *view;
    s_live_slot_i = next;
    if(s_read_alive) {
        view_dispatcher_send_custom_event(app->view_dispatcher, KyberEventReadWord);
    }
}

// The worker's TIM1 is still the field that decoded the EM4100 ID, and its
// pull is still low. Stop only the capture timer. Do not call tim_read_start,
// tim_read_stop, or pins_reset: that second session heard nothing on hardware.
//
// capture_stop disables the TIM2 bus, and the worker's own capture_stop
// crashes unless that bus is enabled. Leave a capture running on return.
static void kyber_capture_handoff(void) {
    furi_hal_rfid_tim_read_capture_start(kyber_capture, NULL);
}

// One address 6, then address 9. A second try happens only when that listen
// was real half-bits. Spike chatter does not get another carrier break.
// An all-zero address 9 is a successful read. A different known address 6
// is reported through other_addr6 and is not treated as this crystal.
static bool kyber_try_pair(uint32_t known_addr6, uint32_t* addr9, uint32_t* other_addr6) {
    *other_addr6 = 0;
    uint32_t addr6 = 0;
    bool got6 = kyber_command_read(6, &addr6);
    bool match6 = got6 && addr6 == known_addr6;
    if(!match6 && got6 && addr6 != known_addr6 && kyber_match_series1(addr6) >= 0) {
        *other_addr6 = addr6;
        return false;
    }
    if(!match6 && s_read_alive && kyber_modulation_ok()) {
        furi_delay_ms(KYBER_RETRY_GAP_MS);
        got6 = kyber_command_read(6, &addr6);
        match6 = got6 && addr6 == known_addr6;
        if(!match6 && got6 && addr6 != known_addr6 && kyber_match_series1(addr6) >= 0) {
            *other_addr6 = addr6;
            return false;
        }
    }
    if(!s_read_alive || !match6) return false;

    if(kyber_command_read(9, addr9)) return true;
    if(!s_read_alive || !kyber_modulation_ok()) return false;
    furi_delay_ms(KYBER_RETRY_GAP_MS);
    return kyber_command_read(9, addr9);
}

static void kyber_note_other(KyberApp* app, KyberLiveView* view, uint32_t other) {
    view->present = true;
    view->have_addr6 = true;
    view->addr6 = other;
    view->have_addr9 = false;
    view->addr9 = 0;
    kyber_publish(app, view);
}

static void kyber_fill_addr9(KyberApp* app, KyberLiveView* view) {
    uint32_t addr9 = 0;
    uint32_t other = 0;
    if(kyber_try_pair(view->addr6, &addr9, &other)) {
        view->present = true;
        view->have_addr9 = true;
        view->addr9 = addr9;
        kyber_publish(app, view);
    } else if(other != 0) {
        kyber_note_other(app, view, other);
    }
}

// Stay on this field until Back. Color and the series 1 voice come from the
// broadcast. Address 9 is asked for when that broadcast is actually audible,
// and the screen updates in place. Silence clears it. Chatter does not.
static void kyber_poll(KyberApp* app, KyberLiveView view) {
    furi_hal_rfid_tim_read_capture_stop();

    if(s_read_alive && view.have_addr6 && !view.have_addr9) {
        kyber_fill_addr9(app, &view);
    }

    uint8_t silent = 0;
    while(s_read_alive) {
        kyber_listen(KYBER_BROADCAST_MS);
        if(!s_read_alive) break;

        KyberAir air = kyber_air();
        if(air == KyberAirSilent) {
            if(silent < 255) silent++;
            if(silent >= KYBER_SILENT_LIMIT && view.present) {
                kyber_view_clear(&view);
                kyber_publish(app, &view);
            }
            continue;
        }
        silent = 0;
        if(air != KyberAirHere) continue;

        uint32_t heard6 = 0;
        bool heard = kyber_demod_em4100(&heard6);
        if(heard && (!view.have_addr6 || heard6 != view.addr6)) {
            view.present = true;
            view.have_addr6 = true;
            view.addr6 = heard6;
            view.have_addr9 = false;
            view.addr9 = 0;
            kyber_publish(app, &view);
        } else if(!view.present && view.have_addr6) {
            view.present = true;
            kyber_publish(app, &view);
        }
        if(!s_read_alive || !view.have_addr6 || view.have_addr9) continue;
        kyber_fill_addr9(app, &view);
    }

    kyber_capture_handoff();
}

// ── Widgets ───────────────────────────────────────────────────────────────────

// "Palpatine/Qui-Gon" keeps Qui-Gon. "Vader/Yoda" on a red or black crystal
// keeps Vader. The other half is the holocron-alignment voice.
void kyber_holocron_voice(int index, char* voice, size_t voice_size) {
    voice[0] = '\0';
    if(index < 0) return;

    char color[24];
    char full[48];
    kyber_split_name(KYBER_CRYSTALS[index].name, color, sizeof(color), full, sizeof(full));

    const char* picked = full;
    char first[40];
    const char* slash = strchr(full, '/');
    if(slash) {
        size_t first_len = (size_t)(slash - full);
        if(first_len >= sizeof(first)) first_len = sizeof(first) - 1;
        memcpy(first, full, first_len);
        first[first_len] = '\0';
        const char* second = slash + 1;
        bool yoda = strncmp(second, "Yoda", 4) == 0;
        bool dark = strcmp(color, "Red") == 0 || strcmp(color, "Black") == 0;
        picked = (yoda && dark) ? first : second;
    }
    if(!picked[0]) picked = "none";
    size_t len = strlen(picked);
    if(len >= voice_size) len = voice_size - 1;
    memcpy(voice, picked, len);
    voice[len] = '\0';
}

void kyber_color_name(int index, char* color, size_t color_size) {
    color[0] = '\0';
    if(index < 0) return;
    char voice[40];
    kyber_split_name(KYBER_CRYSTALS[index].name, color, color_size, voice, sizeof(voice));
}

static void show_waiting(KyberApp* app) {
    widget_reset(app->read_widget);
    widget_add_string_element(
        app->read_widget, 64, 10, AlignCenter, AlignCenter, FontPrimary, "Read Crystal");
    widget_add_string_element(
        app->read_widget, 64, 32, AlignCenter, AlignCenter, FontSecondary, "Hold still");
    widget_add_string_element(
        app->read_widget, 64, 48, AlignCenter, AlignCenter, FontSecondary, "Back to cancel");
}

static bool protocol_is_em4100(ProtocolId protocol) {
    return protocol == LFRFIDProtocolEM4100 || protocol == LFRFIDProtocolEM4100_32 ||
           protocol == LFRFIDProtocolEM4100_16;
}

static void show_line(Widget* widget, uint8_t y, Font font, const char* text) {
    widget_add_string_element(widget, 64, y, AlignCenter, AlignCenter, font, text);
}

static void kyber_copy_str(char* out, size_t size, const char* text) {
    size_t n = 0;
    if(!text) text = "";
    while(text[n] && (n + 1) < size) {
        out[n] = text[n];
        n++;
    }
    out[n] = '\0';
}

static void kyber_label_line(char* out, size_t size, const char* prefix, const char* voice) {
    kyber_copy_str(out, size, prefix);
    size_t n = 0;
    while(out[n]) n++;
    size_t i = 0;
    if(!voice) voice = "";
    while(voice[i] && (n + 1) < size) out[n++] = voice[i++];
    out[n] = '\0';
}

// Drawn on the GUI thread from the slot the worker published.
static void show_live(KyberApp* app) {
    uint8_t slot = s_live_slot_i;
    KyberLiveView view = s_live_slot[slot];
    if(!view.present || !view.have_addr6) {
        show_waiting(app);
        set_rgb_led(app, 0, 0, 40);
        return;
    }

    int s1 = kyber_match_series1(view.addr6);
    int s2 = view.have_addr9 ? kyber_match_addr9(view.addr9) : -1;
    int color_index = (s2 >= 0) ? s2 : s1;

    char color[24];
    char s1_voice[32];
    char s2_voice[32];
    color[0] = '\0';
    s1_voice[0] = '\0';
    s2_voice[0] = '\0';
    kyber_color_name(color_index, color, sizeof(color));
    kyber_holocron_voice(s1, s1_voice, sizeof(s1_voice));
    if(!s1_voice[0]) kyber_copy_str(s1_voice, sizeof(s1_voice), "none");

    Font s1_font = FontSecondary;
    Font s2_font = FontSecondary;
    const char* s2_text = "...";
    // Series 1 only when address 9 was actually read as zero.
    if(view.have_addr9 && view.addr9 == 0) {
        kyber_copy_str(s2_voice, sizeof(s2_voice), s1_voice);
        s2_text = s2_voice;
        s1_font = FontPrimary;
    } else if(s2 >= 0) {
        kyber_holocron_voice(s2, s2_voice, sizeof(s2_voice));
        s2_text = s2_voice;
        s2_font = FontPrimary;
    } else if(view.have_addr9) {
        s2_text = "unknown";
    }

    char s1_line[40];
    char s2_line[40];
    kyber_label_line(s1_line, sizeof(s1_line), "S1  ", s1_voice);
    kyber_label_line(s2_line, sizeof(s2_line), "S2  ", s2_text);

    widget_reset(app->read_widget);
    show_line(app->read_widget, 14, FontPrimary, color[0] ? color : "Unknown");
    show_line(app->read_widget, 34, s1_font, s1_line);
    show_line(app->read_widget, 52, s2_font, s2_line);

    if(color_index >= 0) set_crystal_led(app, color_index);
    else clear_led(app);
}

static void show_rejected(KyberApp* app) {
    widget_reset(app->read_widget);
    show_line(app->read_widget, 10, FontPrimary, "Not a Kyber Crystal");
    show_line(app->read_widget, 32, FontSecondary, "EM4100 tag not detected");
    show_line(app->read_widget, 48, FontSecondary, "Back to cancel");
    clear_led(app);
}

// ── Read address 9 on the field that just heard the crystal ──────────────────
// SenseCardStart runs on the worker thread while TIM1 is still up. The poll
// stays there until Back, and posts a new picture whenever the crystal or
// address 9 changes. Widgets are updated on the GUI thread. ReadDone is only
// the fallback for a tag that never took the EM4100 path.
static volatile bool s_word_done;
static volatile bool s_live_busy;
static LFRFIDWriteRequest s_live_request;

// Stop only sets a flag. Wait until the word read has returned, then a little
// longer so the worker can drop the carrier and go idle. read_start crashes
// if it is called before that.
static void kyber_wait_worker_idle(KyberApp* app) {
    s_read_alive = false;
    lfrfid_worker_stop(app->rfid_worker);
    for(uint8_t i = 0; i < 100 && s_live_busy; i++) furi_delay_ms(10);
    furi_delay_ms(30);
}

// Runs on the worker thread. Do not touch widgets here.
static void read_callback(LFRFIDWorkerReadResult result, ProtocolId protocol, void* context) {
    furi_assert(context);
    KyberApp* app = context;

    if(result == LFRFIDWorkerReadSenseCardStart) {
        if(s_word_done || !protocol_is_em4100(protocol)) return;
        s_word_done = true;
        s_live_busy = true;

        memset(&s_live_request, 0, sizeof(s_live_request));
        s_live_request.write_type = LFRFIDWriteTypeEM4305;
        if(protocol_dict_get_write_data(
               app->protocol_dict, (size_t)protocol, &s_live_request)) {
            KyberLiveView view;
            kyber_view_clear(&view);
            view.present = true;
            view.have_addr6 = true;
            view.addr6 = s_live_request.em4305.word[6];
            // Color and the series 1 voice, before the address 9 attempt.
            kyber_publish(app, &view);
            kyber_poll(app, view);
        }

        // Do not join. This callback is that thread.
        lfrfid_worker_stop(app->rfid_worker);
        s_live_busy = false;
        return;
    }

    if(result == LFRFIDWorkerReadDone && !s_word_done && s_read_alive) {
        scene_manager_set_scene_state(app->scene_manager, KyberSceneRead, (uint32_t)protocol);
        view_dispatcher_send_custom_event(app->view_dispatcher, KyberEventReadDone);
    }
}

static void start_identify(KyberApp* app) {
    kyber_wait_worker_idle(app);
    s_word_done = false;
    kyber_view_clear(&s_live_last);
    kyber_view_clear(&s_live_slot[0]);
    kyber_view_clear(&s_live_slot[1]);
    s_live_slot_i = 0;
    s_read_alive = true;
    show_waiting(app);
    set_rgb_led(app, 0, 0, 40);
    scene_manager_set_scene_state(app->scene_manager, KyberSceneRead, PROTOCOL_NO);
    lfrfid_worker_read_start(app->rfid_worker, LFRFIDWorkerReadTypeASKOnly, read_callback, app);
}

// ── Scene ─────────────────────────────────────────────────────────────────────
void kyberwrite_scene_read_on_enter(void* context) {
    furi_assert(context);
    KyberApp* app = context;

    view_dispatcher_switch_to_view(app->view_dispatcher, KyberViewRead);
    notification_message(app->notifications, &sequence_display_backlight_enforce_on);
    start_identify(app);
}

bool kyberwrite_scene_read_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    KyberApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == KyberEventReadDone && !s_word_done) {
            lfrfid_worker_stop(app->rfid_worker);
            show_rejected(app);
            consumed = true;
        } else if(event.event == KyberEventReadWord) {
            show_live(app);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        s_read_alive = false;
        clear_led(app);
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, KyberSceneMenu);
        consumed = true;
    }
    return consumed;
}

void kyberwrite_scene_read_on_exit(void* context) {
    furi_assert(context);
    KyberApp* app = context;

    kyber_wait_worker_idle(app);
    clear_led(app);
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
    widget_reset(app->read_widget);
}
