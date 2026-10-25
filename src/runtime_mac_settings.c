// SPDX-License-Identifier: GPL-3.0-or-later
// Native widgets adapted from bloodborne_mac gpu/metal/bbhost/game_menu.cpp
// (bbacb95c3338efcd627c0edaec05ff7534e555da), itself adapted from bbhost
// engine/option_menu.cpp (566184a). Only the port's existing settings are exposed.
#include "runtime.h"
#include "gpu/bbmac_settings.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAGE_COUNT=9, ROWS=5, L1=0x400, R1=0x800 };
static unsigned char *image;
static _Atomic int active;
static _Atomic unsigned page_buttons;
static _Atomic int page_direction=1;
static unsigned held_buttons;
static void *dialog;
static int page;
static char section_name[]="MacSettings";
static _Alignas(16) unsigned char own_vtable[0x80];
static uint16_t page_text[96];
static const uint16_t empty_text[]={0};
static uint8_t header_value;

typedef uintptr_t (ABI *GuestCall)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
static uintptr_t addr(uint64_t va) { return (uintptr_t)image+va-0x400000; }
static uintptr_t invoke(uintptr_t fn,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f) {
    return ((GuestCall)fn)(a,b,c,d,e,f);
}
#define CALL(va,a,b,c,d,e,f) invoke(addr(va),(uintptr_t)(a),(uintptr_t)(b),(uintptr_t)(c),(uintptr_t)(d),(uintptr_t)(e),(uintptr_t)(f))
static uint64_t read64(const void *p) { uint64_t v; memcpy(&v,p,8); return v; }
static void put64(void *p,uint64_t v) { memcpy(p,&v,8); }
static void put32(void *p,uint32_t v) { memcpy(p,&v,4); }

static void free_string(unsigned char *p) {
    if (read64(p+0x18)<8) return;
    uintptr_t data=read64(p),allocator=read64(p+0x20);
    if (data && allocator) invoke(read64((void *)(read64((void *)allocator)+0x70)),allocator,data,0,0,0,0);
}
typedef struct { _Alignas(16) unsigned char bytes[0x80]; } Caption;
static Caption caption(unsigned id) {
    Caption c={0};
    CALL(0x1ee8cc0,c.bytes,200,id,0,0,0);
    CALL(0x1ee8cc0,c.bytes+0x40,201,id,0,0,0);
    return c;
}
static void free_caption(Caption *c) { free_string(c->bytes+0x10); free_string(c->bytes+0x50); }
static const uint16_t *text(unsigned id) {
    Caption c=caption(id);
    const uint16_t *s=(const uint16_t *)(uintptr_t)read64(c.bytes);
    free_caption(&c);
    return s ? s : empty_text;
}
static void make_entry(unsigned char *entry,int value,const uint16_t *s) {
    memset(entry,0,0x48); put32(entry,(uint32_t)value); put64(entry+8,(uintptr_t)s);
    CALL(0x2d67040,entry+0x10,s,0,0,0,0); entry[0x40]=1;
}
static void free_entries(unsigned char *list,unsigned count) {
    for (unsigned i=0;i<count;++i) free_string(list+i*0x48+0x18);
}
static void function(unsigned char *out,unsigned vtable,void *fn) {
    memset(out,0,0x28); put64(out,addr(vtable)); put64(out+8,(uintptr_t)fn); put64(out+0x20,(uintptr_t)out);
}

