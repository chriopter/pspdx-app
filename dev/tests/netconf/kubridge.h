typedef struct {
    char modname[32];
    unsigned char version[2];
    unsigned text_size;
} SceModule;
int kuKernelGetModel(void);
int kuKernelFindModuleByName(char *, SceModule *);
