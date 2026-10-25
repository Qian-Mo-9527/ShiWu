#ifndef MARKET_STORE_H
#define MARKET_STORE_H
/* store.h：本地数据层接口。前端（frontend.c）与后台（backend.c）都通过本层
	 访问同一个 SQLite 数据库，界面代码不直接书写 SQL，使账号、余额和交易规则
	 集中在一处实现，保证两个程序行为一致。 */
#include <windows.h>
#include <stdint.h>

	 /* 金额一律以“分”为单位。上限9999999.99元。 */
typedef int64_t Money;
#define MONEY_MAX ((Money)999999999)
typedef struct sqlite3 sqlite3;
/* 一个数据库连接：sql 为 SQLite 句柄，error 存放最近一次可直接展示给用户的中文错误信息。 */
typedef struct Store { sqlite3* sql; wchar_t error[512]; } Store;
/* 用户：账号最长 32、昵称 32、简介 256 字符；avatar 为相对程序目录的图片路径；balance 单位为分。 */
typedef struct User {
	int id;
	wchar_t account[33], nickname[33], bio[257], avatar[512];
	Money balance;
} User;
/* 商品。sold 标记是否已成交；withdrawn 标记下架状态：0：在售；1：卖家自行下架；2：后台（管理员）下架。 */
typedef struct Product {
	int id, seller_id, sold, withdrawn;
	wchar_t name[81], description[1001], image[512], seller_name[33], created_at[32];
	Money price;
} Product;
/* 成交订单。下单时把商品名、图片、成交价以及买卖双方昵称复制为一份快照，即使用户或商品之后被删除，历史订单仍保留成交时的信息。 */
typedef struct Order {
	int id, product_id, buyer_id, seller_id;
	wchar_t product_name[81], image[512], seller_name[33], buyer_name[33], created_at[32];
	Money price;
} Order;

/* —— 连接与初始化 —— */
int store_open(Store* s, const wchar_t* path);
void store_close(Store* s);
const wchar_t* store_error(Store* s);

/* —— 账号、资料与钱包（前端使用）—— */
int store_register(Store* s, const wchar_t* account, const wchar_t* password, int* id);
int store_login(Store* s, const wchar_t* account, const wchar_t* password, User* out);
int store_user(Store* s, int id, User* out);
int store_users(Store* s, User** out, int* count);
int store_profile(Store* s, int id, const wchar_t* nickname, const wchar_t* bio, const wchar_t* avatar, const wchar_t* new_password);
int store_recharge(Store* s, int id, Money amount);

/* —— 商品与交易 —— */
int store_products(Store* s, int exclude_seller, int only_seller, int available_only, Product** out, int* count);
int store_product(Store* s, int id, Product* out);
int store_add_product(Store* s, int seller, const wchar_t* name, const wchar_t* description, Money price, const wchar_t* image, int* id);
int store_buy(Store* s, int buyer, int product);
/* 若当前价格与确认时的价格不一致，则拒绝该笔购买。 */
int store_buy_at_price(Store* s, int buyer, int product, Money expected_price);
int store_orders(Store* s, int buyer, Order** out, int* count);

/* —— 后台增删改查：id 为 0 表示新增；更新用户时密码留空表示保留原密码 —— */
int store_admin_user(Store* s, int id, const wchar_t* account, const wchar_t* password, const wchar_t* nickname, const wchar_t* bio, const wchar_t* avatar, Money balance, int* new_id);
int store_admin_product(Store* s, int id, int seller, const wchar_t* name, const wchar_t* description, Money price, const wchar_t* image, int sold, int withdrawn, int* new_id);
/* 保存已有记录的后台表单：未修改的字段保留数据库最新值，避免旧表单覆盖前端的新操作。 */
int store_edit_user(Store* s, const User* original, const User* edited, const wchar_t* password);
int store_edit_product(Store* s, const Product* original, const Product* edited);
int store_delete_user(Store* s, int id);
int store_delete_product(Store* s, int id);   /* 后台“删除”实际是把商品置为管理员下架 */
int store_withdraw_product(Store* s, int seller, int product);
void store_free(void* p);

/* —— 金额解析与格式化 —— */
/* 严格的小数解析器：按参数要求允许正数或零，最多两位小数。 */
int money_parse(const wchar_t* text, int allow_zero, Money* out);
void money_format(Money cents, wchar_t* out, size_t capacity);
#endif