typedef struct { int value; unsigned message; } Choice;
static const Choice upscalers[]={{0,126300},{1,126301},{2,126302},{3,126303},{4,126304},{5,126305}};
static const Choice presets[]={{0,126310},{1,126311},{2,126312},{3,126313},{4,126314}};
static const Choice outputs[]={{0,126320},{1,126321},{2,126322},{3,126323},{4,126324}};
static const Choice compilers[]={{0,126330},{1,126331},{2,126332}};
static const Choice debug_views[]={{0,126340},{1,126341},{2,126342}};
static const Choice live_modes[]={{-1,126350},{0,126351},{1,126352}};
static const Choice caps[]={{30,126360},{40,126361},{60,126362},{90,126363},{120,126364}};
enum { TOGGLE,SLIDER,LIST,CONFIRM,RESTART };
typedef struct { unsigned field,kind; int min,max,unit; const Choice *choices; unsigned count; } Row;
#define T(f) {f,TOGGLE,0,1,1,NULL,0}
#define S(f,min,max,unit) {f,SLIDER,min,max,unit,NULL,0}
#define C(f,choices) {f,LIST,0,0,1,choices,sizeof(choices)/sizeof(*choices)}
static const Row pages[PAGE_COUNT][ROWS]={
    {T(BBMAC_SHARPEN),T(BBMAC_JITTER),T(BBMAC_OBJECT_MOTION),T(BBMAC_REACTIVE),C(BBMAC_UPSCALER,upscalers)},
    {S(BBMAC_SHARPNESS,0,200,1),S(BBMAC_REACTIVE_SCALE,0,160,10),S(BBMAC_REACTIVE_THRESHOLD,0,100,1),S(BBMAC_REACTIVE_MAX,0,100,1),C(BBMAC_PRESET,presets)},
    {C(BBMAC_OUTPUT,outputs)},
    {T(BBMAC_SHOW_FPS),T(BBMAC_FRAME_PACING),S(BBMAC_MODEL_LOD,-2,2,1),T(BBMAC_EFFECT_FIRST+4),C(BBMAC_SHADER_COMPILE,compilers)},
    {T(BBMAC_EFFECT_FIRST),T(BBMAC_EFFECT_FIRST+1),T(BBMAC_EFFECT_FIRST+2),T(BBMAC_EFFECT_FIRST+3),T(BBMAC_EFFECT_FIRST+5)},
    {T(BBMAC_EFFECT_FIRST+6),T(BBMAC_EFFECT_FIRST+7),T(BBMAC_EFFECT_FIRST+8),T(BBMAC_EFFECT_FIRST+9),C(BBMAC_DEBUG_VIEW,debug_views)},
    {C(BBMAC_LIVE_RESOLUTION,live_modes)},
    {T(BBMAC_FULLSCREEN),T(BBMAC_METAL_HUD),T(BBMAC_TEXTURE_COMPRESSION),C(BBMAC_FRAME_CAP,caps)},
    {T(BBMAC_GAME_TIMING),T(BBMAC_SHOW_LAUNCHER),{BBMAC_COUNT,CONFIRM,0,1,1,NULL,0},{BBMAC_RESTART,RESTART,0,1,1,NULL,0}}
};
static const unsigned page_counts[]={5,5,1,5,5,5,1,4,4};
typedef struct { uint8_t byte; int32_t list,last; int readonly; } Value;
static Value values[ROWS];
static int confirmed;

static void show_last_widget(void *d) {
    uintptr_t end=read64((unsigned char *)d+0xe60);
    uintptr_t widget=read64((void *)(end-0x90+0x80));
    if (!widget) return;
    // Each constructor hides the other widget types, assuming a fresh movie. Paging reuses
    // that movie, so explicitly reveal the selected type's stable GFxAccessor (widget+0x10).
    uintptr_t accessor=widget+0x10;
    invoke(read64((void *)(read64((void *)accessor)+8)),accessor,1,0,0,0,0);
}

