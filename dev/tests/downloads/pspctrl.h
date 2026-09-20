#ifndef TEST_PSPCTRL_H
#define TEST_PSPCTRL_H
#define PSP_CTRL_CIRCLE 0x2000
#define PSP_CTRL_SQUARE 0x8000
#define PSP_CTRL_SELECT 1
#define PSP_CTRL_START 8
#define PSP_CTRL_UP 0x10
#define PSP_CTRL_RIGHT 0x20
#define PSP_CTRL_DOWN 0x40
#define PSP_CTRL_LEFT 0x80
#define PSP_CTRL_LTRIGGER 0x100
#define PSP_CTRL_RTRIGGER 0x200
#define PSP_CTRL_TRIANGLE 0x1000
#define PSP_CTRL_CROSS 0x4000
typedef struct {
    unsigned TimeStamp, Buttons;
    unsigned char Lx, Ly;
} SceCtrlData;
#endif
