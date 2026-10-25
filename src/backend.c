#define _CRT_SECURE_NO_WARNINGS
#include "ui.h"
#include <commctrl.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"uxtheme.lib")

/* 本程序刻意不设置登录或角色权限系统。两个程序共用同一个 SQLite 文件，所有写入操作都经由 store.c 完成。 */
/* 控件 ID：100 段为主窗口按钮/列表；200 段起为编辑对话框内的输入控件与保存/取消。 */
enum {
	ID_USERS = 100, ID_PRODUCTS, ID_ADD, ID_EDIT, ID_DELETE, ID_REFRESH,
	ID_LIST, ID_FOOTER,
	E_ACCOUNT = 200, E_PASSWORD, E_NICKNAME, E_BIO, E_BALANCE, E_PICK,
	E_NAME, E_DESCRIPTION, E_PRICE, E_SELLER, E_STATUS, E_SAVE, E_CANCEL
};

/* 用户编辑对话框的上下文：待编辑用户 + 暂存的头像路径。 */
typedef struct UserEditor {
	User value;
	wchar_t avatar[512];
} UserEditor;
/* 商品编辑对话框的上下文：待编辑商品 + 暂存图片路径 + 卖家下拉所需的全部用户。 */
typedef struct ProductEditor {
	Product value;
	wchar_t image[512];
	User* users;
	int count;
} ProductEditor;

static Store db;                                        /* 全局数据库连接 */
static HWND main_window, list_window, footer_window;    /* 主窗口、列表控件、底部统计标签 */
static int current_tab;                                 /* 当前页签：0 用户、1 商品 */
static User* users;                                     /* 缓存的用户列表 */
static Product* products;                               /* 缓存的商品列表 */
static int user_count, product_count;
static HBRUSH brush_white, brush_bg;
/* 三个窗口类名：主窗口、用户编辑对话框、商品编辑对话框。 */
static const wchar_t MAIN_CLASS[] = L"ShiwuDataManager";
static const wchar_t USER_CLASS[] = L"ShiwuUserEditor";
static const wchar_t PRODUCT_CLASS[] = L"ShiwuProductEditor";

/* 用四个坐标快速构造一个 RECT。 */
static RECT box(int left, int top, int right, int bottom) {
	RECT r = { left,top,right,bottom }; return r;
}

/* 把整数分金额格式化为“元.两位小数”文本（后台表格与对话框使用）。 */
static void amount_text(Money amount, wchar_t* out, size_t capacity) {
	swprintf_s(out, capacity, L"%lld.%02lld", (long long)(amount / 100),
		(long long)(amount % 100));
}

