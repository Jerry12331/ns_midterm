/*
 * netutil.h — client 與 server 共用的工具函式庫（編譯成 libnetutil.so）
 *
 * 內容分成四塊：
 *   1. 通訊協定定義（封包格式、訊息種類）
 *   2. 兩層 debug log（編譯期 DEBUG 巨集 + 執行期 log level）
 *   3. 可靠的 I/O（send_all / recv_all / proto_send / proto_recv）
 *   4. socket 與 signal 小工具
 */
#ifndef NETUTIL_H
#define NETUTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ---------------- 1. 通訊協定 ----------------
 * 每個封包 = 8 bytes 固定標頭 + length bytes 資料
 *
 *   0       2       3       4               8
 *   +-------+-------+-------+---------------+-----------------+
 *   | magic | type  | rsvd  |    length     |  payload ...    |
 *   +-------+-------+-------+---------------+-----------------+
 *   (multi-byte 欄位一律 network byte order)
 *
 * 有長度欄位，接收端才知道「一個訊息到哪裡結束」，
 * 解決 TCP 是位元組串流、沒有訊息邊界（黏包/拆包）的問題。
 */
#define PROTO_MAGIC       0x4E53      /* 'N''S' */
#define PROTO_MAX_PAYLOAD 4096
#define DEFAULT_PORT      8888

enum msg_type {
    MSG_SYSINFO   = 0x01,   /* 查詢 server 系統資訊 */
    MSG_ECHO      = 0x02,   /* 原樣回傳 */
    MSG_TIME      = 0x03,   /* server 目前時間 */
    MSG_PING      = 0x04,   /* 存活檢查 */
    MSG_QUIT      = 0x05,   /* 結束連線 */
    MSG_MONITOR   = 0x06,   /* 每秒推送 CPU/記憶體使用率 N 次 */
    MSG_RESP_OK   = 0x80,   /* 回應：成功（最後一筆） */
    MSG_RESP_ERR  = 0x81,   /* 回應：錯誤 */
    MSG_RESP_MORE = 0x82    /* 回應：串流中，後面還有 */
};

typedef struct {
    uint16_t magic;
    uint8_t  type;
    uint8_t  reserved;
    uint32_t length;
} __attribute__((packed)) proto_hdr_t;

/* proto_recv() 的回傳值 */
#define PROTO_OK     1
#define PROTO_EOF    0    /* 對方正常關閉連線 */
#define PROTO_IOERR -1    /* I/O 錯誤或逾時（看 errno） */
#define PROTO_BAD   -2    /* 封包格式不合法 */

/* ---------------- 2. 兩層 debug log ----------------
 * 編譯期：只有 `make DEBUG=1` 時 LOG_DEBUG 才會被編進程式，
 *         否則整行被前處理器換成 ((void)0)，完全沒有執行成本。
 * 執行期：log level 0~3 決定實際印出哪些等級，
 *         可用命令列 -v N、環境變數 NETUTIL_LOG_LEVEL=N 設定，
 *         server 另外可以 `kill -USR1 <pid>` 在執行中循環切換。
 */
enum log_level { LOG_LV_ERROR = 0, LOG_LV_WARN = 1, LOG_LV_INFO = 2, LOG_LV_DEBUG = 3 };

void log_init(const char *tag, int level);
void log_set_level(int level);
int  log_get_level(void);
int  log_level_from_env(int default_level);
int  log_debug_compiled(void);          /* 這個 .so 編譯時是否有開 DEBUG */
void log_write(int level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define LOG_ERROR(...) log_write(LOG_LV_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_LV_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  log_write(LOG_LV_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#ifdef DEBUG
#define LOG_DEBUG(...) log_write(LOG_LV_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#else
#define LOG_DEBUG(...) ((void)0)
#endif

/* ---------------- 3. 可靠的 I/O ---------------- */
ssize_t send_all(int fd, const void *buf, size_t len);
ssize_t recv_all(int fd, void *buf, size_t len);
int proto_send(int fd, uint8_t type, const void *data, uint32_t len);
int proto_recv(int fd, uint8_t *type, void *buf, size_t bufsize,
               uint32_t *out_len, int validate);
const char *msg_type_name(uint8_t type);

/* ---------------- 4. socket / signal 小工具 ---------------- */
int tcp_listen(int port, int backlog);
int tcp_connect(const char *host, int port);
int set_recv_timeout(int fd, int seconds);
int install_signal(int signo, void (*handler)(int), int flags);

#endif /* NETUTIL_H */
