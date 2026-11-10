/*
 * server.c — 多行程（fork）TCP server
 *
 * 架構：
 *   parent：listen → accept 迴圈 → 每來一個連線就 fork() 一個 child
 *   child ：只負責服務那一個 client，服務完 _exit()
 *   parent 透過 SIGCHLD + waitpid() 回收結束的 child，避免殭屍行程
 *
 * 穩健性機制（可用 -D 個別關閉，用來做「有/無」對照示範）：
 *   zombie   : SIGCHLD + waitpid(WNOHANG) 回收 child
 *   timeout  : SO_RCVTIMEO，client 閒置太久就斷線，避免 child 永遠卡住
 *   validate : 檢查封包 magic 與長度，避免 buffer overflow
 *   sigpipe  : 忽略 SIGPIPE 並檢查 send() 回傳值，client 斷線時不會被系統殺掉
 *   limit    : 同時連線數上限，避免被大量連線耗盡資源（fork bomb）
 */
#define _GNU_SOURCE
#include "netutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---------- 設定 ---------- */
static struct {
    int zombie, timeout, validate, sigpipe, limit;
} g_rb = { 1, 1, 1, 1, 1 };

static int g_port = DEFAULT_PORT;
static int g_idle_timeout = 10;   /* 秒 */
static int g_max_clients = 64;

/* ---------- 狀態 ---------- */
static volatile sig_atomic_t g_stop = 0;       /* 收到 SIGINT/SIGTERM */
static volatile sig_atomic_t g_level_bump = 0; /* 收到 SIGUSR1 */
static int g_active = 0;                       /* 目前存活的 child 數 */
static unsigned long g_total = 0;              /* 總共服務過幾個連線 */
static char g_out[8192];                       /* child 組回應用的 buffer */
static char g_peer_ip[64];                     /* child 服務的 client 位址（放全域，不在 stack 上） */

/* signal handler 只設旗標，真正的工作回到主迴圈做（handler 裡不能呼叫 printf 等函式） */
static void on_sigchld(int s) { (void)s; }      /* 目的只是讓 accept() 被 EINTR 打斷 */
static void on_stop(int s)    { (void)s; g_stop = 1; }
static void on_usr1(int s)    { (void)s; g_level_bump = 1; }

/* ======================= 系統資訊 ======================= */

static int read_cpu(unsigned long long *idle, unsigned long long *total)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    unsigned long long v[8] = { 0 };
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4) return -1;
    *idle = v[3] + v[4];                         /* idle + iowait */
    *total = 0;
    for (int i = 0; i < 8; i++) *total += v[i];
    return 0;
}

/* /proc/stat 是開機以來的累計值，要取兩次相減才是「這段時間」的使用率 */
static double sample_cpu_usage(void)
{
    unsigned long long i1, t1, i2, t2;
    if (read_cpu(&i1, &t1) < 0) return -1;
    usleep(200 * 1000);
    if (read_cpu(&i2, &t2) < 0 || t2 <= t1) return -1;
    return 100.0 * (1.0 - (double)(i2 - i1) / (double)(t2 - t1));
}

static void read_meminfo(long *total_kb, long *avail_kb)
{
    *total_kb = *avail_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        sscanf(line, "MemTotal: %ld kB", total_kb);
        sscanf(line, "MemAvailable: %ld kB", avail_kb);
    }
    fclose(f);
}

static void read_cpu_model(char *out, size_t n)
{
    snprintf(out, n, "unknown");
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "model name", 10) == 0) {
            char *c = strchr(line, ':');
            if (c) {
                c++;
                while (*c == ' ') c++;
                c[strcspn(c, "\n")] = '\0';
                snprintf(out, n, "%s", c);
            }
            break;
        }
    }
    fclose(f);
}

static double mem_usage_percent(void)
{
    long mt, ma;
    read_meminfo(&mt, &ma);
    return mt > 0 ? 100.0 * (double)(mt - ma) / (double)mt : -1;
}

