/* libScePad on SDL3 gamepads, with a keyboard fallback. SDL events are pumped
 * by the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Keyboard layout (when no gamepad is connected):
 *   WASD left stick, arrow keys right stick, Space Cross, LShift Circle,
 *   E Square, Q Triangle, 1 L1, 3 R1, R L2, F R2, Z L3, C R3,
 *   Enter Options, Tab left touchpad, Backspace right touchpad,
 *   IJKL d-pad (I up, K down, J left, L right). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static HostMutex lock = HOST_MUTEX_INIT;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { return host_monotonic_ns() / 1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    /* Without a window in this process (the GPU process split: the window and its event loop are
     * in bbgpu-host) nothing pumps SDL's events: poll the gamepads here. */
    if (!SDL_WasInit(SDL_INIT_VIDEO)) SDL_UpdateGamepads();
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* ---- keyboard bindings from input.ini: one "output = key" per line ---- */
enum { ST_NONE, ST_LUP, ST_LDOWN, ST_LLEFT, ST_LRIGHT, ST_RUP, ST_RDOWN, ST_RLEFT, ST_RRIGHT };
typedef struct { SDL_Scancode key; uint32_t btn; int stick, touch, mouse; } KeyBind;
static float mouse_sens = 1.0f, mouse_full = 12.0f, mouse_dz = 0.10f;
static int mouse_inv_y;
static const char *const mouse_names[5] = {"mouse left", "mouse middle", "mouse right", "mouse x1", "mouse x2"};
/* Mouse from the window process: motion in pixels since the last sample, buttons held. */
static void mouse_sample(float *mx, float *my, uint32_t *mb) {
    static uint64_t last_us;
    static float lx, ly;
    static uint32_t lb;
    const uint64_t t = now_us();
    if (last_us && t - last_us < 3000u) { *mx = lx; *my = ly; *mb = lb; return; }
    last_us = t;
    int dx = 0, dy = 0;
    uint32_t b = 0;
    if (!bbgpu_split_mouse(&dx, &dy, &b)) { dx = dy = 0; b = 0; }
    lx = (float)dx / 16.0f; ly = (float)dy / 16.0f; lb = b;
    *mx = lx; *my = ly; *mb = lb;
}
/* Pixels of motion this sample -> a stick value, with the stick dead zone skipped. */
static uint8_t mouse_stick(float px) {
    float v = px * mouse_sens / mouse_full;
    float a = v < 0.0f ? -v : v;
    if (a > 1.0f) a = 1.0f;
    a = mouse_dz + (1.0f - mouse_dz) * a;
    float out = 128.0f + (v < 0.0f ? -a : a) * 127.0f;
    return (uint8_t)(out < 0.0f ? 0.0f : out > 255.0f ? 255.0f : out + 0.5f);
}
#define MAX_BINDS 256
static KeyBind binds[MAX_BINDS];
static int nbinds, binds_loaded;
static const struct { const char *name; uint32_t btn; int stick, touch; } bind_outputs[] = {
    {"cross",BTN_CROSS,ST_NONE,-1}, {"circle",BTN_CIRCLE,ST_NONE,-1},
    {"square",BTN_SQUARE,ST_NONE,-1}, {"triangle",BTN_TRIANGLE,ST_NONE,-1},
    {"l1",BTN_L1,ST_NONE,-1}, {"r1",BTN_R1,ST_NONE,-1},
    {"l2",BTN_L2,ST_NONE,-1}, {"r2",BTN_R2,ST_NONE,-1},
    {"l3",BTN_L3,ST_NONE,-1}, {"r3",BTN_R3,ST_NONE,-1},
    {"options",BTN_OPTIONS,ST_NONE,-1},
    {"up",BTN_UP,ST_NONE,-1}, {"down",BTN_DOWN,ST_NONE,-1},
    {"left",BTN_LEFT,ST_NONE,-1}, {"right",BTN_RIGHT,ST_NONE,-1},
    {"touchpad_left",0,ST_NONE,0}, {"touchpad_right",0,ST_NONE,1},
    {"lstick_up",0,ST_LUP,-1}, {"lstick_down",0,ST_LDOWN,-1},
    {"lstick_left",0,ST_LLEFT,-1}, {"lstick_right",0,ST_LRIGHT,-1},
    {"rstick_up",0,ST_RUP,-1}, {"rstick_down",0,ST_RDOWN,-1},
    {"rstick_left",0,ST_RLEFT,-1}, {"rstick_right",0,ST_RRIGHT,-1},
};
static const char default_input_ini[] =
    "# Keyboard bindings, one per line: output = key\n"
    "# Keys use SDL names (case-insensitive); '_' stands for a space: left_shift, keypad_enter.\n"
    "# An output may have several lines. Outputs: cross circle square triangle l1 r1 l2 r2 l3 r3\n"
    "# options up down left right (d-pad) touchpad_left touchpad_right\n"
    "# lstick_up/down/left/right rstick_up/down/left/right. Restart the game to apply changes.\n"
    "cross = space\n" "cross = return\n" "cross = keypad_enter\n"
    "circle = left_shift\n" "circle = escape\n"
    "square = e\n" "triangle = q\n"
    "l1 = 1\n" "r1 = 3\n" "l2 = r\n" "r2 = f\n" "l3 = z\n" "r3 = c\n"
    "options = f1\n" "options = o\n"
    "up = i\n" "down = k\n" "left = j\n" "right = l\n"
    "up = up\n" "down = down\n" "left = left\n" "right = right\n"
    "touchpad_left = tab\n" "touchpad_right = backspace\n"
    "lstick_up = w\n" "lstick_down = s\n" "lstick_left = a\n" "lstick_right = d\n"
    "rstick_up = up\n" "rstick_down = down\n" "rstick_left = left\n" "rstick_right = right\n"
    "# Mouse: moving it drives the right stick (camera). F6 captures or releases the mouse.\n"
    "# sensitivity multiplies; full_px = pixels per frame for full stick; deadzone = stick minimum.\n"
    "mouse_sensitivity = 1.0\n" "mouse_invert_y = 0\n" "mouse_full_px = 12\n" "mouse_deadzone = 0.10\n"
    "# Mouse buttons as inputs: mouse_left mouse_right mouse_middle mouse_x1 mouse_x2\n"
    "r1 = mouse_left\n" "r2 = mouse_right\n" "r3 = mouse_middle\n";

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
static void parse_binds(char *text, const char *origin) {
    const size_t n = sizeof(bind_outputs) / sizeof(*bind_outputs);
    nbinds = 0;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        line = trim_ws(line);
        if (!*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *out = trim_ws(line), *in = trim_ws(eq + 1);
        for (char *p = out; *p; ++p) *p = (char)tolower((unsigned char)*p);
        for (char *p = in; *p; ++p) if (*p == '_') *p = ' ';
        if (!strncmp(out, "mouse_", 6)) { /* mouse settings, not a binding */
            const float v = (float)atof(in);
            if (!strcmp(out, "mouse_sensitivity")) mouse_sens = v;
            else if (!strcmp(out, "mouse_invert_y")) mouse_inv_y = v != 0.0f;
            else if (!strcmp(out, "mouse_full_px")) mouse_full = v > 1.0f ? v : 1.0f;
            else if (!strcmp(out, "mouse_deadzone")) mouse_dz = v < 0.0f ? 0.0f : v > 0.5f ? 0.5f : v;
            else printf("Runtime: %s: ignoring '%s'\n", origin, out);
            continue;
        }
        size_t o = 0;
        while (o < n && strcmp(out, bind_outputs[o].name)) ++o;
        int mouse = -1;
        for (int m = 0; m < 5; ++m) if (!SDL_strcasecmp(in, mouse_names[m])) mouse = m;
        SDL_Scancode sc = mouse >= 0 ? SDL_SCANCODE_UNKNOWN
                                     : (*in ? SDL_GetScancodeFromName(in) : SDL_SCANCODE_UNKNOWN);
        if (o == n || (mouse < 0 && (sc == SDL_SCANCODE_UNKNOWN || (int)sc >= 512))) {
            printf("Runtime: %s: ignoring '%s = %s'\n", origin, out, in);
            continue;
        }
        if (nbinds < MAX_BINDS)
            binds[nbinds++] = (KeyBind){sc, bind_outputs[o].btn, bind_outputs[o].stick, bind_outputs[o].touch, mouse};
    }
    printf("Runtime: %d keyboard bindings from %s\n", nbinds, origin);
}
static char binds_path[1024];
static uint32_t binds_seq;
static void load_binds_once(void) {
    const uint32_t seq = bbgpu_split_controls_seq();
    if (binds_loaded && seq == binds_seq) return;
    binds_seq = seq;
    if (!binds_loaded) {
        binds_loaded = 1;
        const char *env = getenv("BB_INPUT_INI"), *home = getenv("HOME");
        if (env && *env) snprintf(binds_path, sizeof binds_path, "%s", env);
        else if (access("input.ini", R_OK) == 0) snprintf(binds_path, sizeof binds_path, "input.ini");
        else if (home) snprintf(binds_path, sizeof binds_path, "%s/Library/Application Support/Bloodborne/input.ini", home);
    }
    char *text = NULL;
    FILE *f = binds_path[0] ? fopen(binds_path, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        rewind(f);
        if (len > 0 && len < 65536 && (text = malloc((size_t)len + 1))) {
            size_t got = fread(text, 1, (size_t)len, f);
            text[got] = 0;
        }
        fclose(f);
    } else if (binds_path[0]) {
        FILE *w = fopen(binds_path, "wb");
        if (w) { fputs(default_input_ini, w); fclose(w); }
    }
    mouse_sens = 1.0f; mouse_full = 12.0f; mouse_dz = 0.10f; mouse_inv_y = 0; /* absent lines revert */
    if (!text) text = strdup(default_input_ini);
    if (text) { parse_binds(text, binds_path[0] ? binds_path : "defaults"); free(text); }
}

