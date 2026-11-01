#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/popup.h>
#include <gui/modules/widget.h>
#include <notification/notification_messages.h>
#include <lib/lfrfid/lfrfid_worker.h>
#include <lib/lfrfid/protocols/lfrfid_protocols.h>
#include <lib/lfrfid/tools/em4305.h>
#include <toolbox/protocols/protocol_dict.h>

// Address 6 is the series 1 color/voice word, and the second half of the
// EM4100 frame. Address 9 is the series 2 identity (xx0D0000). Zero means
// series 1: series 2 holocrons and wayfinders then fall back to address 6.
// Address 5 is the front half of that frame. Series 2 crystals use
// 18C631FF, which broadcasts 1111000Cxx instead of the series 1 0000000Cxx.
// The color nibble stays in address 6 either way.
#define KYBER_ADDR5_SERIES1 0x000001FFu
#define KYBER_ADDR5_SERIES2 0x18C631FFu
#define KYBER_APP_VERSION   "0.20"

typedef struct {
    const char* name;
    uint32_t    addr6;
    uint32_t    addr9;
    uint8_t     r, g, b;
} KyberCrystal;

static inline uint32_t kyber_crystal_addr5(const KyberCrystal* crystal) {
    return (crystal->addr9 != 0) ? KYBER_ADDR5_SERIES2 : KYBER_ADDR5_SERIES1;
}

static const KyberCrystal KYBER_CRYSTALS[] = {
    // Series 1. Address 9 is empty so series 2 devices use address 6.
    { "White  (Palpatine/Ahsoka)",  0x18003000, 0x00000000, 200, 200, 200 },
    { "Red    (Vader/Yoda)",        0x5E003000, 0x00000000, 255,   0,   0 },
    { "Orange",                     0x3D003000, 0x00000000, 255,  60,   0 },
    { "Yellow (Palpatine/Guard)",   0x7B003000, 0x00000000, 220, 120,   0 },
    { "Green  (Palpatine/Qui-Gon)", 0x0C803000, 0x00000000,   0, 255,   0 },
    { "Cyan",                       0x4A803000, 0x00000000,   0, 220, 220 },
    { "Blue   (Palpatine/Obi-Wan)", 0x29803000, 0x00000000,   0,   0, 255 },
    { "Purple (Palpatine/Mace 1)",  0x6F803000, 0x00000000, 255,   0, 255 },
    { "White  (Palpatine/Chirrut)", 0x14403000, 0x00000000, 200, 200, 200 },
    { "Red    (Palpatine/Yoda)",    0x52403000, 0x00000000, 255,   0,   0 },
    { "Red    (Dooku/Yoda)",        0x31403000, 0x00000000, 255,   0,   0 },
    { "Yellow (Palpatine/Maz)",     0x77403000, 0x00000000, 220, 220,   0 },
    { "Green  (Palpatine/Yoda)",    0x00C03000, 0x00000000,   0, 255,   0 },
    { "Red    (Maul/Yoda)",         0x46C03000, 0x00000000, 255,   0,   0 },
    { "Blue   (Palpatine/Old Luke)",0x25C03000, 0x00000000,   0,   0, 255 },
    { "Purple (Palpatine/Mace 2)",  0x63C03000, 0x00000000, 255,   0, 255 },
    { "Red    (Vader 8-Ball/Yoda)", 0x3E183000, 0x00000000, 255,   0,   0 },
    { "Green  (Palpatine/Yoda 8-B)",0x5D183000, 0x00000000,   0, 255,   0 },
    { "Black  (Snoke/Yoda)",        0x1B183000, 0x00000000, 255, 125, 125 },

    // Series 2. Address 6 is the series 1 fallback for that color.
    // Wayfinder locations are in the comment.
    { "Black  (Darth Sidious)",     0x5E003000, 0x010D0000, 255, 125, 125 }, // Exegol
    { "Cracked Blue (Ben Solo)",    0x29803000, 0x020D0000,  90,  20, 255 }, // Kef Bir
    { "Blue   (Cal Kestis)",        0x29803000, 0x030D0000,   0,   0, 255 }, // Bracca
    { "Blue   (Plo Koon)",          0x29803000, 0x040D0000,   0,   0, 255 }, // Florrum
    { "Green  (Luke Skywalker)",    0x0C803000, 0x050D0000,   0, 255,   0 }, // Tatooine
    { "Purple (Grievous)",          0x6F803000, 0x060D0000, 255,   0, 255 }, // Vassek 3
    { "Purple (Mace Windu)",        0x6F803000, 0x070D0000, 255,   0, 255 }, // Bardotta
    { "Red    (Ventress)",          0x5E003000, 0x080D0000, 255,   0,   0 }, // Dathomir
    { "Cracked Red (Kylo Ren)",     0x5E003000, 0x090D0000, 255,   0,  60 }, // Crait
    { "Red    (Darth Bane)",        0x5E003000, 0x100D0000, 255,   0,   0 }, // Moraband
    { "Red    (Maul)",              0x5E003000, 0x110D0000, 255,   0,   0 }, // Mandalore
    { "Red    (Second Sister)",     0x5E003000, 0x120D0000, 255,   0,   0 }, // Nur
    { "Red    (Inquisitor)",        0x5E003000, 0x130D0000, 255,   0,   0 }, // Stygeon Prime
    { "White  (Krin Dagbard)",      0x18003000, 0x140D0000, 200, 200, 200 }, // Batuu
    { "White  (Orla Jareni)",       0x18003000, 0x150D0000, 200, 200, 200 }, // Umbara
    { "Yellow (Indeera Stokes)",    0x7B003000, 0x160D0000, 220, 180,   0 }, // Elphrona
    { "Yellow (Rey)",               0x7B003000, 0x170D0000, 220, 180,   0 }, // Tatooine
    { "Green  (Luminara)",          0x0C803000, 0x180D0000,   0, 255,   0 }, // Naboo
};