static int build_sysinfo(char *out, size_t n)
{
    char host[256] = "?", model[128];
    gethostname(host, sizeof host);
    host[sizeof host - 1] = '\0';

    struct utsname u;
    uname(&u);
    struct sysinfo si;
    sysinfo(&si);

    long mt, ma;
    read_meminfo(&mt, &ma);
    read_cpu_model(model, sizeof model);
    double cpu = sample_cpu_usage();

    long up = si.uptime;
    return snprintf(out, n,
        "Hostname     : %s\n"
        "OS / Kernel  : %s %s (%s)\n"
        "CPU model    : %s\n"
        "CPU cores    : %d\n"
        "CPU usage    : %.1f %%\n"
        "Load average : %.2f %.2f %.2f\n"
        "Memory       : %.1f / %.1f MB used (%.1f %%)\n"
        "Uptime       : %ldd %02ldh %02ldm %02lds\n"
        "Processes    : %u\n"
        "Served by    : child pid %d (parent pid %d)",
        host, u.sysname, u.release, u.machine, model, get_nprocs(), cpu,
        si.loads[0] / 65536.0, si.loads[1] / 65536.0, si.loads[2] / 65536.0,
        (mt - ma) / 1024.0, mt / 1024.0, mt > 0 ? 100.0 * (mt - ma) / mt : 0.0,
        up / 86400, (up % 86400) / 3600, (up % 3600) / 60, up % 60,
        si.procs, (int)getpid(), (int)getppid());
}

/* ======================= child：服務一個 client ======================= */

static int handle_monitor(int fd, const char *arg)
{
    int count = atoi(arg);
    if (count <= 0 || count > 60) count = 5;
    int rc = 0;

    for (int k = 1; k <= count; k++) {
        int m = snprintf(g_out, sizeof g_out, "[%d/%d] CPU %5.1f %%   MEM %5.1f %%",
                         k, count, sample_cpu_usage(), mem_usage_percent());
        rc = proto_send(fd, k == count ? MSG_RESP_OK : MSG_RESP_MORE, g_out, (uint32_t)m);
        if (rc < 0) {
            if (g_rb.sigpipe) {
                /* 穩健版：發現 client 已經不在，就停止推送 */
                LOG_WARN("monitor: client gone (%s), stop streaming at %d/%d",
                         strerror(errno), k, count);
                return -1;
            }
            /* 不穩健版：不檢查錯誤繼續寫，下一次 send() 就會觸發 SIGPIPE */
        }
        if (k < count) sleep(1);
    }
    return rc;
}

static void handle_client(int fd, const char *ip, int port)
{
    char buf[PROTO_MAX_PAYLOAD + 1];

    if (g_rb.timeout) set_recv_timeout(fd, g_idle_timeout);

    for (;;) {
        uint8_t type = 0;
        uint32_t len = 0;
        int r = proto_recv(fd, &type, buf, PROTO_MAX_PAYLOAD, &len, g_rb.validate);

        if (r == PROTO_EOF) {
            LOG_INFO("%s:%d closed the connection", ip, port);
            break;
        }
        if (r == PROTO_IOERR) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_WARN("%s:%d idle for %d s, disconnecting", ip, port, g_idle_timeout);
                const char *m = "idle timeout, bye";
                proto_send(fd, MSG_RESP_ERR, m, (uint32_t)strlen(m));
            } else {
                LOG_WARN("%s:%d recv error: %s", ip, port, strerror(errno));
            }
            break;
        }
        if (r == PROTO_BAD) {
            const char *m = "bad packet";
            proto_send(fd, MSG_RESP_ERR, m, (uint32_t)strlen(m));
            break;
        }
        if (len < sizeof buf) buf[len] = '\0';

        LOG_INFO("%s:%d request %s (%u bytes)", ip, port, msg_type_name(type), len);
        int rc = 0, quit = 0, m;
        time_t now;

        switch (type) {
        case MSG_SYSINFO:
            m = build_sysinfo(g_out, sizeof g_out);
            rc = proto_send(fd, MSG_RESP_OK, g_out, (uint32_t)m);
            break;
        case MSG_ECHO:
            rc = proto_send(fd, MSG_RESP_OK, buf, len);
            break;
        case MSG_TIME:
            now = time(NULL);
            m = (int)strftime(g_out, sizeof g_out, "%Y-%m-%d %H:%M:%S %Z", localtime(&now));
            rc = proto_send(fd, MSG_RESP_OK, g_out, (uint32_t)m);
            break;
        case MSG_PING:
            m = snprintf(g_out, sizeof g_out, "PONG from pid %d", (int)getpid());
            rc = proto_send(fd, MSG_RESP_OK, g_out, (uint32_t)m);
            break;
        case MSG_MONITOR:
            rc = handle_monitor(fd, buf);
            break;
        case MSG_QUIT:
            rc = proto_send(fd, MSG_RESP_OK, "bye", 3);
            quit = 1;
            break;
        default:
            LOG_WARN("%s:%d unknown message type 0x%02x", ip, port, type);
            rc = proto_send(fd, MSG_RESP_ERR, "unknown command", 15);
            break;
        }
        if (rc < 0 && g_rb.sigpipe) {
            LOG_WARN("%s:%d send failed: %s", ip, port, strerror(errno));
            break;
        }
        if (quit) break;
    }
}

