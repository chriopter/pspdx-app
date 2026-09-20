#ifndef TEST_PSPKERNEL_H
#define TEST_PSPKERNEL_H
#include <stddef.h>
typedef int SceUID;
typedef unsigned SceSize;
typedef unsigned SceUInt;
#define PSP_THREAD_ATTR_USER 0
SceUID sceKernelCreateThread(const char *, int (*)(SceSize, void *), int, int, int, void *);
int sceKernelStartThread(SceUID, SceSize, void *);
int sceKernelWaitThreadEnd(SceUID, void *);
int sceKernelDeleteThread(SceUID);
SceUID sceKernelCreateSema(const char *, int, int, int, void *);
int sceKernelWaitSema(SceUID, int, void *);
int sceKernelSignalSema(SceUID, int);
int sceKernelDeleteSema(SceUID);
int sceKernelDelayThread(unsigned);
#endif
