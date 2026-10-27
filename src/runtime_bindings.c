// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
/* Keyboard and mouse bindings (runtime_bindings.h). */
#include "runtime_bindings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <SDL3/SDL.h>

/* Inputs: SDL scancodes below INPUT_MOUSE, then the mouse buttons (SDL_BUTTON_LEFT .. X2) and the
 * wheel. 0 = an empty slot (SDL_SCANCODE_UNKNOWN). */
enum { INPUT_MOUSE=0x200, INPUT_WHEEL_UP=0x210, INPUT_WHEEL_DOWN };
static const struct { const char *name; uint16_t input; } mouse_names[]={
    {"Mouse Left",INPUT_MOUSE+SDL_BUTTON_LEFT}, {"Mouse Right",INPUT_MOUSE+SDL_BUTTON_RIGHT},
    {"Mouse Middle",INPUT_MOUSE+SDL_BUTTON_MIDDLE}, {"Mouse Back",INPUT_MOUSE+SDL_BUTTON_X1},
    {"Mouse Forward",INPUT_MOUSE+SDL_BUTTON_X2},
    {"Wheel Up",INPUT_WHEEL_UP}, {"Wheel Down",INPUT_WHEEL_DOWN},
};

/* The layout before bindings existed (0.5.9), so an ini without bind_ keys plays the same. */
static const struct { const char *key, *defaults; } actions[BIND_COUNT]={
    [BIND_MOVE_FORWARD]={"bind_move_forward","W"}, [BIND_MOVE_BACK]={"bind_move_back","S"},
    [BIND_MOVE_LEFT]={"bind_move_left","A"}, [BIND_MOVE_RIGHT]={"bind_move_right","D"},
    [BIND_WALK]={"bind_walk","Left Alt"},
    [BIND_CAMERA_UP]={"bind_camera_up","Up"}, [BIND_CAMERA_DOWN]={"bind_camera_down","Down"},
    [BIND_CAMERA_LEFT]={"bind_camera_left","Left"}, [BIND_CAMERA_RIGHT]={"bind_camera_right","Right"},
    [BIND_CROSS]={"bind_cross","Space,Return,Keypad Enter"}, [BIND_CIRCLE]={"bind_circle","Left Shift,Escape"},
    [BIND_SQUARE]={"bind_square","E"}, [BIND_TRIANGLE]={"bind_triangle","Q"},
    [BIND_R1]={"bind_r1","Mouse Left,3"}, [BIND_R2]={"bind_r2","Mouse Back,F"},
    [BIND_L1]={"bind_l1","1,Mouse Forward"}, [BIND_L2]={"bind_l2","Mouse Right,R"},
    [BIND_R3]={"bind_r3","Mouse Middle,C"}, [BIND_L3]={"bind_l3","Z"},
    [BIND_DPAD_UP]={"bind_dpad_up","I"}, [BIND_DPAD_DOWN]={"bind_dpad_down","K"},
    [BIND_DPAD_LEFT]={"bind_dpad_left","J"}, [BIND_DPAD_RIGHT]={"bind_dpad_right","L"},
    [BIND_OPTIONS]={"bind_options","O,F1"},
    [BIND_TOUCH_LEFT]={"bind_touch_left","Tab"}, [BIND_TOUCH_RIGHT]={"bind_touch_right","Backspace"},
};

static uint16_t bound[BIND_COUNT][BIND_SLOTS];
static bool camera_keys_dpad=true, use_wheel;
static int loaded;

static uint16_t parse_input(const char *name) {
    for (size_t i=0;i<sizeof(mouse_names)/sizeof(*mouse_names);++i)
        if (!strcasecmp(name,mouse_names[i].name)) return mouse_names[i].input;
    const SDL_Scancode code=SDL_GetScancodeFromName(name);
    if (code==SDL_SCANCODE_LGUI || code==SDL_SCANCODE_RGUI) return 0; /* Cmd is the Mac's */
    return code>SDL_SCANCODE_UNKNOWN && code<INPUT_MOUSE ? (uint16_t)code : 0;
}

/* "Space, Return" -> slots; names it does not know are logged and skipped. */
static void parse_list(int action, const char *list, int log) {
    memset(bound[action],0,sizeof(bound[action]));
    int slot=0;
    for (const char *p=list; *p && slot<BIND_SLOTS;) {
        const char *end=strchr(p,',');
        size_t n=end ? (size_t)(end-p) : strlen(p);
        while (n && isspace((unsigned char)*p)) { ++p; --n; }
        while (n && isspace((unsigned char)p[n-1])) --n;
        if (n && n<64) {
            char name[64];
            memcpy(name,p,n); name[n]=0;
            const uint16_t input=parse_input(name);
            if (input) bound[action][slot++]=input;
            else if (log) printf("Runtime: %s: ignored \"%s\" (not a key or mouse button the game can use)\n",actions[action].key,name);
        }
        if (!end) break;
        p=end+1;
    }
}

static void trim(char *s) {
    size_t n=strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n]=0;
    size_t lead=strspn(s," \t");
    if (lead) memmove(s,s+lead,strlen(s+lead)+1);
}

void runtime_bindings_load(const char *ini_path) {
    for (int a=0;a<BIND_COUNT;++a) parse_list(a,actions[a].defaults,0);
    camera_keys_dpad=true;
    loaded=1;
    FILE *f=ini_path ? fopen(ini_path,"r") : NULL;
    if (f) {
        char line[512];
        while (fgets(line,sizeof line,f)) {
            char *eq=strchr(line,'=');
            if (line[0]=='#' || !eq) continue;
            *eq=0;
            char *key=line, *value=eq+1;
            trim(key); trim(value);
            if (!strcmp(key,"camera_keys_dpad")) { camera_keys_dpad=atoi(value)!=0; continue; }
            for (int a=0;a<BIND_COUNT;++a) {
                if (strcmp(key,actions[a].key)) continue;
                parse_list(a,value,1);
                printf("Runtime: %s=%s\n",key,value);
            }
        }
        fclose(f);
    }
    use_wheel=false;
    for (int a=0;a<BIND_COUNT;++a)
        for (int s=0;s<BIND_SLOTS;++s) if (bound[a][s]==INPUT_WHEEL_UP || bound[a][s]==INPUT_WHEEL_DOWN) use_wheel=true;
}

static void load_once(void) {
    if (!loaded) runtime_bindings_load(getenv("BB_CONFIG"));
}

uint64_t runtime_bindings_held(const bool *keys, uint32_t mouse) {
    load_once();
    uint64_t held=0;
    for (int a=0;a<BIND_COUNT;++a) {
        for (int s=0;s<BIND_SLOTS;++s) {
            const uint16_t in=bound[a][s];
            const bool down=!in ? false
                : in<INPUT_MOUSE ? keys && keys[in]
                : in==INPUT_WHEEL_UP ? (mouse & BIND_WHEEL_UP_MASK)!=0
                : in==INPUT_WHEEL_DOWN ? (mouse & BIND_WHEEL_DOWN_MASK)!=0
                : (mouse & SDL_BUTTON_MASK(in-INPUT_MOUSE))!=0;
            if (down) { held|=UINT64_C(1)<<a; break; }
        }
    }
    return held;
}

bool runtime_bindings_camera_keys_dpad(void) { load_once(); return camera_keys_dpad; }
bool runtime_bindings_use_wheel(void) { load_once(); return use_wheel; }
const char *runtime_bindings_key(int action) { return action>=0 && action<BIND_COUNT ? actions[action].key : NULL; }
const char *runtime_bindings_default(int action) { return action>=0 && action<BIND_COUNT ? actions[action].defaults : NULL; }