/* ======================= parent ======================= */

static void reap_children(void)
{
    int st;
    pid_t p;
    while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
        g_active--;
        if (WIFEXITED(st))
            LOG_INFO("child %d exited (status %d), active=%d", (int)p, WEXITSTATUS(st), g_active);
        else if (WIFSIGNALED(st))
            LOG_WARN("child %d KILLED by signal %d (%s), active=%d",
                     (int)p, WTERMSIG(st), strsignal(WTERMSIG(st)), g_active);
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s [選項]\n"
        "  -p PORT     監聽 port（預設 %d）\n"
        "  -v LEVEL    log level 0=ERROR 1=WARN 2=INFO 3=DEBUG（預設 2，也可用 NETUTIL_LOG_LEVEL）\n"
        "  -t SEC      client 閒置逾時秒數（預設 %d）\n"
        "  -m N        同時連線上限（預設 %d）\n"
        "  -D LIST     關閉穩健性機制，逗號分隔：zombie,timeout,validate,sigpipe,limit,all\n"
        "  -h          顯示說明\n"
        "執行中切換 log level：kill -USR1 <server pid>\n",
        prog, DEFAULT_PORT, g_idle_timeout, g_max_clients);
}

static void disable_mechanisms(char *list)
{
    for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) {
        if (!strcmp(t, "zombie"))        g_rb.zombie = 0;
        else if (!strcmp(t, "timeout"))  g_rb.timeout = 0;
        else if (!strcmp(t, "validate")) g_rb.validate = 0;
        else if (!strcmp(t, "sigpipe"))  g_rb.sigpipe = 0;
        else if (!strcmp(t, "limit"))    g_rb.limit = 0;
        else if (!strcmp(t, "all"))      memset(&g_rb, 0, sizeof g_rb);
        else { fprintf(stderr, "未知的機制: %s\n", t); exit(2); }
    }
}

