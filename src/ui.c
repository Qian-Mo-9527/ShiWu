#define _CRT_SECURE_NO_WARNINGS
#include "ui.h"
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <imm.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#pragma comment(lib,"imm32.lib")

HINSTANCE ui_instance;                  /* 应用实例句柄 */
HFONT ui_font, ui_small, ui_heading, ui_title, ui_price;  /* 全局共用的几种字号字体 */
wchar_t ui_base[MAX_PATH];              /* 可执行文件所在目录，所有相对路径都基于它解析 */
static IWICImagingFactory* wic;         /* WIC 图像工厂，用于解码 JPG/PNG */
static int modal_result;                /* 模态对话框的返回值 */
/* 已解码图片缓存项：绝对路径 + 显示用缩略位图 + 缩略图宽高及字节数；磁盘原图保持不变。 */
typedef struct { wchar_t path[1024]; HBITMAP bitmap; int w, h; size_t bytes; } CachedImage;
#define IMAGE_CACHE_MAX_DIMENSION 768
#define IMAGE_CACHE_BUDGET ((size_t)64 * 1024 * 1024)
static CachedImage cache[80];           /* 同时限制 80 张与 64 MiB，超出任一上限便淘汰最旧的一张 */
static int cache_count;
static size_t cache_bytes;

/* 缓存位图只在绘制期间临时选入 DC，绘制结束即取出，因此淘汰时可直接释放。 */
static void evict_oldest_image(void) {
	if (!cache_count)return;
	DeleteObject(cache[0].bitmap); cache_bytes -= cache[0].bytes;
	--cache_count;
	if (cache_count)memmove(cache, cache + 1, (size_t)cache_count * sizeof(*cache));
	ZeroMemory(&cache[cache_count], sizeof(*cache));
}
static void clear_image_cache(void) {
	for (int i = 0; i < cache_count; ++i)DeleteObject(cache[i].bitmap);
	ZeroMemory(cache, sizeof(cache)); cache_count = 0; cache_bytes = 0;
}

