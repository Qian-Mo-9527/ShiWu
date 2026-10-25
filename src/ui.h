#ifndef MARKET_UI_H
#define MARKET_UI_H
/* ui.h：两个程序共用的基础界面库——封装自绘控件、金额输入框、图片选择与显示、圆形头像和模态对话框，使前端与后台的视觉风格保持一致。 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define COBJMACROS
#include <windows.h>
#include "store.h"

extern HINSTANCE ui_instance;
extern HFONT ui_font, ui_small, ui_heading, ui_title, ui_price;
extern wchar_t ui_base[MAX_PATH];

/* 全局配色：背景、纯白、主文字、次要文字、分隔线、主题绿、必填红。 */
#define C_BG RGB(245,246,250)
#define C_WHITE RGB(255,255,255)
#define C_INK RGB(31,42,55)
#define C_MUTED RGB(111,121,135)
#define C_LINE RGB(226,231,237)
#define C_ACCENT RGB(30,119,93)
#define C_RED RGB(222,75,63)

/* —— 初始化、路径与基础绘制 —— */
void ui_init(HINSTANCE instance);
void ui_cleanup(void);
void ui_path(const wchar_t* relative, wchar_t* absolute, size_t capacity);
void ui_database_path(wchar_t* out, size_t capacity);
void ui_fill(HDC dc, RECT r, COLORREF c);
void ui_round(HDC dc, RECT r, COLORREF c, int radius);
void ui_text(HDC dc, const wchar_t* text, RECT r, HFONT f, COLORREF color, UINT flags);
void ui_required_text(HDC dc, const wchar_t* text, RECT r, HFONT f, COLORREF color, UINT flags);

/* —— 控件 —— */
HWND ui_label(HWND parent, const wchar_t* text, int x, int y, int w, int h);
HWND ui_edit(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, DWORD style, int limit);
HWND ui_money_edit(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, int limit);
HWND ui_button(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, int primary);
void ui_draw_button(const DRAWITEMSTRUCT* d);
void ui_get(HWND parent, int id, wchar_t* out, int capacity);
void ui_clear(HWND parent);

/* —— 图片、头像与提示 —— */
int ui_pick_image(HWND owner, wchar_t* relative, size_t capacity);
void ui_image(HDC dc, const wchar_t* path, RECT r, int cover);
void ui_avatar(HDC dc, const wchar_t* path, RECT r, const wchar_t* name);
void ui_error(HWND owner, Store* s);

/* —— 窗口定位与模态对话框 —— */
void ui_center(HWND hwnd, int width, int height);
int ui_modal(HWND dialog, HWND parent);
void ui_end_modal(HWND dialog, int result);
#endif
