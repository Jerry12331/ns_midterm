/*
 * client.c — TCP client
 *
 * 一般用法：
 *   ns_client sysinfo              查詢 server 系統資訊
 *   ns_client echo hello world     原樣回傳
 *   ns_client time | ping          server 時間 / 存活檢查
 *   ns_client monitor 5            每秒推送 CPU/記憶體使用率 5 次
 *   ns_client                      不給指令 → 互動模式
 *
 * 測試異常狀況用（搭配 server 的 -D 做有/無穩健性對照）：
 *   --idle SEC        連上後什麼都不送，看 server 會不會把閒置連線踢掉
 *   --oversize N      送出 length = N 的超大封包
 *   --badmagic        送出不是本協定的資料（像 HTTP 請求）
 *   --abort-monitor   開始 monitor 後收到第一筆就直接斷線
 *
 * client 自己的穩健性：連線失敗會重試（指數退避）、等待回應有逾時、忽略 SIGPIPE。
 */
#define _GNU_SOURCE
#include "netutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *g_host = "127.0.0.1";
static int g_port = DEFAULT_PORT;
static int g_timeout = 5;      /* 等待回應的秒數 */
static int g_retry = 3;        /* 連線重試次數 */
static int g_keep = 0;         /* 完成後保持連線幾秒（concurrency 測試用） */
static int g_repeat = 1;       /* 同一連線重複送幾次 */

static char g_buf[PROTO_MAX_PAYLOAD + 1];

static int connect_with_retry(void)
{
    for (int i = 0; i <= g_retry; i++) {
        int fd = tcp_connect(g_host, g_port);
        if (fd >= 0) {
            LOG_DEBUG("connected to %s:%d (fd %d)", g_host, g_port, fd);
            return fd;
        }
        if (i < g_retry) {
            int wait = 1 << i;                       /* 1, 2, 4 秒 */
            LOG_WARN("connect %s:%d failed: %s, retry in %d s (%d/%d)",
                     g_host, g_port, strerror(errno), wait, i + 1, g_retry);
            sleep((unsigned)wait);
        } else {
            LOG_ERROR("connect %s:%d failed: %s, giving up", g_host, g_port, strerror(errno));
        }
    }
    return -1;
}

/* 收回應直到最後一筆（MSG_RESP_MORE 表示後面還有）
 * 回傳 0：成功；-1：server 回錯誤訊息（連線還在）；-2：連線層錯誤（斷線/逾時/格式錯） */
static int read_reply(int fd)
{
    for (;;) {
        uint8_t type;
        uint32_t len;
        int r = proto_recv(fd, &type, g_buf, PROTO_MAX_PAYLOAD, &len, 1);
        if (r == PROTO_EOF) {
            LOG_ERROR("server closed the connection");
            return -2;
        }
        if (r == PROTO_IOERR) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                LOG_ERROR("no response within %d s (timeout)", g_timeout);
            else
                LOG_ERROR("recv: %s", strerror(errno));
            return -2;
        }
        if (r == PROTO_BAD) {
            LOG_ERROR("malformed packet from server");
            return -2;
        }
        g_buf[len] = '\0';
        if (type == MSG_RESP_ERR) {
            printf("[server error] %s\n", g_buf);
            fflush(stdout);
            return -1;
        }
        printf("%s\n", g_buf);
        fflush(stdout);
        if (type != MSG_RESP_MORE) return 0;
    }
}

static int request(int fd, uint8_t type, const char *payload)
{
    uint32_t len = payload ? (uint32_t)strlen(payload) : 0;
    if (len > PROTO_MAX_PAYLOAD) {
        LOG_ERROR("payload too long (%u > %d)", len, PROTO_MAX_PAYLOAD);
        return -1;
    }
    if (proto_send(fd, type, payload, len) < 0) {
        LOG_ERROR("send: %s", strerror(errno));
        return -2;
    }
    return read_reply(fd);
}