static void toggle_row(void *d,Value *v,unsigned id) {
    Caption c=caption(id); _Alignas(16) unsigned char list[0x100]={0}; uint8_t def=v->byte;
    CALL(0x1f2b3b0,list,0,0,0,0,0);
    CALL(0x1f2a100,d,c.bytes,&v->byte,list,&def,0);
    free_entries(list,(unsigned)read64(list+0x98)); free_caption(&c);
    show_last_widget(d);
}
static void text_row(void *d,uint8_t *value,unsigned id,const uint16_t *s,int navigation) {
    Caption c=caption(id); _Alignas(16) unsigned char list[0x100]={0},entry[0x48]; uint8_t def=0;
    make_entry(entry,0,s); CALL(0x1f2c200,list,entry,0,0,0,0); free_entries(entry,1);
    if (navigation) { make_entry(entry,1,s); CALL(0x1f2c200,list,entry,0,0,0,0); free_entries(entry,1); }
    *value=0; CALL(0x1f2a100,d,c.bytes,value,list,&def,0); free_entries(list,navigation ? 2 : 1); free_caption(&c);
    show_last_widget(d);
}
static void slider_row(void *d,Value *v,unsigned id,const Row *r) {
    Caption c=caption(id); uint8_t def=v->byte;
    CALL(0x1f2ac00,d,c.bytes,&v->byte,&def,0,0); free_caption(&c);
    // Native SliderComponent bounds/current/step, verified in 1.09 sub_1f2c500.
    uintptr_t end=read64((unsigned char *)d+0xe60);
    unsigned char *w=(unsigned char *)(uintptr_t)read64((void *)(end-0x90+0x80));
    if (w) { put32(w+0xc94,(uint32_t)(r->max-r->min)); put32(w+0xc98,v->byte);
        CALL(0x1edcc10,w+0xa0,0,0,0,0,0); }
    show_last_widget(d);
}
static void list_row(void *d,Value *v,unsigned id,const Row *r) {
    Caption c=caption(id); _Alignas(16) unsigned char list[0x1000]={0},entry[0x48]; unsigned count=0;
    int mask=bbgpu_mac_setting_get(BBMAC_PROVIDER_MASK);
    for (unsigned i=0;i<r->count;++i) {
        // Keep an unavailable saved provider visible without offering other unavailable ones.
        if (r->field==BBMAC_UPSCALER && !(mask&(1<<r->choices[i].value)) && r->choices[i].value!=v->list) continue;
        make_entry(entry,r->choices[i].value,text(r->choices[i].message));
        CALL(0x1f1c0c0,list,entry,0,0,0,0); free_entries(entry,1); ++count;
    }
    int32_t def=v->list; CALL(0x1f29370,d,c.bytes,&v->list,list,&def,0);
    free_entries(list,count); free_caption(&c);
    show_last_widget(d);
}
static void update_heading(void) {
    const uint16_t *title=text(126010+(unsigned)page);
    const uint16_t *suffix=bbgpu_mac_setting_get(BBMAC_RESTART_NEEDED) ? text(126021) : empty_text;
    unsigned n=0;
    page_text[n++]=(uint16_t)('1'+page); page_text[n++]='/'; page_text[n++]='9'; page_text[n++]=' ';
    while (*title && n<70) page_text[n++]=*title++;
    if (*suffix) { page_text[n++]=' '; while (*suffix && n<95) page_text[n++]=*suffix++; }
    page_text[n]=0;
}
static void build_rows(void *d) {
    _Alignas(16) unsigned char scratch[0x100]={0}; CALL(0x208af20,scratch,0,0,0,0,0);
    update_heading(); text_row(d,&header_value,126020,page_text,1); confirmed=0;
    for (unsigned i=0;i<page_counts[page];++i) {
        const Row *r=&pages[page][i]; Value *v=&values[i]; memset(v,0,sizeof(*v));
        int value=r->kind==CONFIRM || r->kind==RESTART ? 0 : bbgpu_mac_setting_get(r->field);
        v->list=value; v->byte=(uint8_t)(value/r->unit-r->min);
        int wide=bbgpu_mac_setting_get(BBMAC_NATIVE_ULTRAWIDE);
        v->readonly=(r->field==BBMAC_SHOW_LAUNCHER && value<0) || (wide &&
            (r->field==BBMAC_UPSCALER || r->field==BBMAC_PRESET || r->field==BBMAC_OUTPUT || r->field==BBMAC_LIVE_RESOLUTION));
        unsigned id=r->kind==CONFIRM ? 126022 : 126100+r->field;
        if (v->readonly) text_row(d,&v->byte,id,text(wide ? 126024 : 126023),0);
        else if (r->kind==LIST) list_row(d,v,id,r);
        else if (r->kind==SLIDER) slider_row(d,v,id,r);
        else toggle_row(d,v,id);
        v->last=r->kind==LIST ? v->list : v->byte;
    }
}
static void free_row(unsigned char *row) {
    // The native helper releases the widget atomically and destroys both caption strings.
    CALL(0x1f1c190,row,0,0,0,0,0);
}
static void clear_rows(void *d,int defaults_only) {
    unsigned char *p=d; uintptr_t begin=read64(p+0xe58),end=read64(p+0xe60);
    if (!begin || end<begin || (end-begin)%0x90) return;
    while (end>begin) {
        unsigned char *row=(unsigned char *)(end-0x90);
        if (defaults_only && read64(row)!=(uintptr_t)text(0x9d8a)) break;
        free_row(row); end-=0x90; put64(p+0xe60,end);
        if (defaults_only) break;
    }
}
static void poll_values(void) {
    for (unsigned i=0;i<page_counts[page];++i) {
        const Row *r=&pages[page][i]; Value *v=&values[i];
        int next=r->kind==LIST ? v->list : v->byte;
        if (v->readonly || next==v->last) continue;
        v->last=next;
        if (r->kind==CONFIRM) confirmed=next!=0;
        else if (r->kind==RESTART) {
            if (next && confirmed) bbgpu_mac_setting_set(BBMAC_RESTART,1);
            // RestoreDefaults also resets the native choice index and redraws Off. Merely
            // changing its backing byte leaves the rejected action visibly On.
            uintptr_t begin=read64((unsigned char *)dialog+0xe58);
            uintptr_t widget=read64((void *)(begin+(i+1)*0x90+0x80));
            if (widget) invoke(read64((void *)(read64((void *)widget)+0x20)),widget,0,0,0,0,0);
            v->byte=0; v->last=0;
        } else bbgpu_mac_setting_set(r->field,r->kind==SLIDER ? (next+r->min)*r->unit : next);
    }
}
static int row_captures_input(void *d) {
    unsigned char *p=d; uintptr_t begin=read64(p+0xe58),end=read64(p+0xe60);
    if (!begin || end<begin || (end-begin)%0x90) return 1;
    for (uintptr_t row=begin;row<end;row+=0x90) {
        uintptr_t widget=read64((void *)(row+0x80));
        if (widget && (uint8_t)invoke(read64((void *)(read64((void *)widget)+0x30)),widget,0,0,0,0,0)) return 1;
    }
    return 0;
}
static ABI void menu_update(void *d,float dt,void *input) {
    typedef void (ABI *Update)(void *,float,void *);
    ((Update)addr(0x1f1a490))(d,dt,input);
    if (d!=dialog || !atomic_load_explicit(&active,memory_order_acquire)) return;
    poll_values();
    unsigned buttons=atomic_exchange_explicit(&page_buttons,0,memory_order_acq_rel);
    // The game's own interactivity checks exclude popups, child dialogs and transitions.
    if ((buttons || header_value) && !row_captures_input(d) &&
        (uint8_t)CALL(0x1eec800,d,0,0,0,0,0) && !(uint8_t)CALL(0x1eec7d0,d,0,0,0,0,0)) {
        int step=buttons ? ((buttons&R1) ? 1 : -1) : atomic_load(&page_direction);
        clear_rows(d,0); page=(page+PAGE_COUNT+step)%PAGE_COUNT;
        build_rows(d); CALL(0x1f28de0,d,0,0,0,0,0);
        fprintf(stderr,"Mac Settings: page %d/%d\n",page+1,PAGE_COUNT);
    }
    update_heading();
}
static void closed(void *d) {
    if (d==dialog) { poll_values(); atomic_store_explicit(&active,0,memory_order_release);
        atomic_store(&page_buttons,0); dialog=NULL; }
}
static ABI void menu_destructor(void *d) { closed(d); CALL(0x1f241b0,d,0,0,0,0,0); }
static ABI void menu_delete(void *d) { closed(d); CALL(0x1f24250,d,0,0,0,0,0); }
static ABI __attribute__((aligned(16))) void menu_handler(void *d,void *unused) { (void)unused; build_rows(d); }
static ABI uintptr_t menu_open(void *root,void *params) {
    page=0;
    uintptr_t d=CALL(0x1f20900,root,params,section_name,menu_handler,0,0);
    if (!d) return 0;
    clear_rows((void *)d,1); CALL(0x1f28de0,d,0,0,0,0,0);
    memcpy(own_vtable,(void *)(addr(0x579abe0)-0x10),sizeof(own_vtable));
    put64(own_vtable+0x10+8,(uintptr_t)menu_destructor);
    put64(own_vtable+0x10+0x10,(uintptr_t)menu_delete);
    put64(own_vtable+0x10+0x18,(uintptr_t)menu_update);
    put64((void *)d,(uintptr_t)(own_vtable+0x10)); dialog=(void *)d;
    atomic_store(&page_buttons,0); atomic_store_explicit(&active,1,memory_order_release);
    fprintf(stderr,"Mac Settings: opened\n"); return d;
}
static ABI void *menu_row(void *out,void *ctx) {
    _Alignas(16) unsigned char f[0x28]; function(f,0x5743880,menu_open);
    CALL(0x1fb4c70,out,ctx,f,0,0,0); return out;
}
static ABI void *system_finalize(void *out,void *builder,void *arg3) {
    unsigned char *b=builder; uintptr_t begin=read64(b+0x68),end=read64(b+0x70); int move_exit=0;
    if (begin && end>begin && (end-begin)%0x180==0) {
        unsigned char *last=(unsigned char *)(end-0x180);
        for (unsigned i=0;i<0x180;i+=8) if (read64(last+i)==addr(0x200c9f0)) move_exit=1;
        if (move_exit) { invoke(read64((void *)(read64(last)+8)),(uintptr_t)last,0,0,0,0,0); put64(b+0x70,end-0x180); }
    }
    Caption c=caption(126001); _Alignas(16) unsigned char f[0x28]; int64_t flag=0;
    function(f,0x573d120,menu_row); CALL(0x1f4e2a0,b,c.bytes,f,&flag,0,0); free_caption(&c);
    if (move_exit) { c=caption(0x1adb4); function(f,0x573d120,(void *)addr(0x200c9f0));
        CALL(0x1f4c9a0,b,c.bytes,f,0,0,0); free_caption(&c); }
    CALL(0x1fea630,out,builder,arg3,0,0,0); return out;
}

