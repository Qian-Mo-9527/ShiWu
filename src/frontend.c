#define _CRT_SECURE_NO_WARNINGS
#include "ui.h"
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/* 页面状态：登录、注册、交易、详情、上架、我的、资料、钱包、已购买、已上架。 */
enum { LOGIN, REGISTER, TRADE, DETAIL, PUBLISH, MY, PROFILE, WALLET, PURCHASED, LISTED };
/* 控件 ID：ACT_* 为按钮动作（10 段），NAV_* 为左侧导航（60 段），
	 ED_* 为输入框（100 段），CARD_BASE 起为商品卡片按钮（其 id 减去 CARD_BASE 即卡片下标）。 */
enum {
	ACT_LOGIN = 10, ACT_REGISTER, ACT_SWITCH, ACT_BUY, ACT_POST, ACT_IMAGE, ACT_SAVE_PROFILE,
	ACT_AVATAR, ACT_RECHARGE, ACT_LOGOUT, ACT_WALLET, ACT_PURCHASED, ACT_LISTED, ACT_PROFILE, ACT_BACK, ACT_REFRESH, ACT_WITHDRAW,
	ACT_EYE,
	NAV_TRADE = 60, NAV_POST, NAV_MY, ED_ACCOUNT = 100, ED_PASSWORD, ED_NAME, ED_BIO, ED_PRICE, ED_NEW_PASSWORD,
	CARD_BASE = 1000
};
static Store db;
static User me;                                  /* 当前登录用户；me.id 为 0 表示未登录 */
static HWND main_window, content;                /* 主窗口与承载卡片列表的滚动子窗口 */
static int page = LOGIN, selected_product = 0, detail_origin = TRADE, scroll_y = 0, content_height = 0, busy = 0;
static int wheel_remainder = 0;                 /* 累计触控板/高分辨率滚轮不足一格的滚动量 */
/* page 当前页；selected_product 详情页商品 id；detail_origin 详情页返回去向；scroll_y/content_height 卡片滚动位置与总高；busy 防止重入刷新（如命令处理中）。 */
static Product* products = NULL;
static Order* orders = NULL;
static int product_count = 0, order_count = 0;
static wchar_t chosen_image[512], chosen_avatar[512], last_account[33];  /* 待提交图片/头像、上次输入的账号 */
static int password_visible = 0;                  /* 登录/注册密码框是否以明文显示（小眼睛开关） */
static HBRUSH white_brush, bg_brush;
static const int W = 1120, H = 820, LEFT = 220;        /* 主窗口宽高与左侧导航栏宽度 */

static void show_page(int next);
static void command(int id);
/* 以下 rect/text_at/label/button/field 都是针对主窗口的便捷封装，减少重复参数。 */
static RECT rect(int x, int y, int w, int h) { RECT r = { x,y,x + w,y + h }; return r; }
static void text_at(HDC dc, const wchar_t* s, int x, int y, int w, int h, HFONT f, COLORREF c, UINT flags) { ui_text(dc, s, rect(x, y, w, h), f, c, flags); }
static void label(const wchar_t* s, int x, int y, int w, int h) { ui_label(main_window, s, x, y, w, h); }
static void button(int id, const wchar_t* s, int x, int y, int w, int h, int primary) { ui_button(main_window, id, s, x, y, w, h, primary); }
static void field(int id, const wchar_t* s, int x, int y, int w, int h, DWORD flags, int limit) { ui_edit(main_window, id, s, x, y, w, h, flags, limit); }
/* 工作区较矮时让退出按钮仍保留底部 24 像素间距，侧栏用户信息跟随上移。 */
static int logout_top(void) {
	RECT client; GetClientRect(main_window, &client);
	int top = client.bottom - 42 - 24;
	if (top > 690)top = 690;
	if (top < 0)top = 0;
	return top;
}
/* 释放并清空商品、订单的内存缓存（切页/退出时调用）。 */
static void clear_lists(void) { store_free(products); products = NULL; product_count = 0; store_free(orders); orders = NULL; order_count = 0; }
/* 把整数分格式化为“¥ 元.分”的展示文本。 */
static void balance_text(wchar_t* out, size_t capacity, Money cents) { wchar_t n[40]; money_format(cents, n, 40); swprintf_s(out, capacity, L"¥ %s", n); }
/* 重新拉取当前用户最新资料；若该账号已被后台删除，则置为未登录并提示。 */
static void current_user(void) {
	if (me.id && !store_user(&db, me.id, &me)) {
		me.id = 0; MessageBoxW(main_window, L"此账号已在本地数据管理中删除，请重新登录。", L"账号已删除", MB_OK | MB_ICONINFORMATION);
	}
}
/* 关键操作（购买、上架、充值等）前确认仍处于登录态；未登录则跳回登录页并返回 0。 */
static int refresh_user(void) { current_user(); if (!me.id) { show_page(LOGIN); return 0; }return 1; }
/* 各页面顶部显示的标题。 */
static const wchar_t* page_title(void) {
	switch (page) {
	case TRADE:return L"发现好物"; case DETAIL:return L"商品详情"; case PUBLISH:return L"上架物品"; case MY:return L"我的";
	case PROFILE:return L"编辑个人信息"; case WALLET:return L"我的钱包"; case PURCHASED:return L"已购买"; case LISTED:return L"已上架";
	default:return L"";
	}
}