static HFONT font(int height, int weight) {
	return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
}
/* 初始化全局界面资源：声明 DPI 感知、初始化 COM 与 WIC、创建字体，并确保 data 与 data\images 目录存在。 */
void ui_init(HINSTANCE instance) {
	ui_instance = instance;
	SetProcessDPIAware();          /* 启用 DPI 感知，避免高分屏下界面模糊 */
	OleInitialize(NULL);           /* WIC 基于 COM，使用前先初始化 */
	CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void**)&wic);
	/* 取 exe 完整路径并去掉文件名，得到程序根目录 ui_base。 */
	GetModuleFileNameW(NULL, ui_base, MAX_PATH);
	PathRemoveFileSpecW(ui_base);
	ui_font = font(17, FW_NORMAL); ui_small = font(14, FW_NORMAL);
	ui_heading = font(23, FW_SEMIBOLD); ui_title = font(32, FW_BOLD); ui_price = font(28, FW_BOLD);
	wchar_t path[MAX_PATH];
	/* 首次运行时创建数据库目录与用户图片目录，目录已存在时 CreateDirectory 会被忽略。 */
	ui_path(L"data", path, MAX_PATH); CreateDirectoryW(path, NULL);
	ui_path(L"data\\images", path, MAX_PATH); CreateDirectoryW(path, NULL);
}
/* 退出时释放全部资源：清空图片缓存、删除字体、释放 WIC 工厂并关闭 COM。 */
void ui_cleanup(void) {
	clear_image_cache();
	DeleteObject(ui_font); DeleteObject(ui_small); DeleteObject(ui_heading); DeleteObject(ui_title); DeleteObject(ui_price);
	if (wic) { IWICImagingFactory_Release(wic); wic = NULL; }
	OleUninitialize();
}
/* 把相对程序目录的路径拼成绝对路径；传入的本就是绝对路径时原样复制。 */
void ui_path(const wchar_t* relative, wchar_t* absolute, size_t capacity) {
	if (!relative || !*relative) { if (capacity) *absolute = 0; return; }
	if (PathIsRelativeW(relative)) swprintf_s(absolute, capacity, L"%s\\%s", ui_base, relative);
	else wcsncpy_s(absolute, capacity, relative, _TRUNCATE);
}
/* 返回固定的数据库文件路径 data\market.db。 */
void ui_database_path(wchar_t* out, size_t capacity) { ui_path(L"data\\market.db", out, capacity); }
/* 用纯色填充矩形。 */
void ui_fill(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
/* 画填充圆角矩形（NULL_PEN 表示不描边）。 */
void ui_round(HDC dc, RECT r, COLORREF c, int radius) {
	HBRUSH b = CreateSolidBrush(c); HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
	RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius); SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
}
/* 在指定矩形内按 flags 对齐方式绘制透明背景文字。 */
void ui_text(HDC dc, const wchar_t* text, RECT r, HFONT f, COLORREF color, UINT flags) {
	HFONT old = SelectObject(dc, f); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
	DrawTextW(dc, text ? text : L"", -1, &r, flags | DT_NOPREFIX); SelectObject(dc, old);
}
/* 绘制标签文字，其中 '*' 用红色显示（必填标记），其余用正常颜色；按“是否为星号”分段输出并逐段累加宽度，保证红星位置与正常排版一致。 */
void ui_required_text(HDC dc, const wchar_t* text, RECT r, HFONT f, COLORREF color, UINT flags) {
	if (!text || !wcschr(text, L'*')) { ui_text(dc, text, r, f, color, flags); return; }
	int saved = SaveDC(dc); HFONT old = SelectObject(dc, f ? f : GetStockObject(DEFAULT_GUI_FONT));
	SetBkMode(dc, TRANSPARENT); SIZE size; int n = (int)wcslen(text);
	GetTextExtentPoint32W(dc, text, n, &size);
	int x = r.left, y = r.top;
	if (flags & DT_CENTER)x += (r.right - r.left - size.cx) / 2;
	else if (flags & DT_RIGHT)x = r.right - size.cx;
	if (flags & DT_VCENTER)y += (r.bottom - r.top - size.cy) / 2;
	else if (flags & DT_BOTTOM)y = r.bottom - size.cy;
	IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
	for (int i = 0; i < n;) {
		int end = i + 1; int star = text[i] == L'*';
		while (end < n && (text[end] == L'*') == star)end++;
		SetTextColor(dc, star ? C_RED : color); TextOutW(dc, x, y, text + i, end - i);
		GetTextExtentPoint32W(dc, text + i, end - i, &size); x += size.cx; i = end;
	}
	SelectObject(dc, old); RestoreDC(dc, saved);
}
/* 自绘必填标签：先向父窗口请求背景刷以保持底色一致，再用红星逻辑输出文字。 */
static void paint_required_label(HWND hwnd, HDC dc) {
	RECT r; GetClientRect(hwnd, &r);
	HBRUSH b = (HBRUSH)SendMessageW(GetParent(hwnd), WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)hwnd);
	if (!b)b = GetSysColorBrush(COLOR_WINDOW); FillRect(dc, &r, b);
	wchar_t label[256]; GetWindowTextW(hwnd, label, 256);
	ui_required_text(dc, label, r, (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0), GetTextColor(dc), DT_SINGLELINE);
}
/* 标签子类化过程：拦截绘制消息改用红星画法，并处理 WM_PRINTCLIENT 以兼容双缓冲。 */
static LRESULT CALLBACK required_label_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
	(void)data;
	switch (msg) {
	case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); paint_required_label(hwnd, dc); EndPaint(hwnd, &ps); return 0; }
	case WM_PRINTCLIENT:paint_required_label(hwnd, (HDC)w); return 0;
	case WM_SETTEXT: { LRESULT result = DefSubclassProc(hwnd, msg, w, l); InvalidateRect(hwnd, NULL, TRUE); return result; }
	case WM_NCDESTROY:RemoveWindowSubclass(hwnd, required_label_proc, id); break;
	}
	return DefSubclassProc(hwnd, msg, w, l);
}
/* 创建静态标签；文本含 '*' 时自动挂子类化，把星号渲染成红色。 */
HWND ui_label(HWND p, const wchar_t* text, int x, int y, int w, int h) {
	HWND a = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, w, h, p, NULL, ui_instance, NULL);
	if (a && text && wcschr(text, L'*'))SetWindowSubclass(a, required_label_proc, 1, 0);
	SendMessageW(a, WM_SETFONT, (WPARAM)ui_font, TRUE); return a;
}
/* 创建文本输入框；单行框加 ES_AUTOHSCROLL，可设置最大字符数 limit 与内边距。 */
HWND ui_edit(HWND p, int id, const wchar_t* text, int x, int y, int w, int h, DWORD style, int limit) {
	DWORD editing = (style & ES_MULTILINE) ? 0 : ES_AUTOHSCROLL;
	HWND a = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | editing | style, x, y, w, h, p, (HMENU)(INT_PTR)id, ui_instance, NULL);
	SendMessageW(a, WM_SETFONT, (WPARAM)ui_font, TRUE); SendMessageW(a, EM_SETLIMITTEXT, limit, 0);
	SendMessageW(a, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(9, 9)); return a;
}
/* 编辑过程中允许留空或尚未输入完整的小数；提交时仍会通过money_parse 校验必填、正数取值以及余额上限。 */
static int money_text_valid(HWND hwnd, const wchar_t* text) {
	if (!text)return 1;
	size_t n = 0, decimals = 0; int dot = 0;
	LRESULT limit = SendMessageW(hwnd, EM_GETLIMITTEXT, 0, 0);
	while (text[n]) {
		if (n >= 1022 || n >= (size_t)limit)return 0;
		wchar_t c = text[n++];
		if (c == L'.') { if (dot)return 0; dot = 1; }
		else if (c < L'0' || c>L'9')return 0;
		else if (dot && ++decimals > 2)return 0;
	}
	return 1;
}
/* 在当前选区插入/替换文本并即时校验：以小数点开头时自动补成“0.”；校验不通过（多个小数点、小数位超限等）则拒绝本次输入。 */
static LRESULT money_replace(HWND hwnd, WPARAM undo, const wchar_t* insert) {
	wchar_t before[1024], after[1024]; DWORD start = 0, end = 0;
	GetWindowTextW(hwnd, before, 1024); size_t n = wcslen(before), add = insert ? wcslen(insert) : 0;
	SendMessageW(hwnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
	if (start > n)start = (DWORD)n; if (end > n)end = (DWORD)n;
	if (!wcscmp(insert ? insert : L"", L".") && start == 0 && end == n) { insert = L"0."; add = 2; }
	if (add >= 1024 || start + add + n - end >= 1024)return 0;
	wmemcpy(after, before, start); if (add)wmemcpy(after + start, insert, add);
	wcscpy_s(after + start + add, 1024 - start - add, before + end);
	if (!money_text_valid(hwnd, after))return 0;
	return DefSubclassProc(hwnd, EM_REPLACESEL, undo, (LPARAM)(insert ? insert : L""));
}
/* 失焦时规范化显示：开头的“.”补为“0.”，结尾的“.”去掉，让显示更整洁。 */
static void money_normalize(HWND hwnd) {
	wchar_t value[1024], normalized[1024]; GetWindowTextW(hwnd, value, 1024);
	if (!*value)return;
	swprintf_s(normalized, 1024, L"%s%s", *value == L'.' ? L"0" : L"", value);
	size_t n = wcslen(normalized); if (n && normalized[n - 1] == L'.')normalized[n - 1] = 0;
	if (wcscmp(value, normalized) && money_text_valid(hwnd, normalized)) {
		SetWindowTextW(hwnd, normalized); SendMessageW(hwnd, EM_SETSEL, wcslen(normalized), wcslen(normalized));
	}
}
/* 金额输入框子类化：只放行数字和一个小数点（小数点后最多两位），屏蔽其余可打印字符；Ctrl+A 全选，Ctrl+V 改走受控 WM_PASTE 过滤剪贴板；失焦时规范化。 */
static LRESULT CALLBACK money_edit_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
	(void)data;
	switch (msg) {
	case WM_CHAR:
		if (w >= L'0' && w <= L'9' || w == L'.') {
			wchar_t digit[2] = { (wchar_t)w,0 }; money_replace(hwnd, TRUE, digit); return 0;
		}
		if (w == 1) { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
		if (w == 22) { SendMessageW(hwnd, WM_PASTE, 0, 0); return 0; }
		if (w >= 32)return 0;
		break;
	case WM_IME_CHAR:return SendMessageW(hwnd, WM_CHAR, w, l);
	case WM_SETTEXT:
		if (!money_text_valid(hwnd, (const wchar_t*)l))return FALSE;
		break;
	case EM_REPLACESEL:return money_replace(hwnd, w, (const wchar_t*)l);
	case WM_PASTE:
		if (OpenClipboard(hwnd)) {
			HANDLE handle = GetClipboardData(CF_UNICODETEXT);
			if (handle) {
				const wchar_t* text = GlobalLock(handle); SIZE_T bytes = GlobalSize(handle);
				if (text) {
					size_t cap = bytes / sizeof(wchar_t); if (cap > 1024)cap = 1024;
					if (wmemchr(text, 0, cap))money_replace(hwnd, TRUE, text);
					GlobalUnlock(handle);
				}
			}
			CloseClipboard();
		}
		return 0;
	case WM_KILLFOCUS:money_normalize(hwnd); break;
	case WM_NCDESTROY:RemovePropW(hwnd, L"money_edit"); RemoveWindowSubclass(hwnd, money_edit_proc, id); break;
	}
	return DefSubclassProc(hwnd, msg, w, l);
}
/* 创建金额专用输入框：挂 money_edit_proc 子类化并关闭输入法，避免输入中文。 */
HWND ui_money_edit(HWND p, int id, const wchar_t* text, int x, int y, int w, int h, int limit) {
	HWND edit = ui_edit(p, id, text, x, y, w, h, ES_NUMBER, limit);
	if (edit) {
		SetPropW(edit, L"money_edit", (HANDLE)1); SetWindowSubclass(edit, money_edit_proc, 1, 0);
		ImmAssociateContext(edit, NULL);
	}
	return edit;
}
/* 创建自绘按钮。primary 控制样式：0 普通浅灰、1 主色实心（强调）、2 浅绿次级；该值通过窗口属性 primary 传给绘制函数。 */
HWND ui_button(HWND p, int id, const wchar_t* text, int x, int y, int w, int h, int primary) {
	HWND a = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW, x, y, w, h, p, (HMENU)(INT_PTR)id, ui_instance, NULL);
	SetPropW(a, L"primary", (HANDLE)(INT_PTR)primary); SendMessageW(a, WM_SETFONT, (WPARAM)ui_font, TRUE); return a;
}
/* 自绘按钮：根据 primary 与禁用/按下状态选择配色，画圆角并居中文字；文字含 '*' 时使用红星画法，获得焦点时绘制焦点框。 */
void ui_draw_button(const DRAWITEMSTRUCT* d) {
	int primary = (int)(INT_PTR)GetPropW(d->hwndItem, L"primary");
	int disabled = (d->itemState & ODS_DISABLED) != 0, pressed = (d->itemState & ODS_SELECTED) != 0;
	COLORREF bg = primary ? C_ACCENT : RGB(235, 240, 243), fg = primary ? C_WHITE : C_INK;
	if (primary == 2) { bg = RGB(226, 239, 232); fg = C_ACCENT; }
	if (disabled) { bg = RGB(231, 233, 236); fg = C_MUTED; }
	else if (pressed) bg = primary ? RGB(22, 91, 72) : RGB(216, 226, 231);
	ui_round(d->hDC, d->rcItem, bg, 12);
	wchar_t t[200]; GetWindowTextW(d->hwndItem, t, 200);
	if (wcschr(t, L'*'))ui_required_text(d->hDC, t, d->rcItem, ui_font, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	else ui_text(d->hDC, t, d->rcItem, ui_font, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	if (d->itemState & ODS_FOCUS) { RECT r = d->rcItem; InflateRect(&r, -4, -4); DrawFocusRect(d->hDC, &r); }
}
/* 读取某个输入框文本；若是金额框先规范化（补 0、去尾点）再取值。 */
void ui_get(HWND p, int id, wchar_t* out, int cap) {
	HWND edit = GetDlgItem(p, id); if (edit && GetPropW(edit, L"money_edit"))money_normalize(edit);
	GetDlgItemTextW(p, id, out, cap);
}
/* 销毁窗口内全部子控件，用于切换/重绘内容前清空界面。 */
void ui_clear(HWND p) { HWND child; while ((child = GetWindow(p, GW_CHILD)) != NULL)DestroyWindow(child); }
/* 用 WIC 解码 JPG/PNG 为 32 位自顶向下 HBITMAP。先校验原图尺寸与总像素，
   再把显示用位图的最长边缩至 768；只读原文件，避免照片原尺寸一直占用缓存内存。
   任何一步失败都释放已申请对象并返回 NULL。 */
static HBITMAP load_bitmap(const wchar_t* path, int* width, int* height) {
	IWICBitmapDecoder* dec = NULL; IWICBitmapFrameDecode* frame = NULL; IWICFormatConverter* conv = NULL; IWICBitmapScaler* scaler = NULL;
	UINT w = 0, h = 0; HBITMAP bmp = NULL; void* bits = NULL; HRESULT hr = E_FAIL;
	if (!wic)return NULL;
	hr = IWICImagingFactory_CreateDecoderFromFilename(wic, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec);
	if (SUCCEEDED(hr))hr = IWICBitmapDecoder_GetFrame(dec, 0, &frame);
	if (SUCCEEDED(hr))hr = IWICBitmapFrameDecode_GetSize(frame, &w, &h);
	/* 宽高任一不超过 12000、总像素不超过 4000 万，否则视为无效图片。 */
	if (SUCCEEDED(hr) && (w == 0 || h == 0 || w > 12000 || h > 12000 || (uint64_t)w * h > 40000000))hr = E_INVALIDARG;
	IWICBitmapSource* source = (IWICBitmapSource*)frame;
	if (SUCCEEDED(hr) && (w > IMAGE_CACHE_MAX_DIMENSION || h > IMAGE_CACHE_MAX_DIMENSION)) {
		UINT scaled_w, scaled_h;
		if (w >= h) {
			scaled_w = IMAGE_CACHE_MAX_DIMENSION;
			scaled_h = (UINT)(((uint64_t)h * IMAGE_CACHE_MAX_DIMENSION + w / 2) / w);
		}
		else {
			scaled_h = IMAGE_CACHE_MAX_DIMENSION;
			scaled_w = (UINT)(((uint64_t)w * IMAGE_CACHE_MAX_DIMENSION + h / 2) / h);
		}
		if (!scaled_w)scaled_w = 1; if (!scaled_h)scaled_h = 1;
		hr = IWICImagingFactory_CreateBitmapScaler(wic, &scaler);
		if (SUCCEEDED(hr))hr = IWICBitmapScaler_Initialize(scaler, source, scaled_w, scaled_h, WICBitmapInterpolationModeFant);
		if (SUCCEEDED(hr)) { source = (IWICBitmapSource*)scaler; w = scaled_w; h = scaled_h; }
	}
	if (SUCCEEDED(hr))hr = IWICImagingFactory_CreateFormatConverter(wic, &conv);
	if (SUCCEEDED(hr))hr = IWICFormatConverter_Initialize(conv, source, &GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom);
	if (SUCCEEDED(hr)) {
		BITMAPINFO bi = { 0 }; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w;
		bi.bmiHeader.biHeight = -(int)h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
		bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
		if (!bmp)hr = E_OUTOFMEMORY;
		else hr = IWICFormatConverter_CopyPixels(conv, NULL, w * 4, w * h * 4, bits);
	}
	if (FAILED(hr) && bmp) { DeleteObject(bmp); bmp = NULL; }
	if (conv)IWICFormatConverter_Release(conv); if (scaler)IWICBitmapScaler_Release(scaler); if (frame)IWICBitmapFrameDecode_Release(frame); if (dec)IWICBitmapDecoder_Release(dec);
	if (bmp) { *width = w; *height = h; }return bmp;
}
/* 弹出系统选图对话框：校验扩展名（JPG/PNG）、文件大小（≤20MB）且能正常解码，再用 GUID 重命名复制到 data\images，避免重名覆盖；最终回传相对路径。 */
int ui_pick_image(HWND owner, wchar_t* relative, size_t capacity) {
	wchar_t path[MAX_PATH] = L""; OPENFILENAMEW of = { 0 }; of.lStructSize = sizeof(of); of.hwndOwner = owner;
	of.lpstrFilter = L"图片（JPG、PNG）\0*.jpg;*.jpeg;*.png\0\0"; of.lpstrFile = path; of.nMaxFile = MAX_PATH;
	of.lpstrTitle = L"选择一张图片"; of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&of))return 0;
	const wchar_t* ext = PathFindExtensionW(path);
	if (_wcsicmp(ext, L".jpg") && _wcsicmp(ext, L".jpeg") && _wcsicmp(ext, L".png")) {
		MessageBoxW(owner, L"请选择 JPG 或 PNG 图片。", L"图片格式", MB_OK | MB_ICONINFORMATION); return 0;
	}
	WIN32_FILE_ATTRIBUTE_DATA info;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info) || info.nFileSizeHigh || info.nFileSizeLow > 20 * 1024 * 1024) {
		MessageBoxW(owner, L"图片无法读取，或超过 20 MB。", L"图片读取失败", MB_OK | MB_ICONWARNING); return 0;
	}
	int w, h; HBITMAP check = load_bitmap(path, &w, &h);
	if (!check) { MessageBoxW(owner, L"图片无法解码，请选择有效的 JPG 或 PNG 图片。", L"图片读取失败", MB_OK | MB_ICONWARNING); return 0; }
	DeleteObject(check);
	GUID guid; wchar_t guidtext[48], dest[MAX_PATH], rel[128]; CoCreateGuid(&guid); StringFromGUID2(&guid, guidtext, 48);
	swprintf_s(rel, 128, L"data\\images\\%s%s", guidtext, ext); ui_path(rel, dest, MAX_PATH);
	if (!CopyFileW(path, dest, TRUE)) { MessageBoxW(owner, L"图片保存失败，请确认程序所在文件夹可以写入。", L"保存失败", MB_OK | MB_ICONERROR); return 0; }
	wcsncpy_s(relative, capacity, rel, _TRUNCATE); return 1;
}
/* 在矩形 r 内绘制图片：先查缓存，未命中则解码显示用缩略图并加入；
   缓存最多 80 张、合计 64 MiB，按先入先出释放旧项。cover=1 按“铺满裁剪”缩放，
   否则按“完整包含”缩放；图片缺失时显示占位文字。 */