/* 从程序目录加载窗口图标。 */
static HICON load_icon(const wchar_t* file) {
	wchar_t path[MAX_PATH];
	ui_path(file, path, MAX_PATH);
	return (HICON)LoadImageW(NULL, path, IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
}

/* 创建一个固定高度 24 的静态标签（对 ui_label 的简单封装）。 */
static void label(HWND hwnd, const wchar_t* text, int x, int y, int width) {
	ui_label(hwnd, text, x, y - 3, width, 24);
}

/* 保持底部按钮间距；工作区较矮时仍把保存与取消放在可见范围内。 */
static void position_editor_actions(HWND hwnd, int cancel_x, int save_x, int preferred_y) {
	RECT client; GetClientRect(hwnd, &client);
	int y = client.bottom - 68;
	if (y > preferred_y)y = preferred_y;
	if (y < 0)y = 0;
	HWND cancel = GetDlgItem(hwnd, E_CANCEL), save = GetDlgItem(hwnd, E_SAVE);
	if (cancel)MoveWindow(cancel, cancel_x, y, 100, 40, TRUE);
	if (save)MoveWindow(save, save_x, y, 126, 40, TRUE);
}

/* 给控件设置全局正文字体。 */
static void control_font(HWND control) {
	SendMessageW(control, WM_SETFONT, (WPARAM)ui_font, TRUE);
}

/* 绘制编辑对话框通用头部：白底、大标题、可选灰色副标题（hint 为空则不画）、以及标题下的分隔线。商品窗口传 NULL 即不显示副标题。 */
static void dialog_paint(HWND hwnd, HDC dc, const wchar_t* title, const wchar_t* hint) {
	RECT r; GetClientRect(hwnd, &r);
	ui_fill(dc, r, C_WHITE);
	ui_text(dc, title, box(28, 22, r.right - 28, 57), ui_title, C_INK, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
	if (hint && hint[0]) ui_text(dc, hint, box(28, 63, r.right - 28, 92), ui_small, C_MUTED, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
	ui_fill(dc, box(28, 104, r.right - 28, 105), C_LINE);
}

/* 编辑对话框内控件配色：白底深色文字；静态标签用透明背景以贴合自绘底色。 */
static LRESULT dialog_colors(UINT msg, WPARAM wp) {
	HDC dc = (HDC)wp;
	SetTextColor(dc, C_INK);
	SetBkColor(dc, C_WHITE);
	if (msg == WM_CTLCOLORSTATIC) SetBkMode(dc, TRANSPARENT);
	return (LRESULT)brush_white;
}

/* “新增/编辑用户”模态对话框的窗口过程。WM_CREATE 布置所有输入控件；WM_COMMAND 处理选头像、取消与保存；保存时把余额解析为分并调用 store_admin_user。 */
static LRESULT CALLBACK UserProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	UserEditor* e = (UserEditor*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	switch (msg) {
	case WM_NCCREATE:
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
		return DefWindowProcW(hwnd, msg, wp, lp);
	case WM_CREATE: {
		wchar_t balance[64];
		e = (UserEditor*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
		amount_text(e->value.balance, balance, 64);
		label(hwnd, L"登录账号", 28, 121, 340);
		ui_edit(hwnd, E_ACCOUNT, e->value.account, 28, 149, 340, 37, 0, 32);
		label(hwnd, e->value.id ? L"新密码（留空则保持原密码）" : L"密码", 28, 199, 385);
		ui_edit(hwnd, E_PASSWORD, L"", 28, 227, 340, 37, ES_PASSWORD, 64);
		label(hwnd, L"昵称", 28, 277, 340);
		ui_edit(hwnd, E_NICKNAME, e->value.nickname, 28, 305, 340, 37, 0, 32);
		label(hwnd, L"钱包余额（元）", 28, 355, 340);
		ui_money_edit(hwnd, E_BALANCE, balance, 28, 383, 340, 37, 16);
		label(hwnd, L"个人简介", 28, 433, 340);
		ui_edit(hwnd, E_BIO, e->value.bio, 28, 461, 340, 115, ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN, 256);
		ui_button(hwnd, E_PICK, L"选择头像", 414, 328, 204, 39, 0);
		label(hwnd, L"JPG / PNG · 图片保存在本地", 402, 380, 250);
		label(hwnd, e->value.id ? L"密码留空可保留原密码。" : L"新增用户时密码必填。", 402, 432, 250);
		label(hwnd, L"修改资料后立即写入本地数据。", 402, 460, 260);
		ui_button(hwnd, E_CANCEL, L"取消", 402, 608, 100, 40, 0);
		ui_button(hwnd, E_SAVE, L"保存用户", 518, 608, 126, 40, 1);
		position_editor_actions(hwnd, 402, 518, 608);
		SetFocus(GetDlgItem(hwnd, E_ACCOUNT));
		return 0;
	}
	case WM_PAINT: {
		PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
		dialog_paint(hwnd, dc, e->value.id ? L"编辑用户" : L"新增用户", NULL);
		ui_avatar(dc, e->avatar, box(437, 141, 597, 301), e->value.nickname[0] ? e->value.nickname : L"用户");
		EndPaint(hwnd, &ps); return 0;
	}
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case E_PICK:
			if (ui_pick_image(hwnd, e->avatar, 512)) InvalidateRect(hwnd, NULL, FALSE);
			return 0;
		case IDCANCEL: case E_CANCEL: ui_end_modal(hwnd, 0); return 0;
		case IDOK: case E_SAVE: {
			wchar_t account[33], password[65], nickname[33], bio[257], balance[64];
			Money cents; int id;
			ui_get(hwnd, E_ACCOUNT, account, 33); ui_get(hwnd, E_PASSWORD, password, 65);
			ui_get(hwnd, E_NICKNAME, nickname, 33); ui_get(hwnd, E_BIO, bio, 257);
			ui_get(hwnd, E_BALANCE, balance, 64);
			if (!money_parse(balance, 1, &cents)) {
				MessageBoxW(hwnd, L"请输入有效余额，范围 0.00 至 9999999.99 元，最多两位小数。", L"余额格式", MB_OK | MB_ICONINFORMATION);
				SetFocus(GetDlgItem(hwnd, E_BALANCE)); return 0;
			}
			User edited = e->value;
			wcscpy_s(edited.account, _countof(edited.account), account);
			wcscpy_s(edited.nickname, _countof(edited.nickname), nickname);
			wcscpy_s(edited.bio, _countof(edited.bio), bio);
			wcscpy_s(edited.avatar, _countof(edited.avatar), e->avatar);
			edited.balance = cents;
			/* 编辑时只提交实际改动，保留前端刚入账的余额和新资料。 */
			int saved = e->value.id ? store_edit_user(&db, &e->value, &edited, password)
				: store_admin_user(&db, 0, account, password, nickname, bio, e->avatar, cents, &id);
			if (!saved) {
				SecureZeroMemory(password, sizeof(password)); ui_error(hwnd, &db); return 0;
			}
			SecureZeroMemory(password, sizeof(password));
			ui_end_modal(hwnd, 1); return 0;
		}
		} break;
	case WM_SIZE:position_editor_actions(hwnd, 402, 518, 608); return 0;
	case WM_DRAWITEM: ui_draw_button((const DRAWITEMSTRUCT*)lp); return TRUE;
	case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: return dialog_colors(msg, wp);
	case WM_CLOSE: ui_end_modal(hwnd, 0); return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

/* “新增/编辑商品”模态对话框的窗口过程。WM_CREATE 布置控件并填充卖家下拉、状态下拉；保存时解析价格、读取卖家 id 与状态，调用 store_admin_product。 */
static LRESULT CALLBACK ProductProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	ProductEditor* e = (ProductEditor*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	switch (msg) {
	case WM_NCCREATE:
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
		return DefWindowProcW(hwnd, msg, wp, lp);
	case WM_CREATE: {
		wchar_t price[64]; HWND combo; int i, selected = 0;
		e = (ProductEditor*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
		amount_text(e->value.price, price, 64);
		label(hwnd, L"商品名称 *", 28, 121, 390);
		ui_edit(hwnd, E_NAME, e->value.name, 28, 149, 390, 37, 0, 80);
		label(hwnd, L"卖家 *", 28, 199, 390);
		combo = CreateWindowExW(WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
			28, 227, 390, 260, hwnd, (HMENU)(INT_PTR)E_SELLER, ui_instance, NULL);
		control_font(combo);
		/* 卖家下拉：显示“昵称(账号)”，并把对应用户 id 存到项的附加数据，便于保存时取回。 */
		for (i = 0; i < e->count; i++) {
			wchar_t name[100]; LRESULT item;
			swprintf_s(name, 100, L"%s  (%s)", e->users[i].nickname, e->users[i].account);
			item = SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)name);
			SendMessageW(combo, CB_SETITEMDATA, (WPARAM)item, (LPARAM)e->users[i].id);
			if (e->users[i].id == e->value.seller_id) selected = (int)item;
		}
		SendMessageW(combo, CB_SETCURSEL, selected, 0);
		label(hwnd, L"价格（元）*", 28, 279, 180);
		ui_money_edit(hwnd, E_PRICE, price, 28, 307, 180, 37, 16);
		label(hwnd, L"商品状态", 230, 279, 188);
		combo = CreateWindowExW(WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
			230, 307, 188, 160, hwnd, (HMENU)(INT_PTR)E_STATUS, ui_instance, NULL);
		control_font(combo);
		SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"未售出");
		SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"已售出");
		SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"商品已下架");
		SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"商品已被管理员下架");
		SendMessageW(combo, CB_SETCURSEL, e->value.sold ? 1 : e->value.withdrawn == 1 ? 2 : e->value.withdrawn == 2 ? 3 : 0, 0);
		label(hwnd, L"商品介绍 *", 28, 357, 390);
		ui_edit(hwnd, E_DESCRIPTION, e->value.description, 28, 385, 390, 217, ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN, 1000);
		ui_button(hwnd, E_PICK, L"选择商品图片 *", 450, 353, 212, 39, 0);
		label(hwnd, L"JPG / PNG · 每件商品一张图片", 442, 414, 255);
		label(hwnd, L"商品信息直接同步至前端。", 442, 454, 255);
		label(hwnd, L"已有成交的商品不可下架。", 442, 483, 255);
		ui_button(hwnd, E_CANCEL, L"取消", 420, 634, 100, 40, 0);
		ui_button(hwnd, E_SAVE, L"保存商品", 536, 634, 126, 40, 1);
		position_editor_actions(hwnd, 420, 536, 634);
		SetFocus(GetDlgItem(hwnd, E_NAME));
		return 0;
	}
	case WM_PAINT: {
		PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
		dialog_paint(hwnd, dc, e->value.id ? L"编辑商品" : L"新增商品", NULL);
		ui_round(dc, box(450, 141, 662, 329), C_BG, 12);
		if (e->image[0]) ui_image(dc, e->image, box(456, 147, 656, 323), 0);
		else ui_text(dc, L"商品图片", box(450, 141, 662, 329), ui_font, C_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		EndPaint(hwnd, &ps); return 0;
	}
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case E_PICK:
			if (ui_pick_image(hwnd, e->image, 512)) InvalidateRect(hwnd, NULL, FALSE);
			return 0;
		case IDCANCEL: case E_CANCEL: ui_end_modal(hwnd, 0); return 0;
		case IDOK: case E_SAVE: {
			wchar_t name[81], description[1001], price[64];
			Money cents; int seller, sold, withdrawn, id; LRESULT selection;
			ui_get(hwnd, E_NAME, name, 81); ui_get(hwnd, E_DESCRIPTION, description, 1001);
			ui_get(hwnd, E_PRICE, price, 64);
			if (!money_parse(price, 0, &cents)) {
				MessageBoxW(hwnd, L"请输入有效价格，范围 0.01 至 9999999.99 元，最多两位小数。", L"价格格式", MB_OK | MB_ICONINFORMATION);
				SetFocus(GetDlgItem(hwnd, E_PRICE)); return 0;
			}
			selection = SendDlgItemMessageW(hwnd, E_SELLER, CB_GETCURSEL, 0, 0);
			if (selection == CB_ERR) {
				MessageBoxW(hwnd, L"请选择一个卖家。", L"商品信息", MB_OK | MB_ICONINFORMATION); return 0;
			}
			seller = (int)SendDlgItemMessageW(hwnd, E_SELLER, CB_GETITEMDATA, (WPARAM)selection, 0);
			/* 状态下拉索引：0 未售出、1 已售出、2 卖家下架、3 管理员下架，
				 映射为数据库的 sold 与 withdrawn 两个字段。 */
			selection = SendDlgItemMessageW(hwnd, E_STATUS, CB_GETCURSEL, 0, 0);
			sold = (selection == 1);
			withdrawn = selection == 2 ? 1 : selection == 3 ? 2 : 0;
			Product edited = e->value;
			edited.seller_id = seller;
			wcscpy_s(edited.name, _countof(edited.name), name);
			wcscpy_s(edited.description, _countof(edited.description), description);
			wcscpy_s(edited.image, _countof(edited.image), e->image);
			edited.price = cents; edited.sold = sold; edited.withdrawn = withdrawn;
			int saved = e->value.id ? store_edit_product(&db, &e->value, &edited)
				: store_admin_product(&db, 0, seller, name, description, cents, e->image, sold, withdrawn, &id);
			if (!saved) {
				ui_error(hwnd, &db); return 0;
			}
			ui_end_modal(hwnd, 1); return 0;
		}
		} break;
	case WM_SIZE:position_editor_actions(hwnd, 420, 536, 634); return 0;
	case WM_DRAWITEM: ui_draw_button((const DRAWITEMSTRUCT*)lp); return TRUE;
	case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: return dialog_colors(msg, wp);
	case WM_CLOSE: ui_end_modal(hwnd, 0); return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 返回列表中当前选中行的记录 id（id 存放在每行的 lParam 中）；未选中返回 0。 */
static int selected_id(void) {
	int row = ListView_GetNextItem(list_window, -1, LVNI_SELECTED);
	LVITEMW item; ZeroMemory(&item, sizeof(item));
	if (row < 0) return 0;
	item.mask = LVIF_PARAM; item.iItem = row;
	if (!ListView_GetItem(list_window, &item)) return 0;
	return (int)item.lParam;
}

/* 向列表视图插入一个列。 */
static void column(int index, const wchar_t* name, int width) {
	LVCOLUMNW c; ZeroMemory(&c, sizeof(c));
	c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
	c.pszText = (LPWSTR)name; c.cx = width; c.iSubItem = index;
	ListView_InsertColumn(list_window, index, &c);
}

/* 设置某一行某一子列（列号 sub>0）显示的文本。 */
static void item_text(int row, int sub, const wchar_t* text) {
	LVITEMW item; ZeroMemory(&item, sizeof(item));
	item.iSubItem = sub; item.pszText = (LPWSTR)text;
	SendMessageW(list_window, LVM_SETITEMTEXTW, row, (LPARAM)&item);
}

/* 按当前页签配置列表列：用户页 4 列、商品页 5 列；已配置过则仅调整列宽。 */
static void list_columns(void) {
	RECT client; int width, expected = current_tab ? 5 : 4;
	GetClientRect(list_window, &client); width = client.right - 28;
	if (Header_GetItemCount(ListView_GetHeader(list_window)) == expected) {
		if (!current_tab) {
			ListView_SetColumnWidth(list_window, 0, 70);
			ListView_SetColumnWidth(list_window, 1, width / 4);
			ListView_SetColumnWidth(list_window, 2, width / 3);
			ListView_SetColumnWidth(list_window, 3, width - 70 - width / 4 - width / 3);
		}
		else {
			ListView_SetColumnWidth(list_window, 0, 60);
			ListView_SetColumnWidth(list_window, 1, width - 550);
			ListView_SetColumnWidth(list_window, 2, 170);
			ListView_SetColumnWidth(list_window, 3, 130);
			ListView_SetColumnWidth(list_window, 4, 190);
		}
		return;
	}
	while (ListView_DeleteColumn(list_window, 0)) {}
	if (!current_tab) {
		column(0, L"ID", 70);
		column(1, L"登录账号", width / 4);
		column(2, L"昵称", width / 3);
		column(3, L"钱包余额（元）", width - 70 - width / 4 - width / 3);
	}
	else {
		column(0, L"ID", 60);
		column(1, L"商品名称", width - 550);
		column(2, L"卖家", 170);
		column(3, L"价格（元）", 130);
		column(4, L"状态", 190);
	}
}

/* 从数据库重新读取当前页签数据并重建列表行；尽量保持原选中行，最后更新底部统计。切换页签、增删改、窗口重新激活时都会调用，以反映另一程序可能做出的改动。 */
static void refresh_data(void) {
	int i, selection = selected_id(); wchar_t status[160];
	if (!current_tab) {
		User* fresh = NULL; int count = 0;
		if (!store_users(&db, &fresh, &count)) { ui_error(main_window, &db); return; }
		store_free(users); users = fresh; user_count = count;
	}
	else {
		Product* fresh = NULL; int count = 0;
		if (!store_products(&db, 0, 0, 0, &fresh, &count)) { ui_error(main_window, &db); return; }
		store_free(products); products = fresh; product_count = count;
	}
	SendMessageW(list_window, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(list_window);
	for (i = 0; i < (current_tab ? product_count : user_count); i++) {
		wchar_t id[24], money[64]; LVITEMW item; int ident;
		ZeroMemory(&item, sizeof(item));
		ident = current_tab ? products[i].id : users[i].id;
		swprintf_s(id, 24, L"%d", ident);
		item.mask = LVIF_TEXT | LVIF_PARAM; item.iItem = i; item.pszText = id; item.lParam = ident;
		ListView_InsertItem(list_window, &item);
		if (!current_tab) {
			amount_text(users[i].balance, money, 64);
			item_text(i, 1, users[i].account); item_text(i, 2, users[i].nickname); item_text(i, 3, money);
		}
		else {
			amount_text(products[i].price, money, 64);
			item_text(i, 1, products[i].name); item_text(i, 2, products[i].seller_name);
			item_text(i, 3, money);
			item_text(i, 4, products[i].sold ? L"已售出" : products[i].withdrawn == 1 ? L"商品已下架" : products[i].withdrawn == 2 ? L"商品已被管理员下架" : L"未售出");
		}
		if (ident == selection) ListView_SetItemState(list_window, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
	}
	SendMessageW(list_window, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(list_window, NULL, TRUE);
	swprintf_s(status, 160, L"共 %d %s", current_tab ? product_count : user_count, current_tab ? L"件商品" : L"个用户");
	SetWindowTextW(footer_window, status);
}

/* 切换用户/商品页签：更新按钮文字、重建列并刷新数据。 */
static void set_tab(int tab) {
	current_tab = tab;
	SetWindowTextW(GetDlgItem(main_window, ID_ADD), tab ? L"新增商品" : L"新增用户");
	SetWindowTextW(GetDlgItem(main_window, ID_EDIT), tab ? L"编辑商品" : L"编辑用户");
	SetWindowTextW(GetDlgItem(main_window, ID_DELETE), tab ? L"下架商品" : L"删除用户");
	ListView_DeleteAllItems(list_window);
	list_columns(); refresh_data(); InvalidateRect(main_window, NULL, FALSE);
}

/* 打开新增（create=1）或编辑（create=0）对话框。编辑时取选中行 id；按当前页签创建用户或商品窗口并以模态运行，关闭后刷新列表。 */
static void edit_entry(int create) {
	int id = create ? 0 : selected_id(); HWND dialog;
	if (!create && !id) {
		MessageBoxW(main_window, L"请先在表格中选择要编辑的一行。", L"选择记录", MB_OK | MB_ICONINFORMATION); return;
	}
	if (!current_tab) {
		UserEditor e; ZeroMemory(&e, sizeof(e));
		if (id && !store_user(&db, id, &e.value)) { ui_error(main_window, &db); refresh_data(); return; }
		/* 新增用户没有预置头像；界面使用昵称首字占位。 */
		if (id) wcscpy_s(e.avatar, 512, e.value.avatar);
		dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, USER_CLASS, id ? L"编辑用户" : L"新增用户",
			WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 0, 0, 700, 720, main_window, NULL, ui_instance, &e);
		if (dialog) { ui_center(dialog, 700, 720); ui_modal(dialog, main_window); }
	}
	else {
		ProductEditor e; ZeroMemory(&e, sizeof(e));
		if (id && !store_product(&db, id, &e.value)) { ui_error(main_window, &db); refresh_data(); return; }
		if (!store_users(&db, &e.users, &e.count)) { ui_error(main_window, &db); return; }
		if (!e.count) {
			store_free(e.users);
			MessageBoxW(main_window, L"请先新增一个用户，再为该用户添加商品。", L"尚无卖家", MB_OK | MB_ICONINFORMATION); return;
		}
		wcscpy_s(e.image, 512, e.value.image);
		dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, PRODUCT_CLASS, id ? L"编辑商品" : L"新增商品",
			WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 0, 0, 720, 760, main_window, NULL, ui_instance, &e);
		if (dialog) { ui_center(dialog, 720, 760); ui_modal(dialog, main_window); }
		store_free(e.users);
	}
	refresh_data();
}

/* 删除/下架当前选中记录：先弹确认框（商品为管理员下架、用户为删除），确认后调用对应数据层接口并刷新；已成交商品会被数据层拒绝。 */
static void delete_entry(void) {
	int id = selected_id(), i; wchar_t prompt[640], name[81] = L"";
	if (!id) {
		MessageBoxW(main_window, L"请先在表格中选择要删除的一行。", L"选择记录", MB_OK | MB_ICONINFORMATION); return;
	}
	if (current_tab) {
		for (i = 0; i < product_count; i++) if (products[i].id == id) wcscpy_s(name, 81, products[i].name);
		swprintf_s(prompt, 640, L"确定下架商品“%s”？\n\n商品将从交易页隐藏，卖家的已上架列表会保留该商品，并显示“商品已被管理员下架”。\n\n已完成购买的商品不可下架或删除。", name);
	}
	else {
		for (i = 0; i < user_count; i++) if (users[i].id == id) wcscpy_s(name, 81, users[i].account);
		swprintf_s(prompt, 640, L"确定删除用户“%s”？\n\n该用户的账号、资料、钱包和上架商品会一并删除。其他用户已有的购买记录保留成交时的信息。\n\n删除后无法恢复。", name);
	}
	if (MessageBoxW(main_window, prompt, current_tab ? L"确认下架" : L"确认删除", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) return;
	if (!(current_tab ? store_delete_product(&db, id) : store_delete_user(&db, id))) ui_error(main_window, &db);
	refresh_data();
}

/* 主窗口过程：创建顶部按钮和列表视图，处理页签/增删改/刷新命令、双击行编辑、窗口重新激活时自动刷新，并负责标题区的自绘。 */
static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_CREATE: {
		main_window = hwnd;
		ui_button(hwnd, ID_USERS, L"用户管理", 32, 127, 136, 43, 0);
		ui_button(hwnd, ID_PRODUCTS, L"商品管理", 180, 127, 136, 43, 0);
		ui_button(hwnd, ID_ADD, L"新增用户", 32, 192, 128, 40, 1);
		ui_button(hwnd, ID_EDIT, L"编辑用户", 172, 192, 128, 40, 0);
		ui_button(hwnd, ID_DELETE, L"删除用户", 312, 192, 128, 40, 0);
		ui_button(hwnd, ID_REFRESH, L"刷新数据", 452, 192, 128, 40, 0);
		list_window = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
			32, 251, 960, 380, hwnd, (HMENU)(INT_PTR)ID_LIST, ui_instance, NULL);
		control_font(list_window);
		SetWindowTheme(list_window, L"Explorer", NULL);
		ListView_SetExtendedListViewStyle(list_window, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
		ListView_SetBkColor(list_window, C_WHITE); ListView_SetTextColor(list_window, C_INK); ListView_SetTextBkColor(list_window, C_WHITE);
		footer_window = ui_label(hwnd, L"", 32, 647, 950, 28);
		set_tab(0);
		return 0;
	}
	case WM_SIZE: {
		int width = LOWORD(lp), height = HIWORD(lp);
		if (list_window) {
			MoveWindow(list_window, 32, 251, width - 64, height - 311, TRUE);
			MoveWindow(footer_window, 32, height - 45, width - 64, 28, TRUE);
			list_columns();
		}
		InvalidateRect(hwnd, NULL, FALSE); return 0;
	}
	case WM_GETMINMAXINFO:
		((MINMAXINFO*)lp)->ptMinTrackSize.x = 940;
		((MINMAXINFO*)lp)->ptMinTrackSize.y = 690;
		return 0;
	case WM_PAINT: {
		PAINTSTRUCT ps; RECT r; HDC dc = BeginPaint(hwnd, &ps); GetClientRect(hwnd, &r);
		ui_fill(dc, r, C_BG);
		ui_fill(dc, box(0, 0, r.right, 105), C_WHITE);
		ui_round(dc, box(32, 27, 76, 72), C_ACCENT, 12);
		ui_text(dc, L"拾", box(32, 27, 76, 72), ui_heading, C_WHITE, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
		ui_text(dc, L"拾物 / 本地数据管理", box(91, 22, 610, 61), ui_title, C_INK, DT_SINGLELINE | DT_VCENTER);
		ui_text(dc, L"用户与商品的增删改查", box(93, 64, 610, 89), ui_small, C_MUTED, DT_SINGLELINE | DT_VCENTER);
		ui_fill(dc, box(current_tab ? 180 : 32, 174, current_tab ? 316 : 168, 177), C_ACCENT);
		EndPaint(hwnd, &ps); return 0;
	}
							 /* 窗口从后台回到前台时重新拉取数据，以便看到前端程序刚做出的改动。 */
	case WM_ACTIVATE:
		if (LOWORD(wp) != WA_INACTIVE && list_window && IsWindowEnabled(hwnd)) refresh_data();
		return 0;
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case ID_USERS: set_tab(0); return 0;
		case ID_PRODUCTS: set_tab(1); return 0;
		case ID_ADD: edit_entry(1); return 0;
		case ID_EDIT: edit_entry(0); return 0;
		case ID_DELETE: delete_entry(); return 0;
		case ID_REFRESH: refresh_data(); return 0;
		}
		break;
		/* 双击列表某行直接打开编辑对话框。 */
	case WM_NOTIFY:
		if (((NMHDR*)lp)->idFrom == ID_LIST && ((NMHDR*)lp)->code == NM_DBLCLK) { edit_entry(0); return 0; }
		break;
	case WM_DRAWITEM: ui_draw_button((const DRAWITEMSTRUCT*)lp); return TRUE;
	case WM_CTLCOLORSTATIC:
		SetTextColor((HDC)wp, C_MUTED); SetBkColor((HDC)wp, C_BG); return (LRESULT)brush_bg;
	case WM_DESTROY: PostQuitMessage(0); return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 后台程序入口：初始化界面与通用控件、打开数据库（首次运行仅建立空表）、注册三个窗口类、创建主窗口后进入消息循环；退出时统一释放资源。 */
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
	WNDCLASSEXW wc; HWND hwnd; MSG msg; wchar_t database[MAX_PATH];
	INITCOMMONCONTROLSEX common = { sizeof(common),ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
	(void)previous; (void)command;
	ui_init(instance); InitCommonControlsEx(&common);
	brush_white = CreateSolidBrush(C_WHITE); brush_bg = CreateSolidBrush(C_BG);
	ui_database_path(database, MAX_PATH);
	/* 打开数据库；首次运行仅建立空表，不写入账号、商品或成交记录。 */
	if (!store_open(&db, database)) {
		ui_error(NULL, &db); store_close(&db); ui_cleanup(); return 1;
	}
	ZeroMemory(&wc, sizeof(wc)); wc.cbSize = sizeof(wc); wc.hInstance = instance;
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hbrBackground = brush_bg;
	wc.lpfnWndProc = MainProc; wc.lpszClassName = MAIN_CLASS;
	wc.hIcon = load_icon(L"assets/backend.ico"); wc.hIconSm = wc.hIcon;
	RegisterClassExW(&wc);
	wc.hbrBackground = brush_white; wc.lpfnWndProc = UserProc; wc.lpszClassName = USER_CLASS;
	RegisterClassExW(&wc);
	wc.lpfnWndProc = ProductProc; wc.lpszClassName = PRODUCT_CLASS;
	RegisterClassExW(&wc);
	hwnd = CreateWindowExW(0, MAIN_CLASS, L"拾物 · 本地数据管理", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		CW_USEDEFAULT, CW_USEDEFAULT, 1040, 760, NULL, NULL, instance, NULL);
	if (!hwnd) { store_close(&db); ui_cleanup(); return 1; }
	ui_center(hwnd, 1040, 760); ShowWindow(hwnd, show); UpdateWindow(hwnd);
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
	}
	store_free(users); store_free(products); store_close(&db);
	DeleteObject(brush_white); DeleteObject(brush_bg); ui_cleanup();
	return (int)msg.wParam;
}