/* 指令字串 → 訊息種類 */
static int parse_cmd(const char *cmd)
{
    if (!strcmp(cmd, "sysinfo")) return MSG_SYSINFO;
    if (!strcmp(cmd, "echo"))    return MSG_ECHO;
    if (!strcmp(cmd, "time"))    return MSG_TIME;
    if (!strcmp(cmd, "ping"))    return MSG_PING;
    if (!strcmp(cmd, "monitor")) return MSG_MONITOR;
    if (!strcmp(cmd, "quit"))    return MSG_QUIT;
    return -1;
}

static void interactive(int fd)
{
    char line[PROTO_MAX_PAYLOAD];
    printf("connected to %s:%d — 指令: sysinfo | echo <文字> | time | ping | monitor <N> | quit\n",
           g_host, g_port);
    for (;;) {
        printf("ns> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        line[strcspn(line, "\n")] = '\0';
        if (!*line) continue;

        char *arg = strchr(line, ' ');
        if (arg) *arg++ = '\0';
        int type = parse_cmd(line);
        if (type < 0) {
            printf("未知指令: %s\n", line);
            continue;
        }
        /* server 回錯誤訊息（-1）可以繼續；連線斷了（-2）就離開 */
        if (request(fd, (uint8_t)type, arg) == -2) {
            printf("連線已中斷\n");
            break;
        }
        if (type == MSG_QUIT) break;
    }
}

/* ---------------- 異常測試模式 ---------------- */

static int test_idle(int fd, int sec)
{
    LOG_INFO("connected, now sending NOTHING for up to %d s ...", sec);
    set_recv_timeout(fd, sec);
    uint8_t type;
    uint32_t len;
    int r = proto_recv(fd, &type, g_buf, PROTO_MAX_PAYLOAD, &len, 1);
    if (r == PROTO_OK) {
        g_buf[len] = '\0';
        printf("server 主動回應: [%s] %s\n", msg_type_name(type), g_buf);
    } else if (r == PROTO_EOF) {
        printf("server 關閉了閒置連線\n");
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        printf("過了 %d 秒 server 還佔著這個連線（沒有閒置逾時機制，child 會一直卡住）\n", sec);
    } else {
        printf("recv: %s\n", strerror(errno));
    }
    return 0;
}

static int test_oversize(int fd, uint32_t n)
{
    LOG_INFO("sending ECHO with length=%u (server buffer is %d)", n, PROTO_MAX_PAYLOAD);
    char *data = malloc(n);
    if (!data) return -1;
    memset(data, 'A', n);
    proto_hdr_t h = { htons(PROTO_MAGIC), MSG_ECHO, 0, htonl(n) };
    if (send_all(fd, &h, sizeof h) < 0 || send_all(fd, data, n) < 0) {
        printf("送出失敗: %s\n", strerror(errno));
        free(data);
        return -1;
    }
    free(data);

    uint8_t type;
    uint32_t len;
    static char big[1 << 16];
    int r = proto_recv(fd, &type, big, sizeof big, &len, 1);
    if (r == PROTO_OK && type == MSG_RESP_ERR) {
        big[len] = '\0';
        printf("server 拒絕了超大封包: %s\n", big);
    } else if (r == PROTO_OK) {
        printf("server 竟然接受了 %u bytes 的封包並回傳 %u bytes（buffer 已經溢位！）\n", n, len);
    } else {
        printf("沒有正常回應（server child 可能已經崩潰）\n");
    }
    return 0;
}

static int test_badmagic(int fd)
{
    const char *junk = "GET / HTTP/1.0\r\n\r\n";
    LOG_INFO("sending non-protocol data: %s", "GET / HTTP/1.0");
    send_all(fd, junk, strlen(junk));
    return read_reply(fd);
}

static int test_abort_monitor(int fd)
{
    LOG_INFO("start monitor 10, then disconnect after the first reply");
    proto_send(fd, MSG_MONITOR, "10", 2);
    uint8_t type;
    uint32_t len;
    if (proto_recv(fd, &type, g_buf, PROTO_MAX_PAYLOAD, &len, 1) == PROTO_OK) {
        g_buf[len] = '\0';
        printf("%s\n", g_buf);
    }
    printf("client 直接斷線（server 還會繼續推送）\n");
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s [選項] [指令 [參數...]]\n"
        "指令: sysinfo | echo <文字> | time | ping | monitor <N> | quit（不給指令進入互動模式）\n"
        "選項:\n"
        "  -H HOST   server 位址（預設 127.0.0.1）\n"
        "  -p PORT   server port（預設 %d）\n"
        "  -v LEVEL  log level 0~3（也可用 NETUTIL_LOG_LEVEL）\n"
        "  -t SEC    等待回應逾時（預設 %d）\n"
        "  -r N      連線失敗重試次數（預設 %d）\n"
        "  -n N      同一連線重複送 N 次\n"
        "  -k SEC    完成後保持連線 SEC 秒（concurrency 測試用）\n"
        "異常測試: --idle SEC | --oversize N | --badmagic | --abort-monitor\n",
        prog, DEFAULT_PORT, g_timeout, g_retry);
}