void ui_image(HDC dc, const wchar_t* path, RECT r, int cover) {
	wchar_t abs[1024]; ui_path(path, abs, 1024); CachedImage* im = NULL;
	if (!*abs) { ui_round(dc, r, RGB(233, 238, 239), 12); return; }
	for (int i = 0; i < cache_count; i++)if (!wcscmp(cache[i].path, abs)) { im = &cache[i]; break; }
	if (!im) {
		CachedImage next = { 0 }; wcsncpy_s(next.path, 1024, abs, _TRUNCATE); next.bitmap = load_bitmap(abs, &next.w, &next.h);
		if (next.bitmap) {
			next.bytes = (size_t)next.w * next.h * 4;
			while (cache_count && (cache_count == (int)_countof(cache) || cache_bytes + next.bytes > IMAGE_CACHE_BUDGET))evict_oldest_image();
			if (next.bytes <= IMAGE_CACHE_BUDGET) {
				cache[cache_count] = next; cache_bytes += next.bytes; im = &cache[cache_count++];
			}
			else DeleteObject(next.bitmap);
		}
	}
	ui_fill(dc, r, RGB(239, 242, 241));
	if (!im) { ui_text(dc, L"图片暂不可用", r, ui_small, C_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE); return; }
	int rw = r.right - r.left, rh = r.bottom - r.top; double sx = (double)rw / im->w, sy = (double)rh / im->h;
	double scale = cover ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy); int dw = (int)(im->w * scale), dh = (int)(im->h * scale);
	HDC mem = CreateCompatibleDC(dc); HGDIOBJ old = SelectObject(mem, im->bitmap);
	int saved = SaveDC(dc); IntersectClipRect(dc, r.left, r.top, r.right, r.bottom); SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL);
	StretchBlt(dc, r.left + (rw - dw) / 2, r.top + (rh - dh) / 2, dw, dh, mem, 0, 0, im->w, im->h, SRCCOPY);
	RestoreDC(dc, saved); SelectObject(mem, old); DeleteDC(mem);
}
/* 绘制圆形头像：用椭圆区域裁剪；有图片则铺满显示，否则用浅绿底+昵称首字占位。 */
void ui_avatar(HDC dc, const wchar_t* path, RECT r, const wchar_t* name) {
	HRGN clip = CreateEllipticRgn(r.left, r.top, r.right, r.bottom); int save = SaveDC(dc); ExtSelectClipRgn(dc, clip, RGN_AND);
	if (path && *path)ui_image(dc, path, r, 1);
	else { ui_fill(dc, r, RGB(220, 236, 224)); wchar_t initial[2] = { name && *name ? *name : L'我',0 }; ui_text(dc, initial, r, ui_heading, C_ACCENT, DT_CENTER | DT_VCENTER | DT_SINGLELINE); }
	RestoreDC(dc, save); DeleteObject(clip);
}
/* 统一弹出最近一次数据层错误信息。 */
void ui_error(HWND owner, Store* s) { MessageBoxW(owner, store_error(s), L"操作提示", MB_OK | MB_ICONWARNING); }
/* 依据屏幕工作区把窗口居中显示，并限制窗口尺寸不超出工作区。 */
void ui_center(HWND hwnd, int w, int h) {
	RECT work; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	if (w > work.right - work.left)w = work.right - work.left; if (h > work.bottom - work.top)h = work.bottom - work.top;
	SetWindowPos(hwnd, NULL, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h, SWP_NOZORDER);
}
/* 进入模态对话框循环：禁用父窗口、显示对话框并自建消息循环，直到对话框关闭，随后恢复父窗口并返回对话框结果。 */
int ui_modal(HWND dialog, HWND parent) {
	MSG msg; modal_result = 0; EnableWindow(parent, FALSE); ShowWindow(dialog, SW_SHOW); SetForegroundWindow(dialog);
	while (IsWindow(dialog) && GetMessageW(&msg, NULL, 0, 0) > 0) { if (!IsDialogMessageW(dialog, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
	EnableWindow(parent, TRUE); SetForegroundWindow(parent); return modal_result;
}
/* 保存模态结果并销毁对话框，使 ui_modal 的消息循环结束。 */
void ui_end_modal(HWND dialog, int result) { modal_result = result; DestroyWindow(dialog); }