static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    if (bbgpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    const bool *k=bbgpu_split_keyboard(); /* the window is in the GPU host (split) */
    if (!k) k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (g) {
        static const struct { SDL_GamepadButton sdl; uint32_t ps; } map[]={
            {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
            {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
            {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,BTN_TOUCHPAD},
            {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
            {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
        };
        for (size_t i=0;i<sizeof(map)/sizeof(*map);++i) if (SDL_GetGamepadButton(g,map[i].sdl)) d->buttons|=map[i].ps;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select on pads without a touch surface is a left-side click.
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
        if (k && k[SDL_SCANCODE_TAB]) touch_click(d,0);
        if (k && k[SDL_SCANCODE_BACKSPACE]) touch_click(d,1);
        return;
    }
    if (!k) return;
    load_binds_once();
    float mx = 0.0f, my = 0.0f;
    uint32_t mb = 0;
    mouse_sample(&mx, &my, &mb);
    int ls[4] = {0,0,0,0}, rs[4] = {0,0,0,0}; /* up, down, left, right */
    for (int i = 0; i < nbinds; ++i) {
        const KeyBind *b = &binds[i];
        if (b->mouse >= 0 ? !((mb >> b->mouse) & 1u) : !k[b->key]) continue;
        d->buttons |= b->btn;
        if (b->touch >= 0) touch_click(d, b->touch);
        if (b->stick >= ST_LUP && b->stick <= ST_LRIGHT) ls[b->stick - ST_LUP] = 1;
        else if (b->stick >= ST_RUP) rs[b->stick - ST_RUP] = 1;
    }
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    d->left_x=(uint8_t)(128-(ls[2]?128:0)+(ls[3]?127:0));
    d->left_y=(uint8_t)(128-(ls[0]?128:0)+(ls[1]?127:0));
    d->right_x=(uint8_t)(128-(rs[2]?128:0)+(rs[3]?127:0));
    d->right_y=(uint8_t)(128-(rs[0]?128:0)+(rs[1]?127:0));
    if (mx != 0.0f) d->right_x = mouse_stick(mx);
    if (my != 0.0f) d->right_y = mouse_stick(mouse_inv_y ? -my : my);
}

/* BB_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
static int replay_armed;      /* 1 while a BB_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check;
    static struct timespec mtime;
    if (!checked) { path=getenv("BB_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef __APPLE__
#define st_mtim st_mtimespec
#endif
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const struct { const char *name; uint32_t ps; } names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* BB_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(token,names[i].name)) injected.buttons|=names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* BB_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; BB_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in BB_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("BB_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("BB_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* Touches as the DualShock 4 reports them: every finger that goes down gets a new id (1..127, kept
 * while it stays down) and the time since the first one went down. With id 0 and no hold time
 * the game ignored touchpad presses: no gesture menu from the touchpad, Back or Tab, and no
 * reading of notes (from upstream bbport). */
static void touch_ids(PadData *d) {
    static uint8_t next_id=1, ids[2]; static int down[2]; static uint64_t since;
    for (int i=0;i<2;++i) {
        const int now=i<d->touch_count;
        if (now && !down[i]) { ids[i]=next_id; next_id=next_id==127 ? 1 : next_id+1; }
        down[i]=now;
        if (now) d->touches[i].id=ids[i];
    }
    if (!d->touch_count) since=0;
    else if (!since) since=d->timestamp;
    d->touch_held_time=d->touch_count ? (uint32_t)(d->timestamp-since) : 0;
}
static void sample(PadData *d) {
    sample_host(d);
    if (bbgpu_overlay_captures_input()) return;
    record_sample(d);
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    touch_ids(d);
}

static ABI int32_t pad_init(void) { host_lock(&lock); initialized=1; host_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    host_lock(&lock);
    int already=opened; opened=1;
    host_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    host_lock(&lock);
    sample(data); ++reads;
    host_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    host_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    host_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    host_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    host_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static const RuntimeExport exports[]={
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }
