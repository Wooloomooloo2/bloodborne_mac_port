#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"

static int capture, mouse_look;
static bool keys[512];
static int use_keys;
int bbgpu_overlay_captures_input(void) { return capture; }
int bbgpu_mouse_look_enabled(void) { return mouse_look; }
const bool *bbgpu_split_keyboard(void) { return use_keys ? keys : NULL; }
uint32_t bbgpu_bench_frame_draws(void) { return 0; }
uint64_t bbgpu_bench_host_flips(void) { return 0; }
void bbgpu_bench_set_phase(uint32_t phase, const char *error) { (void)phase; (void)error; }
volatile int runtime_bench_map_opened;
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}

static PadData press(const SDL_Scancode *codes, size_t count) {
    memset(keys,0,sizeof keys);
    for (size_t i=0;i<count;++i) keys[codes[i]]=true;
    PadData data;
    assert(pad_read_state(1,&data)==0);
    return data;
}
#define PRESS(...) press((const SDL_Scancode[]){__VA_ARGS__},sizeof((const SDL_Scancode[]){__VA_ARGS__})/sizeof(SDL_Scancode))

static void write_file(const char *path, const char *text) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(text,f);
    fclose(f);
}

/* Bindings (runtime_bindings.c): the defaults play like 0.5.9, the ini rebinds, unbinds and
 * ignores what it cannot use; the walk key; camera keys as the d-pad under mouse look. */
static void test_bindings(void) {
    use_keys=1;
    runtime_bindings_load(NULL);
    PadData d=PRESS(SDL_SCANCODE_W);
    assert(d.left_y==0 && d.left_x==128 && d.buttons==0);
    d=PRESS(SDL_SCANCODE_S,SDL_SCANCODE_D);
    assert(d.left_y==255 && d.left_x==255);
    d=PRESS(SDL_SCANCODE_W,SDL_SCANCODE_S);
    assert(d.left_y==127);
    d=PRESS(SDL_SCANCODE_LALT,SDL_SCANCODE_A,SDL_SCANCODE_S);
    assert(d.left_x==64 && d.left_y==192);
    d=PRESS(SDL_SCANCODE_SPACE,SDL_SCANCODE_LSHIFT,SDL_SCANCODE_R,SDL_SCANCODE_F);
    assert(d.buttons==(BTN_CROSS|BTN_CIRCLE|BTN_L2|BTN_R2) && d.l2==255 && d.r2==255);
    d=PRESS(SDL_SCANCODE_KP_ENTER,SDL_SCANCODE_O,SDL_SCANCODE_I,SDL_SCANCODE_L);
    assert(d.buttons==(BTN_CROSS|BTN_OPTIONS|BTN_UP|BTN_RIGHT));
    memset(keys,0,sizeof keys); keys[SDL_SCANCODE_TAB]=true; /* not pressed through the pad: touch ids are checked below */
    assert(runtime_bindings_held(keys,0)==(UINT64_C(1)<<BIND_TOUCH_LEFT));
    d=PRESS(SDL_SCANCODE_LEFT,SDL_SCANCODE_DOWN);
    assert(d.right_x==0 && d.right_y==255 && d.buttons==0);
    mouse_look=1;
    d=PRESS(SDL_SCANCODE_LEFT,SDL_SCANCODE_DOWN);
    assert(d.right_x==128 && d.right_y==128 && d.buttons==(BTN_LEFT|BTN_DOWN));
    mouse_look=0;
    d=PRESS(SDL_SCANCODE_LGUI,SDL_SCANCODE_Q);
    assert(d.buttons==0); /* Cmd+Q is the Mac's */
    assert(runtime_bindings_held(NULL,SDL_BUTTON_LMASK|SDL_BUTTON_X1MASK)==((UINT64_C(1)<<BIND_R1)|(UINT64_C(1)<<BIND_R2)));
    assert(!runtime_bindings_use_wheel());

    char path[]="/tmp/bbport-bindings-test-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    write_file(path,"# comment\r\nupscaler=metalfx\r\nbind_cross = K , Mouse Left\r\nbind_dpad_down=\r\n"
                    "bind_square=Left GUI,Bogus,g\r\nbind_r1=Wheel Up\r\nbind_walk=CapsLock\r\ncamera_keys_dpad=0\r\n");
    runtime_bindings_load(path);
    d=PRESS(SDL_SCANCODE_K);
    assert(d.buttons==BTN_CROSS); /* K was the d-pad's down: now Cross, and down is unbound */
    d=PRESS(SDL_SCANCODE_SPACE,SDL_SCANCODE_G);
    assert(d.buttons==BTN_SQUARE); /* Space is no longer Cross; Cmd and unknown names ignored */
    d=PRESS(SDL_SCANCODE_CAPSLOCK,SDL_SCANCODE_D);
    assert(d.left_x==192);
    mouse_look=1;
    d=PRESS(SDL_SCANCODE_UP);
    assert(d.right_y==0 && d.buttons==0); /* camera_keys_dpad=0: the keys keep the camera */
    mouse_look=0;
    assert(runtime_bindings_held(NULL,SDL_BUTTON_LMASK)==(UINT64_C(1)<<BIND_CROSS));
    assert(runtime_bindings_held(NULL,BIND_WHEEL_UP_MASK)==(UINT64_C(1)<<BIND_R1));
    assert(runtime_bindings_held(NULL,BIND_WHEEL_DOWN_MASK)==0);
    assert(runtime_bindings_use_wheel());
    unlink(path);
    runtime_bindings_load(NULL);
    memset(keys,0,sizeof keys);
    use_keys=0;
}

/* The launcher's tables (launcher/Model/KeyCodes.swift, Bindings.swift) against SDL and
 * runtime_bindings.c: every key name is SDL's own spelling, every action and default the same. */
static void test_launcher_tables(void) {
    FILE *f=fopen("launcher/Model/KeyCodes.swift","r");
    assert(f);
    char line[256]; int names=0;
    while (fgets(line,sizeof line,f)) {
        char name[64]; unsigned code;
        if (sscanf(line," (0x%x, \"%63[^\n]",&code,name)!=2) continue;
        char *end=strrchr(name,'"'); assert(end); *end=0;
        if (!strcmp(name,"\\\\")) strcpy(name,"\\"); /* the Swift escape */
        const SDL_Scancode sc=SDL_GetScancodeFromName(name);
        if (sc==SDL_SCANCODE_UNKNOWN || strcmp(SDL_GetScancodeName(sc),name)) {
            printf("KeyCodes.swift: \"%s\" is not an SDL scancode name (SDL: \"%s\")\n",name,sc ? SDL_GetScancodeName(sc) : "");
            assert(0);
        }
        ++names;
    }
    fclose(f);
    assert(names>100);
    f=fopen("launcher/Model/Bindings.swift","r");
    assert(f);
    int actions=0;
    while (fgets(line,sizeof line,f)) {
        const char *k=strstr(line,"BindAction(key: \""), *d=strstr(line,"defaults: [");
        if (!k || !d) continue;
        char key[64], list[128]="";
        assert(sscanf(k,"BindAction(key: \"%63[^\"]",key)==1);
        for (const char *p=d+11; *p && *p!=']'; ++p) if (*p!='"') strncat(list,(char[]){*p==' ' && p[-1]==',' ? 0 : *p,0},1);
        int found=0;
        for (int a=0;a<BIND_COUNT;++a) {
            if (strcmp(runtime_bindings_key(a),key)) continue;
            found=1;
            if (strcmp(runtime_bindings_default(a),list)) { printf("Bindings.swift %s: %s, runtime_bindings.c: %s\n",key,list,runtime_bindings_default(a)); assert(0); }
        }
        assert(found);
        ++actions;
    }
    fclose(f);
    assert(actions==BIND_COUNT);
}

int main(void) {
    char path[]="/tmp/bbport-pad-test-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("BB_PAD_FILE",path,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    test_bindings();
    test_launcher_tables();
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    /* A new touch gets a new id (1..127), as from a DualShock 4: the game ignores id 0. */
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471 && data.touches[0].id==1);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==2);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==3);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture, bindings");
}