int main(int argc, char **argv)
{
    int level = log_level_from_env(LOG_LV_INFO);
    int opt;
    while ((opt = getopt(argc, argv, "p:v:t:m:D:h")) != -1) {
        switch (opt) {
        case 'p': g_port = atoi(optarg); break;
        case 'v': level = atoi(optarg); break;
        case 't': g_idle_timeout = atoi(optarg); break;
        case 'm': g_max_clients = atoi(optarg); break;
        case 'D': disable_mechanisms(optarg); break;
        default:  usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    log_init("server", level);
    if (level >= LOG_LV_DEBUG && !log_debug_compiled())
        LOG_WARN("要求 DEBUG 等級，但這次編譯沒有開 DEBUG；請用 make DEBUG=1 重新編譯");

    LOG_INFO("robustness: zombie=%s timeout=%s validate=%s sigpipe=%s limit=%s",
             g_rb.zombie ? "ON" : "OFF", g_rb.timeout ? "ON" : "OFF",
             g_rb.validate ? "ON" : "OFF", g_rb.sigpipe ? "ON" : "OFF",
             g_rb.limit ? "ON" : "OFF");

    /* --- signal 設定 --- */
    if (g_rb.sigpipe) signal(SIGPIPE, SIG_IGN);       /* 寫到已關閉的 socket 改成回傳 EPIPE */
    if (g_rb.zombie)  install_signal(SIGCHLD, on_sigchld, 0); /* 不加 SA_RESTART：讓 accept 被打斷以即時回收 */
    install_signal(SIGINT, on_stop, 0);
    install_signal(SIGTERM, on_stop, 0);
    install_signal(SIGUSR1, on_usr1, 0);

    int lfd = tcp_listen(g_port, 128);
    if (lfd < 0) {
        LOG_ERROR("cannot listen on port %d: %s", g_port, strerror(errno));
        return 1;
    }
    LOG_INFO("listening on 0.0.0.0:%d (parent pid %d)", g_port, (int)getpid());

    while (!g_stop) {
        if (g_rb.zombie) reap_children();
        if (g_level_bump) {
            g_level_bump = 0;
            log_set_level((log_get_level() + 1) % 4);
            LOG_WARN("log level changed to %d", log_get_level());
        }

        /*
         * 先用 poll() 等待「最多 1 秒」，而不是直接卡在 accept()。
         * 原因：signal 可能剛好在「檢查 g_stop 之後、呼叫 accept() 之前」抵達，
         * 這時 accept() 不會被打斷，server 就永遠收不到關閉通知（race condition）。
         * 有 1 秒逾時，最晚 1 秒後就會回到迴圈頂端重新檢查旗標、回收 child。
         */
        struct pollfd pfd = { .fd = lfd, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);
        if (pr < 0) {
            if (errno != EINTR) LOG_ERROR("poll: %s", strerror(errno));
            continue;
        }
        if (pr == 0) continue;                       /* 逾時：沒有新連線 */

        struct sockaddr_in cli;
        socklen_t clen = sizeof cli;
        int cfd = accept(lfd, (struct sockaddr *)&cli, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;            /* 被 signal 打斷，不是錯誤 */
            LOG_ERROR("accept: %s", strerror(errno));
            if (errno == EMFILE || errno == ENFILE) sleep(1); /* fd 用完了，稍等再試 */
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof ip);
        int port = ntohs(cli.sin_port);

        if (g_rb.limit && g_active >= g_max_clients) {
            LOG_WARN("too many clients (%d), rejecting %s:%d", g_active, ip, port);
            proto_send(cfd, MSG_RESP_ERR, "server busy, try later", 22);
            close(cfd);
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            LOG_ERROR("fork: %s", strerror(errno));
            proto_send(cfd, MSG_RESP_ERR, "server busy, try later", 22);
            close(cfd);
            continue;
        }
        if (pid == 0) {
            /* ---- child ---- */
            close(lfd);                              /* child 不需要 listening socket */
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGUSR1, SIG_IGN);
            snprintf(g_peer_ip, sizeof g_peer_ip, "%s", ip);
            LOG_DEBUG("child started for %s:%d", g_peer_ip, port);
            handle_client(cfd, g_peer_ip, port);
            close(cfd);
            _exit(0);
        }
        /* ---- parent ---- */
        close(cfd);                                  /* parent 不處理這個連線，要關掉自己那份 */
        g_active++;
        g_total++;
        LOG_INFO("accepted %s:%d -> child pid %d (active=%d)", ip, port, (int)pid, g_active);
    }

    LOG_INFO("shutting down: served %lu connections, %d still active", g_total, g_active);
    close(lfd);
    if (g_rb.zombie) reap_children();
    return 0;
}
