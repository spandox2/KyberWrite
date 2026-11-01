#include "kyberwrite_scene.h"
#include <notification/notification_messages.h>

static void build_write_data(KyberApp* app, LFRFIDEM4305* out) {
    const KyberCrystal* crystal = &KYBER_CRYSTALS[app->crystal_index];

    for(uint8_t i = 0; i < EM4x05_WORD_COUNT; i++) {
        out->word[i] = KYBER_BASE_DATA[i];
    }
    // Address 5 selects the EM4100 prefix, 6 the color, 9 the series 2 voice.
    // Series 1 leaves address 9 at zero so series 2 devices fall back to address 6.
    out->word[5] = kyber_crystal_addr5(crystal);
    out->word[6] = crystal->addr6;
    out->word[9] = crystal->addr9;

    // Full image every time. Address 2 is the password and stays untouched.
    out->mask = 0xFFFF & ~(1u << 2);
}

// Words 5 and 6 are the EM4100 frame, first transmitted bit in the low bit.
// Returns the 5-byte ID a reader reports for that frame.
static bool kyber_em4100_id(uint32_t addr5, uint32_t addr6, uint8_t out[5]) {
    uint8_t frame[64];
    for(uint8_t i = 0; i < 32; i++) {
        frame[i] = (addr5 >> i) & 1;
        frame[32 + i] = (addr6 >> i) & 1;
    }

    for(uint8_t i = 0; i < 9; i++) {
        if(!frame[i]) return false;
    }

    uint8_t nibbles[10];
    for(uint8_t row = 0; row < 10; row++) {
        uint8_t base = 9 + (row * 5);
        uint8_t nibble = 0;
        uint8_t parity = 0;
        for(uint8_t bit = 0; bit < 4; bit++) {
            nibble = (uint8_t)((nibble << 1) | frame[base + bit]);
            parity ^= frame[base + bit];
        }
        if(parity != frame[base + 4]) return false;
        nibbles[row] = nibble;
    }

    for(uint8_t i = 0; i < 5; i++) {
        out[i] = (uint8_t)((nibbles[i * 2] << 4) | nibbles[i * 2 + 1]);
    }
    return true;
}

static void show_status(KyberApp* app, const char* status, const char* footer) {
    widget_reset(app->writing_widget);
    widget_add_string_element(
        app->writing_widget, 64, 10, AlignCenter, AlignCenter, FontPrimary, "KyberWrite");
    widget_add_string_element(
        app->writing_widget, 64, 26, AlignCenter, AlignCenter, FontSecondary, status);
    widget_add_string_element(
        app->writing_widget,
        64,
        40,
        AlignCenter,
        AlignCenter,
        FontSecondary,
        KYBER_CRYSTALS[app->crystal_index].name);
    widget_add_string_element(
        app->writing_widget, 64, 54, AlignCenter, AlignCenter, FontSecondary, footer);
}

// Wait until a tag answers, program it, then accept the result only if a
// read-back reports the EM4100 ID those words produce. em4305_write itself
// never checks that a crystal is in the field.
typedef enum {
    KyberWritePhaseWait,
    KyberWritePhaseProgram,
    KyberWritePhaseVerify,
} KyberWritePhase;

static bool protocol_is_em4100(ProtocolId protocol) {
    return protocol == LFRFIDProtocolEM4100 || protocol == LFRFIDProtocolEM4100_32 ||
           protocol == LFRFIDProtocolEM4100_16;
}

// Address 9 is not in the EM4100 broadcast. Matching words 5 and 6 is the
// check that the chip answered with the frame we just tried to write.
static bool readback_matches(KyberApp* app) {
    if(!protocol_is_em4100(app->detected_protocol)) return false;

    const KyberCrystal* crystal = &KYBER_CRYSTALS[app->crystal_index];
    uint8_t expected[5];
    if(!kyber_em4100_id(kyber_crystal_addr5(crystal), crystal->addr6, expected)) return false;

    size_t id_size = protocol_dict_get_data_size(app->protocol_dict, (size_t)app->detected_protocol);
    if(id_size < 5) return false;

    uint8_t id[5];
    protocol_dict_get_data(app->protocol_dict, (size_t)app->detected_protocol, id, 5);
    for(uint8_t i = 0; i < 5; i++) {
        if(id[i] != expected[i]) return false;
    }
    return true;
}

