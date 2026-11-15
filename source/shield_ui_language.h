#ifndef SHIELD_UI_LANGUAGE_H
#define SHIELD_UI_LANGUAGE_H

#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Stores only the user's UI language choice; it never reads console IDs. */
typedef struct {
    bool french;
    bool confirmation_needed;
    const char *config_path;
} ShieldUiLanguage;

static inline void shield_ui_language_load(ShieldUiLanguage *state,
                                           const char *config_path,
                                           bool cfgu_already_ready) {
    if (!state) return;
    state->french = false;
    state->confirmation_needed = false;
    state->config_path = config_path;

    FILE *file = config_path ? fopen(config_path, "rb") : NULL;
    if (file) {
        int saved = fgetc(file);
        fclose(file);
        if (saved == 'F' || saved == 'E') {
            state->french = saved == 'F';
            return;
        }
    }

    /* The system language enum is the only console setting read here. */
    u8 system_language = CFG_LANGUAGE_EN;
    bool close_cfgu = false;
    bool language_read = false;
    if (cfgu_already_ready) {
        language_read = R_SUCCEEDED(CFGU_GetSystemLanguage(&system_language));
    } else if (R_SUCCEEDED(cfguInit())) {
        close_cfgu = true;
        language_read = R_SUCCEEDED(CFGU_GetSystemLanguage(&system_language));
    }
    if (close_cfgu) cfguExit();
    state->french = language_read && system_language == CFG_LANGUAGE_FR;
    state->confirmation_needed = true;
}

static inline void shield_ui_language_save(const ShieldUiLanguage *state) {
    if (!state || !state->config_path) return;
    mkdir("sdmc:/3ds", 0777);
    char parent[160];
    size_t length = strlen(state->config_path);
    if (length >= sizeof(parent)) return;
    memcpy(parent, state->config_path, length + 1);
    char *slash = strrchr(parent, '/');
    if (slash && slash > parent + 5) {
        *slash = '\0';
        mkdir(parent, 0777);
    }
    FILE *file = fopen(state->config_path, "wb");
    if (!file) return;
    fputc(state->french ? 'F' : 'E', file);
    fclose(file);
}

static inline void shield_ui_language_choose(ShieldUiLanguage *state,
                                              bool accept_detected_language) {
    if (!state) return;
    if (!accept_detected_language) state->french = !state->french;
    state->confirmation_needed = false;
    shield_ui_language_save(state);
}

#endif
