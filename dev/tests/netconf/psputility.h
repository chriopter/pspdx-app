enum { PSP_UTILITY_DIALOG_NONE, PSP_UTILITY_DIALOG_INIT,
       PSP_UTILITY_DIALOG_VISIBLE, PSP_UTILITY_DIALOG_QUIT };
enum { PSP_SYSTEMPARAM_ID_INT_LANGUAGE, PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP };
enum { PSP_NETCONF_ACTION_CONNECTAP };
typedef struct {
    struct {
        unsigned size;
        int language, buttonSwap, graphicsThread, accessThread, fontThread, soundThread, result;
    } base;
    int action;
} pspUtilityNetconfData;
int sceUtilityGetSystemParamInt(int, int *);
int sceUtilityCheckNetParam(int);
int sceUtilityNetconfGetStatus(void);
int sceUtilityNetconfUpdate(int);
int sceUtilityNetconfShutdownStart(void);
int sceUtilityNetconfInitStart(pspUtilityNetconfData *);