// Runs on the worker thread. The read mode is still active here, so this
// only records the protocol and wakes the GUI thread.
static void write_read_callback(LFRFIDWorkerReadResult result, ProtocolId protocol, void* context) {
    furi_assert(context);
    KyberApp* app = context;
    if(result != LFRFIDWorkerReadDone) return;
    app->detected_protocol = protocol;
    view_dispatcher_send_custom_event(app->view_dispatcher, KyberEventReadDone);
}

// lfrfid_worker_read_start crashes unless the worker is idle. Stop only
// sets a flag, and the read callback runs before that flag is handled.
static void start_listen(KyberApp* app) {
    lfrfid_worker_stop(app->rfid_worker);
    furi_delay_ms(20);
    lfrfid_worker_read_start(
        app->rfid_worker, LFRFIDWorkerReadTypeASKOnly, write_read_callback, app);
}

static FuriTimer* write_timer(KyberApp* app) {
    return (FuriTimer*)(uintptr_t)scene_manager_get_scene_state(
        app->scene_manager, KyberSceneWriting);
}

// One shot, so the status line is on screen before em4305_write blocks the
// GUI thread. The write uses FURI_CRITICAL_ENTER and must run there.
static void write_timer_callback(void* context) {
    furi_assert(context);
    KyberApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, KyberEventWriteStepDone);
}

static void begin_wait(KyberApp* app, const char* status) {
    app->write_phase = KyberWritePhaseWait;
    show_status(app, status, "Back to cancel");
    notification_message(app->notifications, &sequence_blink_start_blue);
    start_listen(app);
}

void kyberwrite_scene_writing_on_enter(void* context) {
    furi_assert(context);
    KyberApp* app = context;

    app->write_success = false;
    app->detected_protocol = PROTOCOL_NO;

    FuriTimer* timer = furi_timer_alloc(write_timer_callback, FuriTimerTypeOnce, app);
    scene_manager_set_scene_state(
        app->scene_manager, KyberSceneWriting, (uint32_t)(uintptr_t)timer);

    view_dispatcher_switch_to_view(app->view_dispatcher, KyberViewWriting);
    begin_wait(app, "Waiting for crystal");
}

bool kyberwrite_scene_writing_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    KyberApp* app = context;
    bool consumed = false;

    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == KyberEventReadDone && app->write_phase == KyberWritePhaseWait) {
        // Any decoded tag counts as presence, including a blank FDX-B chip.
        lfrfid_worker_stop(app->rfid_worker);
        app->write_phase = KyberWritePhaseProgram;
        show_status(app, "Writing, hold still", "Keep it on Flipper");
        furi_timer_start(write_timer(app), 200);
        consumed = true;
    } else if(event.event == KyberEventWriteStepDone && app->write_phase == KyberWritePhaseProgram) {
        LFRFIDEM4305 data;
        build_write_data(app, &data);
        em4305_write(&data);

        app->write_phase = KyberWritePhaseVerify;
        show_status(app, "Reading it back", "Keep it on Flipper");
        start_listen(app);
        consumed = true;
    } else if(event.event == KyberEventReadDone && app->write_phase == KyberWritePhaseVerify) {
        lfrfid_worker_stop(app->rfid_worker);
        if(readback_matches(app)) {
            app->write_success = true;
            notification_message(app->notifications, &sequence_blink_stop);
            notification_message(app->notifications, &sequence_success);
            scene_manager_next_scene(app->scene_manager, KyberSceneResult);
        } else {
            notification_message(app->notifications, &sequence_error);
            begin_wait(app, "Missed, hold still");
        }
        consumed = true;
    }

    return consumed;
}

void kyberwrite_scene_writing_on_exit(void* context) {
    furi_assert(context);
    KyberApp* app = context;

    FuriTimer* timer = write_timer(app);
    if(timer) {
        furi_timer_stop(timer);
        furi_timer_free(timer);
        scene_manager_set_scene_state(app->scene_manager, KyberSceneWriting, 0);
    }

    lfrfid_worker_stop(app->rfid_worker);
    app->write_phase = KyberWritePhaseWait;
    notification_message(app->notifications, &sequence_blink_stop);
    widget_reset(app->writing_widget);
}
