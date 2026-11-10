/*
 * netutil.c — libnetutil.so 的實作
 */
#define _GNU_SOURCE
#include "netutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ======================= log ======================= */

static volatile sig_atomic_t g_level = LOG_LV_INFO;
static char g_tag[32] = "app";
static const char *g_lvname[] = { "ERROR", "WARN ", "INFO ", "DEBUG" };

void log_init(const char *tag, int level)
{
    snprintf(g_tag, sizeof g_tag, "%s", tag ? tag : "app");
    log_set_level(level);
}

void log_set_level(int level)
{
    if (level < LOG_LV_ERROR) level = LOG_LV_ERROR;
    if (level > LOG_LV_DEBUG) level = LOG_LV_DEBUG;
    g_level = level;
}

int log_get_level(void) { return g_level; }

int log_level_from_env(int default_level)
{
    const char *s = getenv("NETUTIL_LOG_LEVEL");
    if (!s || !*s) return default_level;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end || v < 0 || v > 3) return default_level;
    return (int)v;
}

int log_debug_compiled(void)
{
#ifdef DEBUG
    return 1;
#else
    return 0;
#endif
}

void log_write(int level, const char *file, int line, const char *fmt, ...)
{
    if (level > g_level) return;           /* 執行期過濾 */

    int saved_errno = errno;               /* 寫 log 不可以弄髒呼叫端的 errno */
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    struct timeval tv;
    struct tm tm;
    char ts[16];
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);

    /* 時間 [程式:pid] 等級 訊息；DEBUG 額外附上 檔名:行號 */
    if (level == LOG_LV_DEBUG)
        fprintf(stderr, "%s.%03ld [%s:%d] %s (%s:%d) %s\n", ts, (long)tv.tv_usec / 1000,
                g_tag, (int)getpid(), g_lvname[level], file, line, msg);
    else
        fprintf(stderr, "%s.%03ld [%s:%d] %s %s\n", ts, (long)tv.tv_usec / 1000,
                g_tag, (int)getpid(), g_lvname[level], msg);

    errno = saved_errno;
}

/* ======================= I/O ======================= */

/* send() 可能只送出一部分，或被 signal 打斷（EINTR），所以要用迴圈送完 */
ssize_t send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    return (ssize_t)len;
}

/* 收滿 len bytes 才回傳。
 * 回傳 len：成功；0：一開始就 EOF（對方正常關閉）；-1：錯誤/逾時/中途斷線 */
ssize_t recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            if (got == 0) return 0;
            errno = ECONNRESET;            /* 訊息收到一半就斷了 */
            return -1;
        }
        got += (size_t)n;
    }
    return (ssize_t)got;
}

/* 標頭和資料組成一個 buffer 一次送出，避免 Nagle + delayed ACK 造成的延遲 */
int proto_send(int fd, uint8_t type, const void *data, uint32_t len)
{
    size_t total = sizeof(proto_hdr_t) + len;
    char stackbuf[sizeof(proto_hdr_t) + PROTO_MAX_PAYLOAD];
    char *pkt = (total <= sizeof stackbuf) ? stackbuf : malloc(total);
    if (!pkt) return -1;

    proto_hdr_t h = { htons(PROTO_MAGIC), type, 0, htonl(len) };
    memcpy(pkt, &h, sizeof h);
    if (len > 0) memcpy(pkt + sizeof h, data, len);

    ssize_t r = send_all(fd, pkt, total);
    if (pkt != stackbuf) free(pkt);
    if (r < 0) return -1;

    LOG_DEBUG("sent %s, payload %u bytes", msg_type_name(type), len);
    return 0;
}

/*
 * 收一個完整封包。
 * validate = 1：檢查 magic 與長度上限（穩健版）
 * validate = 0：完全相信對方送來的 length（不安全版，用來示範 buffer overflow）
 */
int proto_recv(int fd, uint8_t *type, void *buf, size_t bufsize,
               uint32_t *out_len, int validate)
{
    proto_hdr_t h;
    ssize_t r = recv_all(fd, &h, sizeof h);
    if (r == 0) return PROTO_EOF;
    if (r < 0)  return PROTO_IOERR;

    uint32_t len = ntohl(h.length);
    LOG_DEBUG("header: magic=0x%04x type=%s length=%u",
              ntohs(h.magic), msg_type_name(h.type), len);

    if (validate) {
        if (ntohs(h.magic) != PROTO_MAGIC) {
            LOG_WARN("bad magic 0x%04x, not our protocol", ntohs(h.magic));
            return PROTO_BAD;
        }
        if (len > bufsize) {
            LOG_WARN("payload too large: %u > %zu bytes, rejected", len, bufsize);
            return PROTO_BAD;
        }
    }
    /* 不驗證時：len 可能遠大於 buf，recv_all 會寫出界 —— 這就是漏洞 */
    if (len > 0) {
        r = recv_all(fd, buf, len);
        if (r <= 0) {
            if (r == 0) errno = ECONNRESET;
            return PROTO_IOERR;
        }
    }
    *type = h.type;
    *out_len = len;
    return PROTO_OK;
}

const char *msg_type_name(uint8_t t)
{
    switch (t) {
    case MSG_SYSINFO:   return "SYSINFO";
    case MSG_ECHO:      return "ECHO";
    case MSG_TIME:      return "TIME";
    case MSG_PING:      return "PING";
    case MSG_QUIT:      return "QUIT";
    case MSG_MONITOR:   return "MONITOR";
    case MSG_RESP_OK:   return "RESP_OK";
    case MSG_RESP_ERR:  return "RESP_ERR";
    case MSG_RESP_MORE: return "RESP_MORE";
    default:            return "UNKNOWN";
    }
}

/* ================= socket / signal ================= */

int tcp_listen(int port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* SO_REUSEADDR：server 重啟時不會因為舊連線還在 TIME_WAIT 而 bind 失敗 */
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, backlog) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int tcp_connect(const char *host, int port)
{
    struct addrinfo hints, *res, *rp;
    char portstr[16];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%d", port);

    int rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        LOG_ERROR("cannot resolve %s: %s", host, gai_strerror(rc));
        errno = EINVAL;
        return -1;
    }
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        int e = errno;
        close(fd);
        errno = e;
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int set_recv_timeout(int fd, int seconds)
{
    struct timeval tv = { seconds, 0 };
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

int install_signal(int signo, void (*handler)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;
    return sigaction(signo, &sa, NULL);
}