void runtime_mac_settings_pad(uint32_t *buttons) {
    if (*buttons&0x20) atomic_store_explicit(&page_direction,1,memory_order_release);
    else if (*buttons&0x80) atomic_store_explicit(&page_direction,-1,memory_order_release);
    unsigned now=*buttons&(L1|R1),pressed=now&~held_buttons; held_buttons=now;
    if (atomic_load_explicit(&active,memory_order_acquire)) {
        atomic_fetch_or_explicit(&page_buttons,pressed,memory_order_release); *buttons&=~(L1|R1);
    }
}

int runtime_install_mac_settings(unsigned char *base,uint64_t size) {
    const char *enabled=getenv("BB_MAC_SETTINGS_READY");
    if (!enabled || strcmp(enabled,"1") || (getenv("BB_MAC_SETTINGS") && !strcmp(getenv("BB_MAC_SETTINGS"),"0"))) return 0;
    if (size<0x579abe0-0x400000+0x70) return 0;
    image=base;
    unsigned char *cp=(unsigned char *)addr(0x1fb4978),*pad=(unsigned char *)addr(0x1fb4c60);
    const unsigned char expected[]={0x90,0x66,0x66,0x66,0x66,0x66,0x66,0x2e,0x0f,0x1f,0x84,0,0,0,0,0};
    int32_t rel; memcpy(&rel,cp+1,4);
    if (cp[0]!=0xe8 || (uintptr_t)(cp+5)+rel!=addr(0x1fea630) || memcmp(pad,expected,16) ||
        *(unsigned char *)addr(0x1f20e21)!=6 || read64((void *)(addr(0x57398b0)+48)) || read64((void *)(addr(0x57398b0)+56)) ||
        read64((void *)(addr(0x579abe0)+8))!=addr(0x1f241b0) ||
        read64((void *)(addr(0x579abe0)+0x10))!=addr(0x1f24250) ||
        read64((void *)(addr(0x579abe0)+0x18))!=addr(0x1f1a490)) {
        fprintf(stderr,"Mac Settings: unsupported menu code; hooks disabled\n"); image=NULL; return 0;
    }
    // Every guard above is checked before the first mutation; the loader protects pages later.
    put64((void *)(addr(0x57398b0)+48),(uintptr_t)section_name); *(unsigned char *)addr(0x1f20e21)=7;
    pad[0]=0x49; pad[1]=0xbb; put64(pad+2,(uintptr_t)system_finalize);
    pad[10]=0x41; pad[11]=0xff; pad[12]=0xe3;
    rel=(int32_t)(pad-(cp+5)); memcpy(cp+1,&rel,4);
    fprintf(stderr,"Mac Settings: native System menu installed\n"); return 1;
}
