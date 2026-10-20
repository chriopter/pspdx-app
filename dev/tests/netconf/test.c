/* Exercise real netconf.c against the outer utility lifecycle observed on
 * hardware, including a plugin load failure that leaves result zero. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "kubridge.h"
#include "psputility.h"
#include "gui/netconf.h"
#include "gui/gfx.h"

static int frame, status, load_at, result, state, init_error, observable, loaded_log;
static pspUtilityNetconfData *params;
static void (*overlay)(void);
int kuKernelGetModel(void) { return 10; }
unsigned sceKernelDevkitVersion(void) { return 0x06060110; }
int kuKernelFindModuleByName(char *name, SceModule *out) {
    if (!observable || (!strcmp(name,"sceVshNetconf_Module") &&
                        (load_at < 0 || frame < load_at))) return -1;
    snprintf(out->modname,sizeof(out->modname),"%s",name);
    return 0;
}
int sceIoOpen(const char *p,int flags,int mode) {(void)p;(void)flags;(void)mode;return -1;}
int sceIoClose(int fd) {(void)fd;return 0;}
int sceIoRead(int fd,void *p,size_t n) {(void)fd;(void)p;(void)n;return -1;}
int scePowerGetCpuClockFrequencyInt(void) { return 333; }
int scePowerGetBusClockFrequencyInt(void) { return 166; }
int scePowerSetClockFrequency(int c,int p,int b) {(void)c;(void)p;(void)b;return 0;}
int sceWlanGetSwitchState(void) { return 1; }
int sceUtilityCheckNetParam(int id) {(void)id;return -1;}
int sceUtilityGetSystemParamInt(int id,int *v) {(void)id;*v=0;return 0;}
int https_net_init(void) { return 0; }
int sceUtilityNetconfInitStart(pspUtilityNetconfData *p) {
    params=p;status=PSP_UTILITY_DIALOG_INIT;return init_error;
}
int sceUtilityNetconfGetStatus(void) {
    if(status==PSP_UTILITY_DIALOG_INIT && frame>=2) status=PSP_UTILITY_DIALOG_VISIBLE;
    if(status==PSP_UTILITY_DIALOG_VISIBLE && frame>=10) {
        status=PSP_UTILITY_DIALOG_QUIT;params->base.result=result;
    }
    return status;
}
int sceUtilityNetconfUpdate(int n) {(void)n;return 0;}
int sceUtilityNetconfShutdownStart(void) {status=PSP_UTILITY_DIALOG_NONE;return 0;}
int sceNetApctlGetState(int *s) {*s=state;return 0;}
void gfx_frame_overlay(void (*f)(void)) {overlay=f;}
void gfx_frame_begin(unsigned c) {(void)c;assert(++frame<100);}
void gfx_vgrad(int x,int y,int w,int h,unsigned a,unsigned b) {
    (void)x;(void)y;(void)w;(void)h;(void)a;(void)b;
}
void gfx_frame_end(void) {assert(overlay);overlay();}
unsigned now_ms(void) {return frame*16;}
void log_dump(void) {}
void log_dump_later(void) {}
void logline(const char *fmt,...) {
    char s[512];va_list ap;va_start(ap,fmt);vsnprintf(s,sizeof(s),fmt,ap);va_end(ap);
    if(strstr(s,"network dialog module loaded at")) loaded_log++;
}
static void reset(int at,int r,int connected) {
    frame=0;load_at=at;result=r;state=connected;init_error=0;observable=1;loaded_log=0;
}
int main(void) {
    reset(-1,0,0); /* Exact observed missing-plugin failure: result stays zero. */
    assert(netconf_connect()==-1 && netconf_failed_to_open());
    reset(6,1,0); /* Async load after VISIBLE, then ordinary cancellation. */
    assert(netconf_connect()==-1 && !netconf_failed_to_open() && loaded_log==1);
    reset(6,0,4);
    assert(netconf_connect()==0 && !netconf_failed_to_open());
    reset(6,0,0); /* Loaded dialog, no connection: do not mislabel as load failure. */
    assert(netconf_connect()==-1 && !netconf_failed_to_open());
    reset(-1,0,0);observable=0; /* Emulator without kernel module inspection. */
    assert(netconf_connect()==-1 && !netconf_failed_to_open());
    reset(-1,0,0);init_error=-1;
    assert(netconf_connect()==-1 && netconf_failed_to_open());
    reset(2,1,0); /* A later valid attempt clears an earlier failure. */
    assert(netconf_connect()==-1 && !netconf_failed_to_open());
    puts("netconf lifecycle: 7 cases passed");
    return 0;
}
