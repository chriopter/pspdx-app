#ifndef TEST_PSPIO_H
#define TEST_PSPIO_H
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
typedef int SceUID;
typedef size_t SceSize;
typedef long long SceOff;
typedef struct {
    unsigned st_mode;
    unsigned long long st_size;
} SceIoStat;
typedef struct {
    SceIoStat d_stat;
    char d_name[256];
} SceIoDirent;
#define FIO_S_ISDIR S_ISDIR
#define PSP_O_RDONLY O_RDONLY
#define PSP_O_WRONLY O_WRONLY
#define PSP_O_CREAT O_CREAT
#define PSP_O_TRUNC O_TRUNC
#define PSP_O_APPEND O_APPEND
#define PSP_O_EXCL O_EXCL
#define PSP_SEEK_SET SEEK_SET
#define PSP_SEEK_END SEEK_END
int sceIoOpen(const char *, int, int);
int sceIoClose(int);
int sceIoRead(int, void *, size_t);
int sceIoWrite(int, const void *, size_t);
SceOff sceIoLseek(int, SceOff, int);
int sceIoLseek32(int, int, int);
int sceIoRename(const char *, const char *);
int sceIoRemove(const char *);
int sceIoRmdir(const char *);
int sceIoMkdir(const char *, int);
int sceIoGetstat(const char *, SceIoStat *);
int sceIoSync(const char *, int);
int sceIoDopen(const char *);
int sceIoDclose(int);
int sceIoDread(int, SceIoDirent *);
#endif
