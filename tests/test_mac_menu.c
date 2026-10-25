// SPDX-License-Identifier: GPL-3.0-or-later
// Hook installation is all-or-nothing even with unsupported or modified guest code.
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/runtime_mac_settings.c"
int32_t bbgpu_mac_setting_get(uint32_t field) { (void)field; return 0; }
static int restart_requests,visual_restart;
void bbgpu_mac_setting_set(uint32_t field,int32_t value) { if (field==BBMAC_RESTART && value==1) ++restart_requests; }
static ABI uint8_t popup_state(void *w) { return *((uint8_t *)w+8); }
static ABI void reset_action(void *w) { (void)w; visual_restart=0; values[3].byte=0; }

static void seed(unsigned char *base) {
    image=base;
    unsigned char *cp=(unsigned char *)addr(0x1fb4978),*pad=(unsigned char *)addr(0x1fb4c60);
    const unsigned char expected[]={0x90,0x66,0x66,0x66,0x66,0x66,0x66,0x2e,0x0f,0x1f,0x84,0,0,0,0,0};
    cp[0]=0xe8; int32_t rel=(int32_t)(addr(0x1fea630)-(uintptr_t)(cp+5)); memcpy(cp+1,&rel,4);
    memcpy(pad,expected,16); *(unsigned char *)addr(0x1f20e21)=6;
    memset((void *)(addr(0x57398b0)+48),0,16);
    put64((void *)(addr(0x579abe0)+8),addr(0x1f241b0));
    put64((void *)(addr(0x579abe0)+0x10),addr(0x1f24250));
    put64((void *)(addr(0x579abe0)+0x18),addr(0x1f1a490));
}
int main(void) {
    const size_t size=0x579abe0-0x400000+0x70;
    unsigned char *base=calloc(1,size),*before=malloc(size); assert(base && before);
    seed(base); unsetenv("BB_MAC_SETTINGS_READY"); assert(!runtime_install_mac_settings(base,size));
    setenv("BB_MAC_SETTINGS_READY","1",1);
    const unsigned sites[]={0x1fb4978,0x1fb4979,0x1fb4c60,0x1f20e21,0x57398b0+48,0x57398b0+56,0x579abe0+8,0x579abe0+0x10,0x579abe0+0x18};
    for (unsigned i=0;i<sizeof(sites)/sizeof(*sites);++i) {
        seed(base); base[sites[i]-0x400000]^=1; memcpy(before,base,size);
        assert(!runtime_install_mac_settings(base,size)); assert(!memcmp(before,base,size));
    }
    seed(base); assert(!runtime_install_mac_settings(base,size-1));
    setenv("BB_MAC_SETTINGS","0",1); assert(!runtime_install_mac_settings(base,size)); unsetenv("BB_MAC_SETTINGS");
    assert(runtime_install_mac_settings(base,size)); image=base;
    assert(*(unsigned char *)addr(0x1f20e21)==7);
    assert(read64((void *)(addr(0x57398b0)+48))==(uintptr_t)section_name);
    assert(read64((void *)(addr(0x57398b0)+56))==0);
    uint32_t buttons=L1; runtime_mac_settings_pad(&buttons); assert(buttons==L1);
    buttons=0; runtime_mac_settings_pad(&buttons); atomic_store(&active,1);
    buttons=R1|0x4000; runtime_mac_settings_pad(&buttons); assert(buttons==0x4000); assert(atomic_exchange(&page_buttons,0)==R1);
    buttons=R1; runtime_mac_settings_pad(&buttons); assert(!atomic_load(&page_buttons));
    unsigned char fake_dialog[0xf20]={0},row[0x90]={0},widget[16]={0};
    uintptr_t vtable[7]={0}; vtable[6]=(uintptr_t)popup_state;
    put64(widget,(uintptr_t)vtable); put64(row+0x80,(uintptr_t)widget);
    put64(fake_dialog+0xe58,(uintptr_t)row); put64(fake_dialog+0xe60,(uintptr_t)(row+sizeof(row)));
    assert(!row_captures_input(fake_dialog)); widget[8]=1; assert(row_captures_input(fake_dialog));
    unsigned char restart_rows[5*0x90]={0};
    vtable[4]=(uintptr_t)reset_action; put64(restart_rows+4*0x90+0x80,(uintptr_t)widget);
    put64(fake_dialog+0xe58,(uintptr_t)restart_rows); dialog=fake_dialog; page=8;
    memset(values,0,sizeof(values)); confirmed=0; values[3].byte=1; visual_restart=1;
    poll_values(); assert(!restart_requests && !visual_restart && !values[3].byte && !values[3].last);
    confirmed=1; values[3].byte=1; visual_restart=1;
    poll_values(); assert(restart_requests==1 && !visual_restart && !values[3].byte);
    free(before); free(base); puts("Native menu hook guards and input edges PASS");
}
