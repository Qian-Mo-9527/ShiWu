#define _CRT_SECURE_NO_WARNINGS
/* store.c：数据层实现，负责建表与迁移、账号密码、钱包、商品、订单以及购买的原子性。所有写操作要么在事务中完整成功，要么整体回滚，不产生中间状态。 */
#include "store.h"
#include "../vendor/sqlite3.h"
#include <bcrypt.h>
#include <wchar.h>
#include <wctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#pragma comment(lib, "bcrypt.lib")

/* PBKDF2 派生密码哈希的迭代次数，次数越高离线暴力破解的成本越高。 */
#define HASH_ITERATIONS 120000

/* 记录一条可直接展示的错误信息并统一返回 0（失败），方便调用方直接 return。 */
static int failure(Store* s, const wchar_t* message) {
	wcsncpy_s(s->error, _countof(s->error), message, _TRUNCATE);
	return 0;
}
/* 依据 SQLite 扩展错误码给出对应的中文提示（唯一约束、外键、忙锁、只读、磁盘满等）。 */
static int sql_failure(Store* s) {
	int code = sqlite3_extended_errcode(s->sql);
	if (code == SQLITE_CONSTRAINT_UNIQUE) return failure(s, L"账号已存在，请使用其他账号");
	if (code == SQLITE_CONSTRAINT_FOREIGNKEY) return failure(s, L"用户或商品不存在，请刷新后重试");
	if ((code & 255) == SQLITE_BUSY || (code & 255) == SQLITE_LOCKED) return failure(s, L"数据正在使用中，请稍后重试");
	if ((code & 255) == SQLITE_READONLY) return failure(s, L"数据目录不可写，请将完整程序文件夹移到可写位置");
	if ((code & 255) == SQLITE_FULL) return failure(s, L"磁盘空间不足，数据未保存");
	return failure(s, L"本地数据操作失败，请检查数据文件和磁盘是否可写");
}
/* 执行没有返回结果集的 SQL（建表、PRAGMA、事务语句等），成功返回 1。 */
static int run(Store* s, const char* sql) {
	return sqlite3_exec(s->sql, sql, NULL, NULL, NULL) == SQLITE_OK ? 1 : sql_failure(s);
}
/* 预编译一条带 ? 占位符的 SQL，供后续绑定参数、分步执行。 */
static int prepare(Store* s, const char* sql, sqlite3_stmt** stmt) {
	return sqlite3_prepare_v2(s->sql, sql, -1, stmt, NULL) == SQLITE_OK ? 1 : sql_failure(s);
}
/* 以 UTF-16 绑定一个文本参数；NULL 统一绑定为空字符串。 */
static void bind_text(sqlite3_stmt* q, int pos, const wchar_t* value) {
	sqlite3_bind_text16(q, pos, value ? value : L"", -1, SQLITE_TRANSIENT);
}
/* 读取第 n 列文本到定长宽字符缓冲，超长时安全截断。 */
static void copy_column(sqlite3_stmt* q, int n, wchar_t* out, size_t cap) {
	const wchar_t* v = sqlite3_column_text16(q, n);
	wcsncpy_s(out, cap, v ? v : L"", _TRUNCATE);
}
/* 执行一次写入（单步）并释放语句，返回是否恰好执行完成。 */
static int finish_write(Store* s, sqlite3_stmt* q) {
	int ok = sqlite3_step(q) == SQLITE_DONE;
	if (!ok) sql_failure(s);
	sqlite3_finalize(q);
	return ok;
}
/* 回滚当前事务并返回 0，用于出错时统一收尾。 */
static int rollback(Store* s) {
	sqlite3_exec(s->sql, "ROLLBACK", NULL, NULL, NULL);
	return 0;
}
/* 提交事务；提交失败则回滚，避免遗留未结束的事务。 */
static int commit(Store* s) {
	return run(s, "COMMIT") ? 1 : rollback(s);
}
/* —— 输入校验：不通过时写入中文提示并返回 0 —— */
/* 字符串是否含至少一个非空白字符，用于拒绝只填空格的名称、介绍等。 */
static int has_nonspace(const wchar_t* v) {
	if (!v) return 0;
	while (*v) if (!iswspace(*v++)) return 1;
	return 0;
}
static int valid_account(Store* s, const wchar_t* v) {
	size_t n = v ? wcslen(v) : 0, i;
	if (!n || n > 32) return failure(s, L"账号长度须为 1 至 32 个字符");
	for (i = 0; i < n; ++i)
		if (iswspace(v[i]) || iswcntrl(v[i])) return failure(s, L"账号不能包含空白或控制字符");
	return 1;
}
static int valid_password(Store* s, const wchar_t* v) {
	size_t n = v ? wcslen(v) : 0;
	if (!n || n > 64) return failure(s, L"密码长度须为 1 至 64 个字符");
	return 1;
}
static int valid_profile(Store* s, const wchar_t* nick, const wchar_t* bio, const wchar_t* avatar) {
	if (!has_nonspace(nick) || wcslen(nick) > 32) return failure(s, L"昵称不能为空，最多 32 个字符");
	if (bio && wcslen(bio) > 256) return failure(s, L"个人简介最多 256 个字符");
	if (avatar && wcslen(avatar) > 511) return failure(s, L"头像路径过长");
	return 1;
}
/* 商品校验：名称、介绍、图片必填，价格为正且不超过上限。 */
static int valid_product(Store* s, const wchar_t* name, const wchar_t* desc, Money price, const wchar_t* image) {
	if (!has_nonspace(name) || wcslen(name) > 80) return failure(s, L"商品名称不能为空，最多 80 个字符");
	if (!has_nonspace(desc) || wcslen(desc) > 1000) return failure(s, L"商品介绍不能为空，最多 1000 个字符");
	if (price <= 0 || price > MONEY_MAX) return failure(s, L"商品价格须大于 0，最高 9999999.99 元");
	if (!has_nonspace(image) || wcslen(image) > 511) return failure(s, L"请选择一张商品图片");
	return 1;
}
/* 用 PBKDF2-HMAC-SHA256 由“密码 + 随机盐”派生 32 字节哈希：先把密码转成 UTF-8 再派生，结束后立即安全清零明文缓冲。 */
static int password_hash(Store* s, const wchar_t* password, const unsigned char salt[16], unsigned char hash[32]) {
	BCRYPT_ALG_HANDLE alg = NULL;
	NTSTATUS status;
	int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1, NULL, 0, NULL, NULL);
	char utf8[257];
	if (!bytes || bytes > (int)sizeof(utf8)) return failure(s, L"密码包含无效字符");
	WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1, utf8, bytes, NULL, NULL);
	status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
	if (status >= 0) status = BCryptDeriveKeyPBKDF2(alg, (PUCHAR)utf8, (ULONG)(bytes - 1), (PUCHAR)salt, 16, HASH_ITERATIONS, hash, 32, 0);
	SecureZeroMemory(utf8, sizeof(utf8));
	if (alg) BCryptCloseAlgorithmProvider(alg, 0);
	return status >= 0 ? 1 : failure(s, L"本机密码处理失败");
}
/* 为新密码或改密生成 16 字节随机盐后再计算哈希；数据库只保存盐和哈希，不保存明文。 */
static int new_hash(Store* s, const wchar_t* password, unsigned char salt[16], unsigned char hash[32]) {
	if (!valid_password(s, password)) return 0;
	if (BCryptGenRandom(NULL, salt, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return failure(s, L"本机随机数生成失败");
	return password_hash(s, password, salt, hash);
}
/* 把当前行的各列按固定顺序映射到 User 结构体。 */
static void read_user(sqlite3_stmt* q, User* u) {
	memset(u, 0, sizeof(*u));
	u->id = sqlite3_column_int(q, 0);
	copy_column(q, 1, u->account, _countof(u->account));
	copy_column(q, 2, u->nickname, _countof(u->nickname));
	copy_column(q, 3, u->bio, _countof(u->bio));
	copy_column(q, 4, u->avatar, _countof(u->avatar));
	u->balance = sqlite3_column_int64(q, 5);
}
/* 把当前行的各列按固定顺序映射到 Product 结构体（含卖家昵称与下架状态）。 */
static void read_product(sqlite3_stmt* q, Product* p) {
	memset(p, 0, sizeof(*p));
	p->id = sqlite3_column_int(q, 0); p->seller_id = sqlite3_column_int(q, 1);
	copy_column(q, 2, p->name, _countof(p->name));
	copy_column(q, 3, p->description, _countof(p->description));
	p->price = sqlite3_column_int64(q, 4);
	copy_column(q, 5, p->image, _countof(p->image));
	p->sold = sqlite3_column_int(q, 6);
	copy_column(q, 7, p->seller_name, _countof(p->seller_name));
	copy_column(q, 8, p->created_at, _countof(p->created_at));
	p->withdrawn = sqlite3_column_int(q, 9);
}

/* 打开（不存在则创建）本地数据库：开启外键与 WAL，幂等建表，并把旧版本库迁移到最新结构。任何一步失败都会关闭连接并返回 0，不会留下半成品数据库。 */
int store_open(Store* s, const wchar_t* path) {
	sqlite3_stmt* q = NULL; int rc, found = 0;
	memset(s, 0, sizeof(*s));
	if (sqlite3_open16(path, &s->sql) != SQLITE_OK) {
		failure(s, L"无法打开本地数据库，请确认完整程序文件夹位于可写目录");
		store_close(s); return 0;
	}
	sqlite3_extended_result_codes(s->sql, 1);
	sqlite3_busy_timeout(s->sql, 5000);   /* 前后端同时写时，最多等待 5 秒再报忙 */
	/* 外键约束开启；WAL 允许两个程序并发读写；synchronous=FULL 保证已提交事务掉电不丢。 */
	if (!run(s, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;")) { store_close(s); return 0; }
	/* 幂等建表：余额、价格用 CHECK 限定范围；商品随卖家外键级联删除，订单外键则置空（SET NULL），从而在删除用户后仍保留成交快照。 */
	if (!run(s,
		"CREATE TABLE IF NOT EXISTS users("
		"id INTEGER PRIMARY KEY AUTOINCREMENT, account TEXT NOT NULL UNIQUE COLLATE BINARY,"
		"password_salt BLOB NOT NULL CHECK(length(password_salt)=16), password_hash BLOB NOT NULL CHECK(length(password_hash)=32),"
		"nickname TEXT NOT NULL,bio TEXT NOT NULL DEFAULT '',avatar TEXT NOT NULL DEFAULT '',"
		"balance INTEGER NOT NULL DEFAULT 0 CHECK(balance BETWEEN 0 AND 999999999));"
		"CREATE TABLE IF NOT EXISTS products("
		"id INTEGER PRIMARY KEY AUTOINCREMENT,seller_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
		"name TEXT NOT NULL,description TEXT NOT NULL,price INTEGER NOT NULL CHECK(price BETWEEN 1 AND 999999999),"
		"image TEXT NOT NULL,sold INTEGER NOT NULL DEFAULT 0 CHECK(sold IN(0,1)),"
		"withdrawn INTEGER NOT NULL DEFAULT 0 CHECK(withdrawn IN(0,1,2)),"
		"created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%S','now','localtime')));"
		"CREATE TABLE IF NOT EXISTS orders("
		"id INTEGER PRIMARY KEY AUTOINCREMENT,product_id INTEGER REFERENCES products(id) ON DELETE SET NULL,"
		"buyer_id INTEGER REFERENCES users(id) ON DELETE SET NULL,seller_id INTEGER REFERENCES users(id) ON DELETE SET NULL,"
		"product_name TEXT NOT NULL,image TEXT NOT NULL,price INTEGER NOT NULL CHECK(price>0),"
		"buyer_name TEXT NOT NULL,seller_name TEXT NOT NULL,"
		"created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%S','now','localtime')));"
		"CREATE INDEX IF NOT EXISTS products_seller ON products(seller_id);"
		"CREATE INDEX IF NOT EXISTS orders_buyer ON orders(buyer_id);")) {
		store_close(s); return 0;
	}
	/* 串行执行数据库迁移，避免两个程序首次启动时同时执行 ALTER TABLE。 */
	if (!run(s, "BEGIN IMMEDIATE")) { store_close(s); return 0; }
	/* 查看 products 表的列：若旧库缺少 withdrawn 列则随后补列；found 表示该列是否已存在。 */
	if (!prepare(s, "PRAGMA table_info(products)", &q)) { rollback(s); store_close(s); return 0; }
	while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
		const char* name = (const char*)sqlite3_column_text(q, 1);
		if (name && !strcmp(name, "withdrawn"))found = 1;
	}
	if (rc != SQLITE_DONE) { sql_failure(s); sqlite3_finalize(q); rollback(s); store_close(s); return 0; }
	sqlite3_finalize(q);
	if (!found && !run(s, "ALTER TABLE products ADD COLUMN withdrawn INTEGER NOT NULL DEFAULT 0 CHECK(withdrawn IN(0,1,2))")) { rollback(s); store_close(s); return 0; }
	if (!commit(s)) { store_close(s); return 0; }
	return 1;
}
void store_close(Store* s) { if (s && s->sql) { sqlite3_close(s->sql); s->sql = NULL; } }
const wchar_t* store_error(Store* s) { return s->error[0] ? s->error : L"操作失败，请刷新后重试"; }
void store_free(void* p) { free(p); }

/* 注册：复用后台新增用户逻辑，昵称默认取账号、初始余额为 0。 */
int store_register(Store* s, const wchar_t* account, const wchar_t* password, int* id) {
	return store_admin_user(s, 0, account, password, account, L"", L"", 0, id);
}
/* 登录：取出该账号的盐和已保存哈希，用相同盐对输入密码重新派生后逐字节比较。比较用按位或累积（不提前返回），耗时与差异位置无关，可缓解计时侧信道；账号不存在与密码错误都返回相同提示，不暴露账号是否存在。 */
int store_login(Store* s, const wchar_t* account, const wchar_t* password, User* out) {
	sqlite3_stmt* q = NULL;
	unsigned char expected[32], salt[16], actual[32];
	unsigned int diff = 0; int i, uid = 0;
	if (!valid_account(s, account) || !valid_password(s, password)) return 0;
	if (!prepare(s, "SELECT id,password_salt,password_hash FROM users WHERE account=?", &q)) return 0;
	bind_text(q, 1, account);
	if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_bytes(q, 1) == 16 && sqlite3_column_bytes(q, 2) == 32) {
		uid = sqlite3_column_int(q, 0); memcpy(salt, sqlite3_column_blob(q, 1), 16); memcpy(expected, sqlite3_column_blob(q, 2), 32);
	}
	sqlite3_finalize(q);
	if (!uid) return failure(s, L"账号或密码错误");
	if (!password_hash(s, password, salt, actual)) return 0;
	for (i = 0; i < 32; ++i) diff |= expected[i] ^ actual[i];
	SecureZeroMemory(actual, sizeof(actual)); SecureZeroMemory(expected, sizeof(expected));
	if (diff) return failure(s, L"账号或密码错误");
	return store_user(s, uid, out);
}
int store_user(Store* s, int id, User* out) {
	sqlite3_stmt* q = NULL; int rc;
	if (!prepare(s, "SELECT id,account,nickname,bio,avatar,balance FROM users WHERE id=?", &q)) return 0;
	sqlite3_bind_int(q, 1, id); rc = sqlite3_step(q);
	if (rc == SQLITE_ROW) read_user(q, out);
	else if (rc == SQLITE_DONE) failure(s, L"用户不存在，请重新登录"); else sql_failure(s);
	sqlite3_finalize(q); return rc == SQLITE_ROW;
}
/* 读取全部用户到动态数组（容量按 16、32、64… 翻倍扩容）。成功时 *out 需由调用方用 store_free 释放；商品、订单列表也采用同一模式。 */
int store_users(Store* s, User** out, int* count) {
	sqlite3_stmt* q = NULL; User* items = NULL; int n = 0, cap = 0, rc;
	*out = NULL; *count = 0;
	if (!prepare(s, "SELECT id,account,nickname,bio,avatar,balance FROM users ORDER BY id", &q)) return 0;
	while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
		if (n == cap) { User* p; cap = cap ? cap * 2 : 16; p = realloc(items, (size_t)cap * sizeof(*p)); if (!p) { free(items); sqlite3_finalize(q); return failure(s, L"内存不足"); } items = p; }
		read_user(q, &items[n++]);
	}
	if (rc != SQLITE_DONE) { free(items); sql_failure(s); sqlite3_finalize(q); return 0; }
	sqlite3_finalize(q); *out = items; *count = n; return 1;
}
/* 后台新增（id=0）或更新（id>0）用户。change 表示本次是否要写入密码：新增用户、或更新时填写了新密码才重新生成盐和哈希；更新时密码留空则保留原密码。 */
int store_admin_user(Store* s, int id, const wchar_t* account, const wchar_t* password, const wchar_t* nickname, const wchar_t* bio, const wchar_t* avatar, Money balance, int* new_id) {
	sqlite3_stmt* q = NULL; unsigned char salt[16], hash[32];
	int change = !id || (password && *password);
	if (!valid_account(s, account) || !valid_profile(s, nickname, bio, avatar))return 0;
	if (balance<0 || balance>MONEY_MAX)return failure(s, L"余额须在 0 至 9999999.99 元之间");
	if (change && !new_hash(s, password, salt, hash))return 0;
	if (!id) {
		if (!prepare(s, "INSERT INTO users(account,nickname,bio,avatar,balance,password_salt,password_hash) VALUES(?,?,?,?,?,?,?)", &q))return 0;
	}
	else {
		if (!prepare(s, change ? "UPDATE users SET account=?,nickname=?,bio=?,avatar=?,balance=?,password_salt=?,password_hash=? WHERE id=?" : "UPDATE users SET account=?,nickname=?,bio=?,avatar=?,balance=? WHERE id=?", &q))return 0;
	}
	bind_text(q, 1, account); bind_text(q, 2, nickname); bind_text(q, 3, bio); bind_text(q, 4, avatar); sqlite3_bind_int64(q, 5, balance);
	if (change) { sqlite3_bind_blob(q, 6, salt, 16, SQLITE_TRANSIENT); sqlite3_bind_blob(q, 7, hash, 32, SQLITE_TRANSIENT); }
	if (id)sqlite3_bind_int(q, change ? 8 : 6, id);
	if (!finish_write(s, q))return 0;
	SecureZeroMemory(hash, sizeof(hash));
	if (id && !sqlite3_changes(s->sql))return failure(s, L"用户不存在，请刷新列表");
	if (new_id)*new_id = id ? id : (int)sqlite3_last_insert_rowid(s->sql);
	return 1;
}
/* 后台编辑用户时，把“表单打开时的原值”与“本次提交值”逐字段比较。
	未改动的字段保留数据库当前值，尤其不能用旧余额覆盖刚充值或卖出后的入账。
	重新读取、合并和保存都在同一写事务内，避免读取最新余额后再次被另一连接改动。 */
int store_edit_user(Store* s, const User* original, const User* edited, const wchar_t* password) {
	User merged;
	if (!original || !edited || original->id <= 0 || edited->id != original->id)
		return failure(s, L"用户编辑记录无效，请刷新后重试");
	if (!run(s, "BEGIN IMMEDIATE"))return 0;
	if (!store_user(s, original->id, &merged))return rollback(s);
	if (wcscmp(edited->account, original->account))wcscpy_s(merged.account, _countof(merged.account), edited->account);
	if (wcscmp(edited->nickname, original->nickname))wcscpy_s(merged.nickname, _countof(merged.nickname), edited->nickname);
	if (wcscmp(edited->bio, original->bio))wcscpy_s(merged.bio, _countof(merged.bio), edited->bio);
	if (wcscmp(edited->avatar, original->avatar))wcscpy_s(merged.avatar, _countof(merged.avatar), edited->avatar);
	if (edited->balance != original->balance)merged.balance = edited->balance;
	if (!store_admin_user(s, merged.id, merged.account, password, merged.nickname, merged.bio,
		merged.avatar, merged.balance, NULL))return rollback(s);
	return commit(s);
}
/* 用户在前端修改自己的昵称、简介、头像；新密码留空（change 为假）则不改动密码。 */
int store_profile(Store* s, int id, const wchar_t* nickname, const wchar_t* bio, const wchar_t* avatar, const wchar_t* password) {
	sqlite3_stmt* q = NULL; unsigned char salt[16], hash[32]; int change = password && *password;
	if (!valid_profile(s, nickname, bio, avatar))return 0;
	if (change && !new_hash(s, password, salt, hash))return 0;
	if (!prepare(s, change ? "UPDATE users SET nickname=?,bio=?,avatar=?,password_salt=?,password_hash=? WHERE id=?" : "UPDATE users SET nickname=?,bio=?,avatar=? WHERE id=?", &q))return 0;
	bind_text(q, 1, nickname); bind_text(q, 2, bio); bind_text(q, 3, avatar);
	if (change) { sqlite3_bind_blob(q, 4, salt, 16, SQLITE_TRANSIENT); sqlite3_bind_blob(q, 5, hash, 32, SQLITE_TRANSIENT); }
	sqlite3_bind_int(q, change ? 6 : 4, id);
	if (!finish_write(s, q))return 0;
	SecureZeroMemory(hash, sizeof(hash));
	return sqlite3_changes(s->sql) ? 1 : failure(s, L"用户不存在，请重新登录");
}
/* 充值。把“余额不超过上限减去本次金额”作为 UPDATE 的条件，直接在数据库层判断，避免“先读余额再更新”期间被另一程序改动而超额；没有命中行即代表会超限。 */
int store_recharge(Store* s, int id, Money amount) {
	sqlite3_stmt* q = NULL; User u;
	if (amount <= 0 || amount > MONEY_MAX)return failure(s, L"充值金额须大于 0，最高 9999999.99 元");
	if (!prepare(s, "UPDATE users SET balance=balance+? WHERE id=? AND balance<=?", &q))return 0;
	sqlite3_bind_int64(q, 1, amount); sqlite3_bind_int(q, 2, id); sqlite3_bind_int64(q, 3, MONEY_MAX - amount);
	if (!finish_write(s, q))return 0;
	if (sqlite3_changes(s->sql))return 1;
	if (!store_user(s, id, &u))return 0;
	return failure(s, L"充值后余额超出上限 9999999.99 元");
}
/* 商品列表，三个过滤开关由绑定值控制（传 0 表示不启用）：
	 exclude_seller 排除指定卖家（交易页不显示自己上架的商品）；
	 only_seller 只看指定卖家（“我的-已上架”）；
	 available_only 只返回未售出且未下架的在售商品（交易页）。 */
int store_products(Store* s, int exclude_seller, int only_seller, int available_only, Product** out, int* count) {
	sqlite3_stmt* q = NULL; Product* items = NULL; int n = 0, cap = 0, rc;
	*out = NULL; *count = 0;
	if (!prepare(s, "SELECT p.id,p.seller_id,p.name,p.description,p.price,p.image,p.sold,u.nickname,p.created_at,p.withdrawn FROM products p JOIN users u ON u.id=p.seller_id WHERE (?=0 OR p.seller_id<>?) AND (?=0 OR p.seller_id=?) AND (?=0 OR (p.sold=0 AND p.withdrawn=0)) ORDER BY p.id DESC", &q))return 0;
	sqlite3_bind_int(q, 1, exclude_seller); sqlite3_bind_int(q, 2, exclude_seller); sqlite3_bind_int(q, 3, only_seller); sqlite3_bind_int(q, 4, only_seller); sqlite3_bind_int(q, 5, available_only);
	while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
		if (n == cap) { Product* p; cap = cap ? cap * 2 : 16; p = realloc(items, (size_t)cap * sizeof(*p)); if (!p) { free(items); sqlite3_finalize(q); return failure(s, L"内存不足"); }items = p; }
		read_product(q, &items[n++]);
	}
	if (rc != SQLITE_DONE) { free(items); sql_failure(s); sqlite3_finalize(q); return 0; }
	sqlite3_finalize(q); *out = items; *count = n; return 1;
}
int store_product(Store* s, int id, Product* out) {
	sqlite3_stmt* q = NULL; int rc;
	if (!prepare(s, "SELECT p.id,p.seller_id,p.name,p.description,p.price,p.image,p.sold,u.nickname,p.created_at,p.withdrawn FROM products p JOIN users u ON u.id=p.seller_id WHERE p.id=?", &q))return 0;
	sqlite3_bind_int(q, 1, id); rc = sqlite3_step(q);
	if (rc == SQLITE_ROW)read_product(q, out); else if (rc == SQLITE_DONE)failure(s, L"商品不存在，可能已被删除"); else sql_failure(s);
	sqlite3_finalize(q); return rc == SQLITE_ROW;
}
/* 上架新商品：售出、下架状态均初始化为 0。 */
int store_add_product(Store* s, int seller, const wchar_t* name, const wchar_t* description, Money price, const wchar_t* image, int* id) {
	return store_admin_product(s, 0, seller, name, description, price, image, 0, 0, id);
}
/* 查询某商品是否已有成交订单，用于“已成交商品不可删除、下架或改回未售”的保护。 */
static int product_has_orders(Store* s, int id, int* found) {
	sqlite3_stmt* q = NULL; int rc;
	if (!prepare(s, "SELECT 1 FROM orders WHERE product_id=? LIMIT 1", &q))return 0;
	sqlite3_bind_int(q, 1, id); rc = sqlite3_step(q);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) { sql_failure(s); sqlite3_finalize(q); return 0; }
	*found = rc == SQLITE_ROW; sqlite3_finalize(q); return 1;
}
/* 后台新增（id=0）或更新（id>0）商品。更新已有商品时在事务中先检查是否有订单：一旦存在成交记录，就不能删除、下架或把状态改回未售（见下方 has_orders 分支）。own_transaction 标记事务是否由本函数开启，以决定结尾提交还是回滚。 */
int store_admin_product(Store* s, int id, int seller, const wchar_t* name, const wchar_t* description, Money price, const wchar_t* image, int sold, int withdrawn, int* new_id) {
	sqlite3_stmt* q = NULL; int own_transaction = 0, has_orders;
	if (!valid_product(s, name, description, price, image))return 0;
	if (sold != 0 && sold != 1)return failure(s, L"商品状态无效");
	if (withdrawn < 0 || withdrawn>2)return failure(s, L"商品下架状态无效");
	if (sold && withdrawn)return failure(s, L"已完成购买的商品不可删除");
	if (id) {
		own_transaction = sqlite3_get_autocommit(s->sql);
		if (own_transaction && !run(s, "BEGIN IMMEDIATE"))return 0;
		if (!product_has_orders(s, id, &has_orders)) { if (own_transaction)rollback(s); return 0; }
		if (has_orders && (!sold || withdrawn)) {
			failure(s, L"已完成购买的商品不可删除或重新上架");
			if (own_transaction)rollback(s); return 0;
		}
	}
	if (!prepare(s, id ? "UPDATE products SET seller_id=?,name=?,description=?,price=?,image=?,sold=?,withdrawn=? WHERE id=?" : "INSERT INTO products(seller_id,name,description,price,image,sold,withdrawn) VALUES(?,?,?,?,?,?,?)", &q)) { if (own_transaction)rollback(s); return 0; }
	sqlite3_bind_int(q, 1, seller); bind_text(q, 2, name); bind_text(q, 3, description); sqlite3_bind_int64(q, 4, price); bind_text(q, 5, image); sqlite3_bind_int(q, 6, sold); sqlite3_bind_int(q, 7, withdrawn); if (id)sqlite3_bind_int(q, 8, id);
	if (!finish_write(s, q)) { if (own_transaction)rollback(s); return 0; }
	if (id && !sqlite3_changes(s->sql)) { failure(s, L"商品不存在，请刷新列表"); if (own_transaction)rollback(s); return 0; }
	if (new_id)*new_id = id ? id : (int)sqlite3_last_insert_rowid(s->sql);
	return own_transaction ? commit(s) : 1;
}
/* 后台仅修改介绍、图片等内容时，不能撤销卖家刚执行的下架或覆盖刚完成的售出。
	sold 与 withdrawn 作为一个状态整体比较；管理员确实修改状态时仍走原有成交保护。
	调用核心保存函数时外层事务已持有写锁，由本函数统一提交或回滚。 */