/* 自绘一张商品/订单卡片：按钮 id 减去 CARD_BASE 得到下标。
	 “已购买”页用订单快照，其余页用商品数据，两者共用同一套卡片布局。 */
static void paint_card(const DRAWITEMSTRUCT* d) {
	int index = (int)d->CtlID - CARD_BASE;
	int hist = page == PURCHASED;
	if (index < 0 || index >= (hist ? order_count : product_count))return;
	int width = d->rcItem.right, height = d->rcItem.bottom;
	HDC dc = d->hDC; RECT r = d->rcItem;
	ui_fill(dc, r, C_BG); ui_round(dc, r, C_WHITE, 18);
	const wchar_t* name = hist ? orders[index].product_name : products[index].name;
	const wchar_t* image = hist ? orders[index].image : products[index].image;
	Money price = hist ? orders[index].price : products[index].price;
	ui_image(dc, image, rect(12, 12, width - 24, 180), 1);
	text_at(dc, name, 18, 203, width - 36, 48, ui_font, C_INK, DT_WORDBREAK | DT_END_ELLIPSIS);
	wchar_t price_text[64]; balance_text(price_text, 64, price);
	text_at(dc, price_text, 18, 258, width - 36, 36, ui_price, C_RED, DT_SINGLELINE | DT_VCENTER);
	wchar_t note[120];
	if (hist)swprintf_s(note, 120, L"卖家 %s  ·  %s", orders[index].seller_name, orders[index].created_at);
	else if (page == LISTED)swprintf_s(note, 120, L"%s", products[index].sold ? L"已售出" : products[index].withdrawn == 1 ? L"商品已下架" : products[index].withdrawn == 2 ? L"商品已被管理员下架" : L"未售出");
	else swprintf_s(note, 120, L"来自 %s", products[index].seller_name);
	COLORREF note_color = C_MUTED;
	if (page == LISTED)note_color = products[index].sold ? C_ACCENT : products[index].withdrawn ? C_RED : C_MUTED;
	text_at(dc, note, 18, 306, width - 36, 25, page == LISTED ? ui_font : ui_small, note_color, DT_SINGLELINE | DT_END_ELLIPSIS);
	if (d->itemState & ODS_FOCUS) { RECT f = r; InflateRect(&f, -3, -3); DrawFocusRect(dc, &f); }
}
/* 双列卡片布局并配置滚动：按两列计算内容总高度、更新滚动条，再逐一定位卡片。 */
static void layout_cards(HWND host) {
	RECT r; GetClientRect(host, &r);
	int count = page == PURCHASED ? order_count : product_count;
	content_height = ((count + 1) / 2) * 354;
	SCROLLINFO si = { sizeof(si),SIF_RANGE | SIF_PAGE | SIF_POS,0,content_height > 0 ? content_height - 1 : 0,(UINT)r.bottom,scroll_y,0 };
	SetScrollInfo(host, SB_VERT, &si, TRUE);
	SCROLLINFO actual = { sizeof(actual),SIF_POS }; GetScrollInfo(host, SB_VERT, &actual); scroll_y = actual.nPos;
	/* 自动显示/隐藏滚动条会改变客户区宽度，须使用最终宽度计算两列间距。 */
	GetClientRect(host, &r); int width = (r.right - 20) / 2;
	for (int i = 0; i < count; i++)SetWindowPos(GetDlgItem(host, CARD_BASE + i), NULL, (i % 2) * (width + 20), (i / 2) * 354 - scroll_y, width, 338, SWP_NOZORDER);
	InvalidateRect(host, NULL, TRUE);
}
/* 卡片列表子窗口过程：把命令和自绘转发给主窗口，自行处理背景擦除、垂直滚动条与鼠标滚轮（滚轮步长 96 像素）。 */
static LRESULT CALLBACK content_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
	switch (msg) {
	case WM_COMMAND:return SendMessageW(main_window, msg, w, l);
	case WM_DRAWITEM:return SendMessageW(main_window, msg, w, l);
	case WM_ERASEBKGND: { RECT r; GetClientRect(hwnd, &r); ui_fill((HDC)w, r, C_BG); return 1; }
	case WM_VSCROLL: {
		RECT r; GetClientRect(hwnd, &r); SCROLLINFO si = { sizeof(si),SIF_ALL }; GetScrollInfo(hwnd, SB_VERT, &si);
		switch (LOWORD(w)) {
		case SB_LINEUP:scroll_y -= 45; break; case SB_LINEDOWN:scroll_y += 45; break;
		case SB_PAGEUP:scroll_y -= r.bottom; break; case SB_PAGEDOWN:scroll_y += r.bottom; break;
		case SB_THUMBTRACK:case SB_THUMBPOSITION:scroll_y = si.nTrackPos; break; case SB_TOP:scroll_y = 0; break; case SB_BOTTOM:scroll_y = content_height; break;
		}
		if (scroll_y < 0)scroll_y = 0; layout_cards(hwnd); return 0;
	}
	case WM_MOUSEWHEEL: {
		wheel_remainder += GET_WHEEL_DELTA_WPARAM(w);
		int steps = wheel_remainder / WHEEL_DELTA;
		wheel_remainder %= WHEEL_DELTA;
		if (steps) { scroll_y -= steps * 96; if (scroll_y < 0)scroll_y = 0; layout_cards(hwnd); }
		return 0;
	}
	}
	return DefWindowProcW(hwnd, msg, w, l);
}
/* 创建列表滚动子窗口，并为每件商品/订单创建一个自绘卡片按钮（点击即进入详情）。 */
static void make_cards(void) {
	int count = page == PURCHASED ? order_count : product_count;
	if (!count)return;
	RECT client; GetClientRect(main_window, &client);
	int height = client.bottom - 148 - 24; if (height < 1)height = 1;
	content = CreateWindowExW(0, L"MarketContent", L"商品列表", WS_CHILD | WS_VISIBLE | WS_VSCROLL, LEFT, 148, 848, height, main_window, NULL, ui_instance, NULL);
	for (int i = 0; i < count; i++) {
		HWND b = ui_button(content, CARD_BASE + i, page == PURCHASED ? orders[i].product_name : products[i].name, 0, 0, 100, 100, 0);
		if (page == PURCHASED)SetWindowLongPtrW(b, GWL_STYLE, GetWindowLongPtrW(b, GWL_STYLE) & ~WS_TABSTOP);
	}
	layout_cards(content);
}
/* 创建左侧导航（交易/上架/我的、退出）与右上角的返回或刷新按钮。 */
static void make_nav(void) {
	button(NAV_TRADE, L"交易", 24, 150, 152, 48, page == TRADE || page == DETAIL ? 2 : 0);
	button(NAV_POST, L"上架", 24, 210, 152, 48, page == PUBLISH ? 2 : 0);
	button(NAV_MY, L"我的", 24, 270, 152, 48, page >= MY ? 2 : 0);
	button(ACT_LOGOUT, L"退出登录", 24, logout_top(), 152, 42, 0);
	if (page != TRADE && page != PUBLISH && page != MY)button(ACT_BACK, L"返回", LEFT, 88, 76, 38, 0);
	else button(ACT_REFRESH, L"刷新", 992, 87, 76, 38, 0);
}
/* 页面状态机核心：清空旧控件与缓存、切到新页并重建该页的控件与数据。登录/注册页只放账号密码表单；其余页先建导航，再按页面加载对应数据并布局。 */
static void show_page(int next) {
	busy++; ui_clear(main_window); content = NULL; clear_lists(); page = next; scroll_y = 0; wheel_remainder = 0;
	if (page >= TRADE) {
		current_user(); if (!me.id)page = LOGIN;
	}
	if (page == LOGIN || page == REGISTER) {
		password_visible = 0;   /* 每次进入登录/注册页，密码默认隐藏（掩码显示为 *） */
		label(L"账号", 650, 260, 330, 25); field(ED_ACCOUNT, last_account, 650, 292, 330, 42, 0, 32);
		label(L"密码", 650, 352, 330, 25); field(ED_PASSWORD, L"", 650, 384, 330, 42, ES_PASSWORD, 64);
		/* 给密码框留出右侧内边距，避免文字被小眼睛遮住；小眼睛叠加在密码框右内侧。 */
		HWND password_edit = GetDlgItem(main_window, ED_PASSWORD);
		SetWindowLongPtrW(password_edit, GWL_STYLE, GetWindowLongPtrW(password_edit, GWL_STYLE) | WS_CLIPSIBLINGS);
		SendMessageW(password_edit, EM_SETMARGINS, EC_RIGHTMARGIN, MAKELPARAM(9, 42));
		/* 隐藏状态下密码字符统一显示为 *。 */
		SendMessageW(password_edit, EM_SETPASSWORDCHAR, (WPARAM)L'*', 0);
		password_visible = SendMessageW(password_edit, EM_GETPASSWORDCHAR, 0, 0) == 0;
		button(ACT_EYE, password_visible ? L"隐藏密码" : L"显示密码", 942, 386, 36, 38, 0);
		/* 明确把眼睛放在密码框上方；密码框绘制时裁剪同级控件，避免覆盖图标。 */
		HWND eye_button = GetDlgItem(main_window, ACT_EYE);
		SetWindowLongPtrW(eye_button, GWL_STYLE, GetWindowLongPtrW(eye_button, GWL_STYLE) | WS_CLIPSIBLINGS);
		SetWindowPos(eye_button, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		button(page == LOGIN ? ACT_LOGIN : ACT_REGISTER, page == LOGIN ? L"登 录" : L"注 册", 650, 454, 330, 46, 1);
		button(ACT_SWITCH, page == LOGIN ? L"没有账号？去注册" : L"已有账号？去登录", 650, 514, 330, 42, 0);
		SetFocus(GetDlgItem(main_window, ED_ACCOUNT));
	}
	else {
		make_nav();
		switch (page) {
		case TRADE:
			if (!store_products(&db, me.id, 0, 1, &products, &product_count))ui_error(main_window, &db);
			make_cards(); break;
		case DETAIL: {
			Product p; if (!store_product(&db, selected_product, &p)) { ui_error(main_window, &db); busy--; show_page(TRADE); return; }
			int action = p.seller_id == me.id ? ACT_WITHDRAW : ACT_BUY;
			button(action, p.sold ? L"商品已售出" : p.withdrawn == 1 ? L"商品已下架" : p.withdrawn == 2 ? L"商品已被管理员下架" : p.seller_id == me.id ? L"下架这件商品" : L"确认购买", 680, 467, 345, 48, 1);
			if (p.sold || p.withdrawn)EnableWindow(GetDlgItem(main_window, action), FALSE);
			break;
		}
		case PUBLISH:
			chosen_image[0] = 0;
			button(ACT_IMAGE, L"选择商品图片", 244, 427, 282, 43, 0);
			label(L"商品名称 *", 572, 172, 440, 26); field(ED_NAME, L"", 572, 205, 450, 42, 0, 80);
			label(L"商品介绍 *", 572, 269, 440, 26); field(ED_BIO, L"", 572, 302, 450, 160, ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN, 1000);
			label(L"售价（元）*", 572, 488, 440, 26); ui_money_edit(main_window, ED_PRICE, L"", 572, 521, 450, 42, 16);
			button(ACT_POST, L"确认上架", 572, 595, 450, 48, 1); break;
		case MY:
			button(ACT_PROFILE, L"编辑个人信息", 842, 202, 186, 42, 0);
			button(ACT_WALLET, L"查看钱包 / 充值", 842, 375, 186, 43, 1);
			button(ACT_PURCHASED, L"已购买", 244, 509, 375, 86, 0);
			button(ACT_LISTED, L"已上架", 644, 509, 384, 86, 0); break;
		case PROFILE:
			wcsncpy_s(chosen_avatar, 512, me.avatar, _TRUNCATE);
			button(ACT_AVATAR, L"更换头像", 260, 337, 180, 42, 0);
			label(L"昵称", 500, 170, 480, 25); field(ED_NAME, me.nickname, 500, 202, 500, 42, 0, 32);
			label(L"个人简介", 500, 265, 480, 25); field(ED_BIO, me.bio, 500, 297, 500, 124, ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN, 256);
			label(L"新密码（留空则保持原密码）", 500, 445, 500, 25); field(ED_NEW_PASSWORD, L"", 500, 477, 500, 42, ES_PASSWORD, 64);
			button(ACT_SAVE_PROFILE, L"保存修改", 500, 551, 500, 48, 1); break;
		case WALLET:
			label(L"充值金额（元）", 260, 408, 580, 26); ui_money_edit(main_window, ED_PRICE, L"", 260, 446, 496, 46, 16);
			button(ACT_RECHARGE, L"确认充值", 778, 446, 220, 46, 1); break;
		case PURCHASED:
			if (!store_orders(&db, me.id, &orders, &order_count))ui_error(main_window, &db); make_cards(); break;
		case LISTED:
			if (!store_products(&db, 0, me.id, 0, &products, &product_count))ui_error(main_window, &db); make_cards(); break;
		}
	}
	InvalidateRect(main_window, NULL, TRUE); busy--;
}
/* 绘制登录/注册页：左侧品牌展示区（插画与标语），右侧表单控件由 show_page 创建。 */
static void paint_login(HDC dc) {
	ui_fill(dc, rect(0, 0, 1120, 820), C_WHITE);
	ui_fill(dc, rect(0, 0, 540, 820), RGB(231, 239, 230));
	text_at(dc, L"拾物", 55, 46, 400, 55, ui_title, C_ACCENT, DT_SINGLELINE);
	text_at(dc, L"SECOND HAND, NEW STORIES", 57, 111, 430, 28, ui_small, C_ACCENT, DT_SINGLELINE);
	text_at(dc, L"让闲置，\n遇见下一位主人。", 55, 177, 455, 112, ui_title, C_INK, DT_WORDBREAK);
	ui_round(dc, rect(54, 340, 300, 237), C_WHITE, 22); ui_image(dc, L"assets\\lamp.png", rect(64, 350, 280, 217), 0);
	ui_round(dc, rect(268, 465, 219, 172), C_WHITE, 22); ui_image(dc, L"assets\\camera.png", rect(278, 475, 199, 152), 0);
	text_at(dc, L"每件好物，都有新的可能。", 56, 696, 435, 30, ui_font, C_ACCENT, DT_SINGLELINE);
	text_at(dc, page == LOGIN ? L"欢迎回来" : L"创建账号", 650, 143, 350, 55, ui_title, C_INK, DT_SINGLELINE);
	text_at(dc, page == LOGIN ? L"登录，发现身边的二手好物。" : L"只需账号和密码，即可开始。", 650, 208, 370, 30, ui_font, C_MUTED, DT_SINGLELINE);
}
/* 绘制登录后的主界面：左侧导航与用户信息，再按当前页绘制背景与静态内容；各类输入控件由 show_page 创建，这里只负责卡片以外的自绘区域。 */
static void paint_main(HDC dc) {
	RECT client; GetClientRect(main_window, &client); ui_fill(dc, client, C_BG);
	ui_fill(dc, rect(0, 0, 200, client.bottom), C_WHITE);
	text_at(dc, L"拾物", 29, 32, 150, 47, ui_title, C_ACCENT, DT_SINGLELINE);
	text_at(dc, L"二手好物交易", 30, 84, 145, 25, ui_small, C_MUTED, DT_SINGLELINE);
	int sidebar_shift = logout_top() - 690;
	ui_avatar(dc, me.avatar, rect(28, 604 + sidebar_shift, 40, 40), me.nickname);
	text_at(dc, me.nickname, 80, 607 + sidebar_shift, 101, 25, ui_font, C_INK, DT_SINGLELINE | DT_END_ELLIPSIS);
	int title_x = (page != TRADE && page != PUBLISH && page != MY) ? 310 : LEFT;
	text_at(dc, page_title(), title_x, 84, 610, 47, ui_heading, C_INK, DT_SINGLELINE | DT_VCENTER);
	switch (page) {
	case TRADE:case LISTED:case PURCHASED: {
		int count = page == PURCHASED ? order_count : product_count;
		if (!count) {
			ui_round(dc, rect(LEFT, 148, 848, 430), C_WHITE, 20);
			text_at(dc, page == TRADE ? L"暂时没有可购买的商品" : page == LISTED ? L"你还没有上架商品" : L"你还没有购买记录", 240, 310, 800, 44, ui_heading, C_MUTED, DT_SINGLELINE | DT_CENTER);
			text_at(dc, page == TRADE ? L"交易页只显示其他用户未售出的商品。" : page == LISTED ? L"在左侧“上架”发布第一件闲置物品。" : L"去交易页看看有没有心仪的好物吧。", 240, 363, 800, 28, ui_font, C_MUTED, DT_SINGLELINE | DT_CENTER);
		}break;
	}
	case DETAIL: {
		Product p; if (!store_product(&db, selected_product, &p))break;
		ui_round(dc, rect(LEFT, 148, 848, 572), C_WHITE, 20);
		ui_image(dc, p.image, rect(242, 170, 394, 330), 0);
		text_at(dc, p.name, 680, 179, 345, 85, ui_heading, C_INK, DT_WORDBREAK | DT_END_ELLIPSIS);
		wchar_t amount[64], wallet[80], bal[48]; balance_text(amount, 64, p.price); balance_text(bal, 48, me.balance);
		swprintf_s(wallet, 80, L"你的余额  %s", bal);
		text_at(dc, amount, 680, 286, 345, 50, ui_title, C_RED, DT_SINGLELINE);
		wchar_t seller[90]; swprintf_s(seller, 90, L"卖家：%s", p.seller_name);
		text_at(dc, seller, 680, 355, 345, 30, ui_font, C_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
		text_at(dc, p.sold ? L"状态：已售出" : p.withdrawn == 1 ? L"商品已下架" : p.withdrawn == 2 ? L"商品已被管理员下架" : L"状态：未售出", 680, 391, 345, 28, ui_font, p.sold ? C_RED : C_ACCENT, DT_SINGLELINE);
		text_at(dc, wallet, 680, 427, 345, 28, ui_small, C_MUTED, DT_SINGLELINE);
		text_at(dc, L"商品介绍", 244, 538, 790, 30, ui_heading, C_INK, DT_SINGLELINE);
		// 下方使用原生只读编辑框，提供可选中、可滚动的长文本介绍。
		break;
	}
	case PUBLISH:
		ui_round(dc, rect(LEFT, 148, 848, 558), C_WHITE, 20);
		ui_image(dc, chosen_image, rect(244, 174, 282, 228), 0);
		if (!*chosen_image)ui_required_text(dc, L"添加一张商品图片 *", rect(244, 265, 282, 40), ui_font, C_MUTED, DT_SINGLELINE | DT_CENTER);
		text_at(dc, L"支持 JPG、PNG", 244, 485, 282, 25, ui_small, C_MUTED, DT_CENTER | DT_SINGLELINE);
		break;
	case MY: {
		ui_round(dc, rect(LEFT, 148, 848, 155), C_WHITE, 20);
		ui_avatar(dc, me.avatar, rect(244, 179, 88, 88), me.nickname);
		text_at(dc, me.nickname, 355, 176, 460, 38, ui_heading, C_INK, DT_SINGLELINE | DT_END_ELLIPSIS);
		wchar_t account[70]; swprintf_s(account, 70, L"账号：%s", me.account);
		text_at(dc, account, 355, 216, 460, 25, ui_small, C_MUTED, DT_SINGLELINE);
		text_at(dc, *me.bio ? me.bio : L"还没有填写个人简介", 355, 249, 460, 28, ui_font, C_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
		ui_round(dc, rect(LEFT, 325, 848, 150), RGB(224, 238, 230), 20);
		text_at(dc, L"钱包余额", 248, 347, 500, 28, ui_font, C_ACCENT, DT_SINGLELINE);
		wchar_t balance[64]; balance_text(balance, 64, me.balance); text_at(dc, balance, 248, 391, 560, 52, ui_title, C_ACCENT, DT_SINGLELINE);
		break;
	}
	case PROFILE:
		ui_round(dc, rect(LEFT, 148, 848, 535), C_WHITE, 20);
		ui_avatar(dc, chosen_avatar, rect(278, 180, 144, 144), me.nickname);
		text_at(dc, me.account, 245, 403, 220, 30, ui_font, C_INK, DT_SINGLELINE | DT_CENTER | DT_END_ELLIPSIS); break;
	case WALLET: {
		ui_round(dc, rect(LEFT, 148, 848, 220), RGB(224, 238, 230), 20);
		text_at(dc, L"当前余额", 260, 185, 700, 28, ui_font, C_ACCENT, DT_SINGLELINE);
		wchar_t balance[64]; balance_text(balance, 64, me.balance); text_at(dc, balance, 260, 237, 720, 68, ui_title, C_ACCENT, DT_SINGLELINE);
		text_at(dc, L"充值为本地演示操作，确认后金额直接加入余额。", 260, 552, 760, 28, ui_small, C_MUTED, DT_SINGLELINE); break;
	}
	}
}
/* 在详情页额外创建只读、可滚动、可选中的多行编辑框来显示长商品介绍。 */
static void add_detail_description(void) {
	if (page != DETAIL)return; Product p; if (!store_product(&db, selected_product, &p))return;
	HWND e = ui_edit(main_window, 199, p.description, 244, 580, 788, 112, ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 1000);
	SetWindowLongPtrW(e, GWL_EXSTYLE, GetWindowLongPtrW(e, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
	SetWindowPos(e, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}
/* 切换页面；进入详情页时再补建只读介绍框（需在 show_page 之后）。 */
static void navigate(int next) { show_page(next); if (next == DETAIL)add_detail_description(); }
/* 所有按钮动作的分发中枢：先处理卡片点击（进入详情）与回车（IDOK），再按 ACT_* 分别处理登录注册、导航、购买、上架、下架、资料、充值、退出等。 */
static void command(int id) {
	wchar_t account[33], password[65], name[81], bio[1001], amount[48]; Money money; int new_id;
	/* 点击卡片（非已购买页）：记录来源页与商品 id，进入详情。 */
	if (id >= CARD_BASE) { int idx = id - CARD_BASE; if (page != PURCHASED && idx < product_count && idx >= 0) { detail_origin = page; selected_product = products[idx].id; navigate(DETAIL); }return; }
	/* 登录/注册页按回车视为点击登录或注册按钮；焦点在眼睛上时只切换密码显示。 */
	if (id == IDOK) {
		if ((page == LOGIN || page == REGISTER) && GetFocus() == GetDlgItem(main_window, ACT_EYE))id = ACT_EYE;
		else if (page == LOGIN)id = ACT_LOGIN; else if (page == REGISTER)id = ACT_REGISTER; else return;
	}
	switch (id) {
	case ACT_SWITCH:ui_get(main_window, ED_ACCOUNT, last_account, 33); navigate(page == LOGIN ? REGISTER : LOGIN); break;
		/* 小眼睛：切换密码框掩码字符，0=显示真实字符，'*'=以星号隐藏，并刷新眼睛图标。 */
	case ACT_EYE: {
		HWND e = GetDlgItem(main_window, ED_PASSWORD);
		HWND eye = GetDlgItem(main_window, ACT_EYE);
		if ((page != LOGIN && page != REGISTER) || !IsWindow(e) || !IsWindow(eye))break;
		DWORD selection_start = 0, selection_end = 0;
		SendMessageW(e, EM_GETSEL, (WPARAM)&selection_start, (LPARAM)&selection_end);
		/* 以编辑框真实掩码状态为准，不依赖可能过时的布尔值。切换不重写密码文本。 */
		int was_visible = SendMessageW(e, EM_GETPASSWORDCHAR, 0, 0) == 0;
		SendMessageW(e, EM_SETPASSWORDCHAR, (WPARAM)(was_visible ? L'*' : 0), 0);
		password_visible = SendMessageW(e, EM_GETPASSWORDCHAR, 0, 0) == 0;
		SetWindowTextW(eye, password_visible ? L"隐藏密码" : L"显示密码");
		SetFocus(e);
		SendMessageW(e, EM_SETSEL, selection_start, selection_end);
		SendMessageW(e, EM_SCROLLCARET, 0, 0);
		RedrawWindow(e, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
		RedrawWindow(eye, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
		break;
	}
	case ACT_LOGIN:
		ui_get(main_window, ED_ACCOUNT, account, 33); ui_get(main_window, ED_PASSWORD, password, 65);
		if (store_login(&db, account, password, &me)) { wcsncpy_s(last_account, 33, account, _TRUNCATE); SecureZeroMemory(password, sizeof(password)); navigate(TRADE); }
		else { SecureZeroMemory(password, sizeof(password)); ui_error(main_window, &db); }break;
	case ACT_REGISTER:
		ui_get(main_window, ED_ACCOUNT, account, 33); ui_get(main_window, ED_PASSWORD, password, 65);
		if (store_register(&db, account, password, &new_id)) {
			wcsncpy_s(last_account, 33, account, _TRUNCATE); SecureZeroMemory(password, sizeof(password));
			MessageBoxW(main_window, L"注册成功，请使用新账号登录。", L"注册成功", MB_OK | MB_ICONINFORMATION); navigate(LOGIN);
		}
		else { SecureZeroMemory(password, sizeof(password)); ui_error(main_window, &db); }break;
	case NAV_TRADE:navigate(TRADE); break;
	case NAV_POST:navigate(PUBLISH); break;
	case NAV_MY:navigate(MY); break;
	case ACT_BACK:navigate(page == DETAIL ? detail_origin : MY); break;
	case ACT_REFRESH:navigate(page); break;
	case ACT_LOGOUT:SecureZeroMemory(&me, sizeof(me)); chosen_avatar[0] = 0; chosen_image[0] = 0; navigate(LOGIN); break;
	case ACT_IMAGE:if (ui_pick_image(main_window, chosen_image, 512))InvalidateRect(main_window, NULL, FALSE); break;
	case ACT_POST:
		if (!refresh_user())break;
		ui_get(main_window, ED_NAME, name, 81); ui_get(main_window, ED_BIO, bio, 1001); ui_get(main_window, ED_PRICE, amount, 48);
		if (!money_parse(amount, 0, &money)) { MessageBoxW(main_window, L"请输入有效价格，最多两位小数，范围 0.01～9999999.99 元。", L"价格有误", MB_OK | MB_ICONINFORMATION); break; }
		if (store_add_product(&db, me.id, name, bio, money, chosen_image, &new_id)) {
			MessageBoxW(main_window, L"上架成功，可在“我的 → 已上架”查看。", L"上架成功", MB_OK | MB_ICONINFORMATION); navigate(LISTED);
		}
		else ui_error(main_window, &db); break;
		/* 购买：先复核登录态、商品是否仍在售以及余额，再二次确认；调用 store_buy_at_price 并传入详情页看到的价格，由数据库防止期间改价。 */
	case ACT_BUY: {
		if (!refresh_user())break; Product p; if (!store_product(&db, selected_product, &p)) { ui_error(main_window, &db); navigate(TRADE); break; }
		if (p.sold) { MessageBoxW(main_window, L"这件商品已售出。", L"商品已售出", MB_OK | MB_ICONINFORMATION); navigate(TRADE); break; }
		if (p.withdrawn) { MessageBoxW(main_window, L"这件商品已下架。", L"商品已下架", MB_OK | MB_ICONINFORMATION); navigate(TRADE); break; }
		if (p.price > me.balance) { MessageBoxW(main_window, L"余额不足，请充值", L"余额不足", MB_OK | MB_ICONWARNING); break; }
		wchar_t price[48], question[240]; balance_text(price, 48, p.price); swprintf_s(question, 240, L"确认以 %s 购买“%s”？\n购买后款项将转入卖家钱包。", price, p.name);
		if (MessageBoxW(main_window, question, L"确认购买", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
			if (store_buy_at_price(&db, me.id, p.id, p.price)) { current_user(); MessageBoxW(main_window, L"购买成功！商品已保存到“我的 → 已购买”。", L"购买成功", MB_OK | MB_ICONINFORMATION); navigate(PURCHASED); }
			else { ui_error(main_window, &db); navigate(DETAIL); }
		}break;
	}
	case ACT_WITHDRAW:
		if (MessageBoxW(main_window, L"确认下架这件商品？\n交易页将移除该商品，你的已上架列表中保留记录并标注“商品已下架”。\n已完成购买的商品不可下架。", L"确认下架", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
			if (store_withdraw_product(&db, me.id, selected_product))navigate(LISTED);
			else ui_error(main_window, &db);
		}break;
	case ACT_PROFILE:navigate(PROFILE); break;
	case ACT_AVATAR:if (ui_pick_image(main_window, chosen_avatar, 512))InvalidateRect(main_window, NULL, FALSE); break;
	case ACT_SAVE_PROFILE:
		ui_get(main_window, ED_NAME, name, 33); ui_get(main_window, ED_BIO, bio, 257); ui_get(main_window, ED_NEW_PASSWORD, password, 65);
		if (store_profile(&db, me.id, name, bio, chosen_avatar, password)) {
			SecureZeroMemory(password, sizeof(password)); current_user(); MessageBoxW(main_window, L"个人信息已保存。", L"保存成功", MB_OK | MB_ICONINFORMATION); navigate(MY);
		}
		else { SecureZeroMemory(password, sizeof(password)); ui_error(main_window, &db); }break;
	case ACT_WALLET:navigate(WALLET); break;
	case ACT_PURCHASED:navigate(PURCHASED); break;
	case ACT_LISTED:navigate(LISTED); break;
	case ACT_RECHARGE:
		if (!refresh_user())break; ui_get(main_window, ED_PRICE, amount, 48);
		if (!money_parse(amount, 0, &money)) { MessageBoxW(main_window, L"请输入有效充值金额，最多两位小数，范围 0.01～9999999.99 元。", L"金额有误", MB_OK | MB_ICONINFORMATION); break; }
		if (store_recharge(&db, me.id, money)) { current_user(); SetDlgItemTextW(main_window, ED_PRICE, L""); InvalidateRect(main_window, NULL, FALSE); MessageBoxW(main_window, L"充值成功，余额已更新。", L"充值成功", MB_OK | MB_ICONINFORMATION); }
		else ui_error(main_window, &db); break;
	}
}
/* 主窗口过程：分发命令与自绘，WM_PAINT 用内存位图双缓冲（避免自绘闪烁），并在窗口重新激活时从后台拉取最新数据。 */
/* 自绘密码框右侧的小眼睛按钮：明文显示时画睁眼（眼睛+瞳孔），
	 密码隐藏时画一道斜杠表示不可见；背景与输入框内部同为白色，叠加时无违和。 */
static void paint_eye(const DRAWITEMSTRUCT* d) {
	HDC dc = d->hDC; RECT r = d->rcItem;
	HWND password_edit = GetDlgItem(main_window, ED_PASSWORD);
	if (IsWindow(password_edit))password_visible = SendMessageW(password_edit, EM_GETPASSWORDCHAR, 0, 0) == 0;
	int pressed = (d->itemState & ODS_SELECTED) != 0;
	ui_fill(dc, r, pressed ? RGB(237, 240, 242) : C_WHITE);
	int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
	HPEN pen = CreatePen(PS_SOLID, 2, C_MUTED);
	HGDIOBJ old_pen = SelectObject(dc, pen);
	HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
	/* 眼睛外轮廓：横向椭圆（杏仁形）。 */
	Ellipse(dc, cx - 11, cy - 7, cx + 11, cy + 7);
	if (password_visible) {
		/* 明文：中间画实心瞳孔。 */
		HBRUSH b = CreateSolidBrush(C_MUTED);
		HGDIOBJ previous_brush = SelectObject(dc, b);
		Ellipse(dc, cx - 4, cy - 4, cx + 4, cy + 4);
		SelectObject(dc, previous_brush);
		DeleteObject(b);
	}
	else {
		/* 隐藏：一道从左下到右上的斜线穿过眼睛。 */
		MoveToEx(dc, cx - 11, cy + 9, NULL);
		LineTo(dc, cx + 11, cy - 9);
	}
	SelectObject(dc, old_brush);
	SelectObject(dc, old_pen);
	DeleteObject(pen);
	if (d->itemState & ODS_FOCUS) { RECT focus = r; InflateRect(&focus, -3, -3); DrawFocusRect(dc, &focus); }
}
static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
	switch (msg) {
	case WM_CREATE:main_window = hwnd; show_page(LOGIN); return 0;
	case WM_COMMAND:if (HIWORD(w) == BN_CLICKED || LOWORD(w) == IDOK) { busy++; command(LOWORD(w)); busy--; }return 0;
	case WM_DRAWITEM: { const DRAWITEMSTRUCT* d = (const DRAWITEMSTRUCT*)l; if (d->CtlID >= CARD_BASE)paint_card(d); else if (d->CtlID == ACT_EYE)paint_eye(d); else ui_draw_button(d); return TRUE; }
	case WM_CTLCOLORSTATIC:SetBkMode((HDC)w, TRANSPARENT); SetTextColor((HDC)w, C_INK); return(LRESULT)white_brush;
	case WM_CTLCOLOREDIT:SetBkColor((HDC)w, C_WHITE); SetTextColor((HDC)w, C_INK); return(LRESULT)white_brush;
	case WM_ERASEBKGND:return 1;
		/* 双缓冲：先在内存位图上整屏绘制，再一次性 BitBlt 到屏幕，消除自绘闪烁。 */
	case WM_PAINT: {
		PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); RECT r; GetClientRect(hwnd, &r);
		HDC mem = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, r.right, r.bottom); HGDIOBJ old = SelectObject(mem, bitmap);
		if (page == LOGIN || page == REGISTER)paint_login(mem); else paint_main(mem);
		BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY); SelectObject(mem, old); DeleteObject(bitmap); DeleteDC(mem); EndPaint(hwnd, &ps); return 0;
	}
	case WM_LBUTTONUP:if (page == MY) { int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l); if (x >= 220 && x <= 1068 && y >= 148 && y <= 303)navigate(PROFILE); }return 0;
	case WM_MOUSEWHEEL:if (content)SendMessageW(content, msg, w, l); return 0;
		/* 窗口从后台回到前台时刷新：列表/详情页整体重建，其余页仅更新用户并重绘，以便看到后台程序刚做的增删改；busy 时跳过，避免打断正在进行的操作。 */
	case WM_ACTIVATE:if (LOWORD(w) != WA_INACTIVE && me.id && !busy) {
		if (page == TRADE || page == MY || page == LISTED || page == PURCHASED || page == DETAIL)navigate(page);
		else { current_user(); if (!me.id)navigate(LOGIN); else InvalidateRect(hwnd, NULL, FALSE); }
	}return 0;
	case WM_DESTROY:PostQuitMessage(0); return 0;
	}
	return DefWindowProcW(hwnd, msg, w, l);
}
/* 前端程序入口：初始化界面、打开数据库（首次运行仅建立空表）、注册列表子窗口与主窗口两个类，创建主窗口后进入消息循环；退出时释放列表缓存、数据库与界面资源。 */
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR cmd, int show) {
	(void)previous; (void)cmd; ui_init(instance); white_brush = CreateSolidBrush(C_WHITE); bg_brush = CreateSolidBrush(C_BG);
	wchar_t path[MAX_PATH]; ui_database_path(path, MAX_PATH);
	if (!store_open(&db, path)) { ui_error(NULL, &db); store_close(&db); ui_cleanup(); return 1; }
	WNDCLASSW wc = { 0 }; wc.hInstance = instance; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpfnWndProc = content_proc; wc.lpszClassName = L"MarketContent"; wc.hbrBackground = bg_brush; RegisterClassW(&wc);
	ui_path(L"assets\\app.ico", path, MAX_PATH); wc.lpfnWndProc = main_proc; wc.lpszClassName = L"MarketFrontend"; wc.hIcon = (HICON)LoadImageW(NULL, path, IMAGE_ICON, 48, 48, LR_LOADFROMFILE); wc.hbrBackground = bg_brush; RegisterClassW(&wc);
	HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"拾物 · 二手交易", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
		CW_USEDEFAULT, CW_USEDEFAULT, W, H, NULL, NULL, instance, NULL);
	ui_center(hwnd, W, H); ShowWindow(hwnd, show); UpdateWindow(hwnd);
	MSG msg; while (GetMessageW(&msg, NULL, 0, 0) > 0) { if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
	clear_lists(); store_close(&db); DeleteObject(white_brush); DeleteObject(bg_brush); ui_cleanup(); return(int)msg.wParam;
}