int main(int argc, char **argv)
{
    int level = log_level_from_env(LOG_LV_INFO);
    int idle = 0, badmagic = 0, abort_mon = 0;
    long oversize = 0;

    static struct option longopts[] = {
        { "idle",          required_argument, 0, 'I' },
        { "oversize",      required_argument, 0, 'O' },
        { "badmagic",      no_argument,       0, 'B' },
        { "abort-monitor", no_argument,       0, 'A' },
        { "help",          no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "+H:p:v:t:r:n:k:h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'H': g_host = optarg; break;
        case 'p': g_port = atoi(optarg); break;
        case 'v': level = atoi(optarg); break;
        case 't': g_timeout = atoi(optarg); break;
        case 'r': g_retry = atoi(optarg); break;
        case 'n': g_repeat = atoi(optarg); break;
        case 'k': g_keep = atoi(optarg); break;
        case 'I': idle = atoi(optarg); break;
        case 'O': oversize = atol(optarg); break;
        case 'B': badmagic = 1; break;
        case 'A': abort_mon = 1; break;
        default:  usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    log_init("client", level);
    if (level >= LOG_LV_DEBUG && !log_debug_compiled())
        LOG_WARN("要求 DEBUG 等級，但這次編譯沒有開 DEBUG；請用 make DEBUG=1 重新編譯");

    signal(SIGPIPE, SIG_IGN);   /* server 斷線時 send() 回傳錯誤，而不是讓 client 被殺掉 */

    int fd = connect_with_retry();
    if (fd < 0) return 1;
    set_recv_timeout(fd, g_timeout);

    int rc = 0;
    if (idle > 0)            rc = test_idle(fd, idle);
    else if (oversize > 0)   rc = test_oversize(fd, (uint32_t)oversize);
    else if (badmagic)       rc = test_badmagic(fd);
    else if (abort_mon)      rc = test_abort_monitor(fd);
    else if (optind >= argc) interactive(fd);
    else {
        int type = parse_cmd(argv[optind]);
        if (type < 0) {
            fprintf(stderr, "未知指令: %s\n", argv[optind]);
            close(fd);
            return 2;
        }
        /* 剩下的參數用空白接起來當 payload */
        char payload[PROTO_MAX_PAYLOAD] = "";
        size_t used = 0;
        for (int i = optind + 1; i < argc; i++) {
            int w = snprintf(payload + used, sizeof payload - used, "%s%s",
                             i > optind + 1 ? " " : "", argv[i]);
            if (w < 0 || (size_t)w >= sizeof payload - used) break;
            used += (size_t)w;
        }
        for (int i = 0; i < g_repeat && rc == 0; i++)
            rc = request(fd, (uint8_t)type, used ? payload : NULL);

        if (rc == 0 && g_keep > 0) {
            LOG_INFO("keeping connection open for %d s", g_keep);
            sleep((unsigned)g_keep);
            request(fd, MSG_QUIT, NULL);
        }
    }
    close(fd);
    return rc < 0 ? 1 : 0;
}