int store_edit_product(Store* s, const Product* original, const Product* edited) {
	Product merged;
	if (!original || !edited || original->id <= 0 || edited->id != original->id)
		return failure(s, L"商品编辑记录无效，请刷新后重试");
	if (!run(s, "BEGIN IMMEDIATE"))return 0;
	if (!store_product(s, original->id, &merged))return rollback(s);
	if (edited->seller_id != original->seller_id)merged.seller_id = edited->seller_id;
	if (wcscmp(edited->name, original->name))wcscpy_s(merged.name, _countof(merged.name), edited->name);
	if (wcscmp(edited->description, original->description))wcscpy_s(merged.description, _countof(merged.description), edited->description);
	if (edited->price != original->price)merged.price = edited->price;
	if (wcscmp(edited->image, original->image))wcscpy_s(merged.image, _countof(merged.image), edited->image);
	if (edited->sold != original->sold || edited->withdrawn != original->withdrawn) {
		merged.sold = edited->sold; merged.withdrawn = edited->withdrawn;
	}
	if (!store_admin_product(s, merged.id, merged.seller_id, merged.name, merged.description,
		merged.price, merged.image, merged.sold, merged.withdrawn, NULL))return rollback(s);
	return commit(s);
}
/* 不校验确认价的便捷购买入口（expected_price 传 -1 表示跳过价格比对）。 */
int store_buy(Store* s, int buyer, int product) {
	return store_buy_at_price(s, buyer, product, -1);
}
/* 核心交易：原子地完成一次购买。BEGIN IMMEDIATE 立即取得写锁，把两个程序的并发购买串行化；先完成全部前置校验，再依次扣款、给卖家加款、标记售出、写入订单快照，任何一步失败都整体回滚，避免出现“扣了钱却没成交”等中间状态。 */
int store_buy_at_price(Store* s, int buyer, int product, Money expected_price) {
	Product p; User b, v; sqlite3_stmt* q = NULL;
	if (!run(s, "BEGIN IMMEDIATE"))return 0;   /* 立即加写锁，使同一商品的并发购买排队执行 */
	if (!store_product(s, product, &p))return rollback(s);
	if (expected_price >= 0 && expected_price != p.price) { failure(s, L"商品价格已变更，请刷新后重新确认"); return rollback(s); }
	if (!store_user(s, buyer, &b))return rollback(s);
	if (p.withdrawn) { failure(s, p.withdrawn == 2 ? L"商品已被管理员下架" : L"商品已下架"); return rollback(s); }
	if (p.sold) { failure(s, L"商品已售出，请选择其他商品"); return rollback(s); }
	if (p.seller_id == buyer) { failure(s, L"不能购买自己上架的商品"); return rollback(s); }
	if (b.balance < p.price) { failure(s, L"余额不足，请充值"); return rollback(s); }
	if (!store_user(s, p.seller_id, &v))return rollback(s);
	if (v.balance > MONEY_MAX - p.price) { failure(s, L"卖家余额已达上限，暂时无法购买"); return rollback(s); }
	/* 1) 买家扣款 */
	if (!prepare(s, "UPDATE users SET balance=balance-? WHERE id=?", &q))return rollback(s);
	sqlite3_bind_int64(q, 1, p.price); sqlite3_bind_int(q, 2, buyer); if (!finish_write(s, q))return rollback(s);
	/* 2) 卖家收款 */
	if (!prepare(s, "UPDATE users SET balance=balance+? WHERE id=?", &q))return rollback(s);
	sqlite3_bind_int64(q, 1, p.price); sqlite3_bind_int(q, 2, p.seller_id); if (!finish_write(s, q))return rollback(s);
	/* 3) 条件标记售出：仅当仍为未售出且未下架时才生效，保证每件商品只成交一次 */
	if (!prepare(s, "UPDATE products SET sold=1 WHERE id=? AND sold=0 AND withdrawn=0", &q))return rollback(s);
	sqlite3_bind_int(q, 1, product); if (!finish_write(s, q))return rollback(s);
	/* 没有恰好更新一行，说明商品已被他人抢先购买或已下架，回滚本次购买。 */
	if (sqlite3_changes(s->sql) != 1) { failure(s, L"商品状态已变化，请刷新后重试"); return rollback(s); }
	/* 4) 写入成交订单（含商品名、图片、成交价及买卖双方昵称的快照） */
	if (!prepare(s, "INSERT INTO orders(product_id,buyer_id,seller_id,product_name,image,price,buyer_name,seller_name) VALUES(?,?,?,?,?,?,?,?)", &q))return rollback(s);
	sqlite3_bind_int(q, 1, product); sqlite3_bind_int(q, 2, buyer); sqlite3_bind_int(q, 3, p.seller_id); bind_text(q, 4, p.name); bind_text(q, 5, p.image); sqlite3_bind_int64(q, 6, p.price); bind_text(q, 7, b.nickname); bind_text(q, 8, v.nickname);
	if (!finish_write(s, q))return rollback(s);
	return commit(s);
}
int store_orders(Store* s, int buyer, Order** out, int* count) {
	sqlite3_stmt* q = NULL; Order* items = NULL; int n = 0, cap = 0, rc;
	*out = NULL; *count = 0;
	if (!prepare(s, "SELECT id,product_id,buyer_id,seller_id,product_name,image,price,buyer_name,seller_name,created_at FROM orders WHERE (?=0 OR buyer_id=?) ORDER BY id DESC", &q))return 0;
	sqlite3_bind_int(q, 1, buyer); sqlite3_bind_int(q, 2, buyer);
	while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
		Order* o;
		if (n == cap) { Order* p; cap = cap ? cap * 2 : 16; p = realloc(items, (size_t)cap * sizeof(*p)); if (!p) { free(items); sqlite3_finalize(q); return failure(s, L"内存不足"); }items = p; }
		o = &items[n++]; memset(o, 0, sizeof(*o)); o->id = sqlite3_column_int(q, 0); o->product_id = sqlite3_column_int(q, 1); o->buyer_id = sqlite3_column_int(q, 2); o->seller_id = sqlite3_column_int(q, 3);
		copy_column(q, 4, o->product_name, _countof(o->product_name)); copy_column(q, 5, o->image, _countof(o->image)); o->price = sqlite3_column_int64(q, 6); copy_column(q, 7, o->buyer_name, _countof(o->buyer_name)); copy_column(q, 8, o->seller_name, _countof(o->seller_name)); copy_column(q, 9, o->created_at, _countof(o->created_at));
	}
	if (rc != SQLITE_DONE) { free(items); sql_failure(s); sqlite3_finalize(q); return 0; }
	sqlite3_finalize(q); *out = items; *count = n; return 1;
}
/* 后台“删除商品”按规则实为管理员下架（withdrawn=2）：SQL 条件保证只处理未售出且没有成交订单的商品；一条都没更新时，再读取商品给出具体原因。 */
int store_delete_product(Store* s, int id) {
	sqlite3_stmt* q = NULL; Product p;
	if (!prepare(s, "UPDATE products SET withdrawn=2 WHERE id=? AND sold=0 AND NOT EXISTS(SELECT 1 FROM orders WHERE product_id=products.id)", &q))return 0;
	sqlite3_bind_int(q, 1, id); if (!finish_write(s, q))return 0;
	if (sqlite3_changes(s->sql))return 1;
	if (!store_product(s, id, &p))return 0;
	return failure(s, L"已完成购买的商品不可删除");
}
/* 卖家自行下架（withdrawn=1）。SQL 同时校验归属、未售出、未下架且无成交订单；未命中时再区分“非本人商品 / 已成交 / 已下架”等原因给出提示。 */
int store_withdraw_product(Store* s, int seller, int product) {
	sqlite3_stmt* q = NULL; Product p; int has_orders;
	if (!prepare(s, "UPDATE products SET withdrawn=1 WHERE id=? AND seller_id=? AND sold=0 AND withdrawn=0 AND NOT EXISTS(SELECT 1 FROM orders WHERE product_id=products.id)", &q))return 0;
	sqlite3_bind_int(q, 1, product); sqlite3_bind_int(q, 2, seller); if (!finish_write(s, q))return 0;
	if (sqlite3_changes(s->sql))return 1;
	if (!store_product(s, product, &p))return 0;
	if (p.seller_id != seller)return failure(s, L"只能下架自己上架的商品");
	if (!product_has_orders(s, product, &has_orders))return 0;
	if (p.sold || has_orders)return failure(s, L"已完成购买的商品不可删除");
	return failure(s, p.withdrawn == 2 ? L"商品已被管理员下架" : L"商品已下架");
}
/* 删除用户：由外键级联（ON DELETE CASCADE）一并删除其商品；其历史订单的用户外键置空（SET NULL），但订单里的昵称、商品名等成交快照仍保留。 */
int store_delete_user(Store* s, int id) {
	sqlite3_stmt* q = NULL;
	if (!prepare(s, "DELETE FROM users WHERE id=?", &q))return 0;
	sqlite3_bind_int(q, 1, id); if (!finish_write(s, q))return 0;
	return sqlite3_changes(s->sql) ? 1 : failure(s, L"用户不存在，请刷新列表");
}

