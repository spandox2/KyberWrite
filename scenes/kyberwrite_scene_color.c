#include "kyberwrite_scene.h"
#include <string.h>

// Shown colors, including the two cracked effects. Cracked Blue is blue with
// a red core, Cracked Red is red with a blue core. Each is its own address 9.
static const char* const s_colors[] = {
    "Black",
    "Red",
    "Cracked Red",
    "Orange",
    "Yellow",
    "Green",
    "Cyan",
    "Blue",
    "Cracked Blue",
    "Purple",
    "White",
};

#define KYBER_COLOR_COUNT (sizeof(s_colors) / sizeof(s_colors[0]))

static void color_callback(void* context, uint32_t index);

static bool crystal_is_color(uint8_t crystal, const char* color) {
    char name[24];
    kyber_color_name(crystal, name, sizeof(name));
    return strcmp(name, color) == 0;
}

static void color_show_colors(KyberApp* app) {
    submenu_reset(app->color_menu);
    submenu_set_header(app->color_menu, "Write Crystal");

    for(uint8_t i = 0; i < KYBER_COLOR_COUNT; i++) {
        bool any = false;
        for(uint8_t crystal = 0; crystal < KYBER_CRYSTAL_COUNT; crystal++) {
            if(crystal_is_color(crystal, s_colors[i])) {
                any = true;
                break;
            }
        }
        if(!any) continue;
        submenu_add_item(app->color_menu, s_colors[i], i, color_callback, app);
    }
}

static bool label_used(char used[][24], uint8_t count, const char* label) {
    for(uint8_t i = 0; i < count; i++) {
        if(strcmp(used[i], label) == 0) return true;
    }
    return false;
}

static void color_show_voices(KyberApp* app) {
    uint32_t color_index =
        scene_manager_get_scene_state(app->scene_manager, KyberSceneColorSelect);
    if(color_index >= KYBER_COLOR_COUNT) color_index = 0;
    const char* color = s_colors[color_index];

    submenu_reset(app->color_menu);
    submenu_set_header(app->color_menu, color);

    // Series 2 personalities first. Series 1 voices follow. Choosing one of
    // those writes address 9 as zero, so a series 2 holocron speaks it.
    char used[12][24];
    uint8_t used_count = 0;

    for(uint8_t pass = 0; pass < 2; pass++) {
        bool want_series2 = (pass == 0);
        for(uint8_t crystal = 0; crystal < KYBER_CRYSTAL_COUNT; crystal++) {
            bool series2 = KYBER_CRYSTALS[crystal].addr9 != 0;
            if(series2 != want_series2) continue;
            if(!crystal_is_color(crystal, color)) continue;

            char voice[24];
            kyber_holocron_voice(crystal, voice, sizeof(voice));
            if(!voice[0]) {
                memcpy(voice, "none", 5);
            }

            char label[28];
            if(label_used(used, used_count, voice)) {
                snprintf(label, sizeof(label), "%s S1", voice);
            } else {
                size_t len = strlen(voice);
                if(len >= sizeof(label)) len = sizeof(label) - 1;
                memcpy(label, voice, len);
                label[len] = '\0';
            }

            if(used_count < 12) {
                size_t len = strlen(label);
                if(len >= sizeof(used[0])) len = sizeof(used[0]) - 1;
                memcpy(used[used_count], label, len);
                used[used_count][len] = '\0';
                used_count++;
            }

            submenu_add_item(app->color_menu, label, crystal, color_callback, app);
        }
    }
}

static void color_callback(void* context, uint32_t index) {
    furi_assert(context);
    KyberApp* app = context;

    if(!app->color_showing_crystals) {
        scene_manager_set_scene_state(app->scene_manager, KyberSceneColorSelect, index);
        view_dispatcher_send_custom_event(app->view_dispatcher, KyberEventSeriesSelected);
        return;
    }

    app->crystal_index = (uint8_t)index;
    scene_manager_handle_custom_event(app->scene_manager, KyberEventColorSelected);
}

void kyberwrite_scene_color_on_enter(void* context) {
    furi_assert(context);
    KyberApp* app = context;

    app->color_showing_crystals = false;
    color_show_colors(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, KyberViewColorSelect);
}

bool kyberwrite_scene_color_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    KyberApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == KyberEventSeriesSelected) {
            app->color_showing_crystals = true;
            color_show_voices(app);
            consumed = true;
        } else if(event.event == KyberEventColorSelected) {
            scene_manager_next_scene(app->scene_manager, KyberSceneWriting);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack && app->color_showing_crystals) {
        app->color_showing_crystals = false;
        color_show_colors(app);
        consumed = true;
    }
    return consumed;
}

void kyberwrite_scene_color_on_exit(void* context) {
    furi_assert(context);
    KyberApp* app = context;
    app->color_showing_crystals = false;
    submenu_reset(app->color_menu);
}
