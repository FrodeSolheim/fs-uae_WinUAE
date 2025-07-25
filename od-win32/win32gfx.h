#ifndef __WIN32GFX_H__
#define __WIN32GFX_H__

#ifdef FSUAE
#include "uae/compat/windows.h"
#endif

#define RTG_MODE_SCALE 1
#define RTG_MODE_CENTER 2
#define RTG_MODE_INTEGER_SCALE 3

extern void sortdisplays (void);
extern void enumeratedisplays (void);
extern void reenumeratemonitors(void);

int WIN32GFX_IsPicassoScreen(struct AmigaMonitor*);
int WIN32GFX_GetWidth(struct AmigaMonitor*);
int WIN32GFX_GetHeight(struct AmigaMonitor*);
void WIN32GFX_DisplayChangeRequested(int);
void DX_Invalidate(struct AmigaMonitor*, int x, int y, int width, int height);

int WIN32GFX_AdjustScreenmode(struct MultiDisplay *md, int *pwidth, int *pheight);
extern HCURSOR normalcursor;

extern int default_freq;
extern int normal_display_change_starting;
extern int window_led_drives, window_led_drives_end;
extern int window_led_hd, window_led_hd_end;
extern int window_led_joys, window_led_joys_end, window_led_joy_start;
extern int window_led_msg, window_led_msg_end, window_led_msg_start;
extern int on_screen_keyboard;

extern HDC gethdc(int monid);
extern void releasehdc(int monid, HDC hdc);
extern void close_windows(struct AmigaMonitor*);
extern void updatewinfsmode(int monid, struct uae_prefs *p);
extern void gfx_lock(void);
extern void gfx_unlock(void);

void centerdstrect(struct AmigaMonitor*, RECT *);
struct MultiDisplay *getdisplay(struct uae_prefs *p, int monid);
extern int getrefreshrate(int monid, int width, int height);

#endif