/* 把“元.角分”文本严格解析为整数分：只允许数字与一个小数点、最多两位小数，拒绝任何其他字符；allow_zero 为假时 0 也视为非法（用于价格、充值）。 */
int money_parse(const wchar_t* text, int allow_zero, Money* out) {
	const wchar_t* p = text; Money v = 0, cents = 0; int digits = 0, decimal = 0;
	if (!p || !*p || !out)return 0;
	while (*p >= L'0' && *p <= L'9') { v = v * 10 + (*p++ - L'0'); if (v > MONEY_MAX / 100)return 0; ++digits; }
	if (!digits)return 0;
	if (*p == L'.') { ++p; while (*p >= L'0' && *p <= L'9' && decimal < 2) { cents = cents * 10 + (*p++ - L'0'); ++decimal; }if (!decimal)return 0; }
	if (*p)return 0;
	if (decimal == 1)cents *= 10;
	v = v * 100 + cents;
	if (v > MONEY_MAX || (!allow_zero && v == 0))return 0;
	*out = v; return 1;
}
/* 把整数分格式化为“元.两位小数”的展示文本。 */
void money_format(Money cents, wchar_t* out, size_t capacity) {
	if (!capacity)return;
	_snwprintf_s(out, capacity, _TRUNCATE, L"%lld.%02lld", (long long)(cents / 100), (long long)(cents % 100));
}