#define KYBER_CRYSTAL_COUNT (sizeof(KYBER_CRYSTALS) / sizeof(KYBER_CRYSTALS[0]))

void kyber_color_name(int index, char* color, size_t color_size);
void kyber_holocron_voice(int index, char* voice, size_t voice_size);

static const uint32_t KYBER_BASE_DATA[16] = {
    0x00040072, 0x6147FBB3, 0x00000000, 0x000064FC,
    0x0001805F, 0x000001FF, 0x0C803000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00008002, 0x00000000,
};

static const uint8_t KYBER_WRITE_ORDER[] = { 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 1 };
#define KYBER_WRITE_STEPS (sizeof(KYBER_WRITE_ORDER) / sizeof(KYBER_WRITE_ORDER[0]))

// ── Scene IDs ────────────────────────────────────────────────────────────────
typedef enum {
    KyberSceneSplash,
    KyberSceneHint,
    KyberSceneMenu,
    KyberSceneColorSelect,
    KyberSceneWriting,
    KyberSceneResult,
    KyberSceneRead,
    KyberSceneCount,
} KyberScene;

// ── View IDs ─────────────────────────────────────────────────────────────────
typedef enum {
    KyberViewSplash,
    KyberViewHint,
    KyberViewMenu,
    KyberViewColorSelect,
    KyberViewWriting,
    KyberViewResult,
    KyberViewRead,
    KyberViewCount,
} KyberView;

// ── App state ────────────────────────────────────────────────────────────────
typedef struct {
    Gui*             gui;
    ViewDispatcher*  view_dispatcher;
    SceneManager*    scene_manager;
    NotificationApp* notifications;

    // Views
    Submenu* menu;
    Submenu* color_menu;
    Widget*  writing_widget;
    Popup*   result_popup;
    Widget*  read_widget;
    Widget*  splash_widget;
    Widget*  hint_widget;

    // RFID
    LFRFIDWorker*  rfid_worker;
    ProtocolDict*  protocol_dict;

    // State
    uint8_t   crystal_index;
    bool      write_success;
    bool      show_hints;
    bool      color_showing_crystals;
    uint8_t   write_phase;
    ProtocolId detected_protocol;
    char*     hint_title;
    uint8_t   hint_text_lines;
    const char**    hint_text;
    KyberScene hint_next_scene;
} KyberApp;
