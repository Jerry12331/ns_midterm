# 網路系統程式期中專題：fork-based Client–Server

C 語言實作的多行程 TCP client–server。client 可以向 server 查詢系統資訊（CPU、記憶體、負載、uptime 等），另外支援 echo、time、ping、monitor（即時推送使用率）。

## 環境準備（WSL Ubuntu）

```bash
sudo apt update
sudo apt install -y build-essential tcpdump iproute2
```

## 目錄結構

```
include/netutil.h     共用函式庫標頭：通訊協定、log、I/O、socket/signal 工具
lib/netutil.c         共用函式庫實作 → 編成 build/libnetutil.so
src/server.c          server：accept 迴圈 + 每個連線 fork() 一個 child
src/client.c          client：一般指令、互動模式、異常測試模式
scripts/              並行測試與四個穩健性對照示範
Makefile
```

## 建置

```bash
make            # 一般版：LOG_DEBUG 完全不會編進程式
make DEBUG=1    # 除錯版：編譯期開啟 DEBUG（加 -g -O0）
make clean
```

server 和 client 都連結 `build/libnetutil.so`，可用 `ldd build/ns_server` 確認。Makefile 設了 `rpath=$ORIGIN`，所以執行時不用設定 `LD_LIBRARY_PATH`。

## 執行

```bash
./build/ns_server                       # 預設 port 8888
./build/ns_client sysinfo               # 查詢系統資訊
./build/ns_client echo hello world
./build/ns_client monitor 5             # 每秒推送一次 CPU/記憶體使用率，共 5 次
./build/ns_client                       # 互動模式
./build/ns_client -H 192.168.1.10 time  # 連到別台電腦的 server
```

## 通訊協定

每個封包 = 8 bytes 標頭 + payload：`magic(2) | type(1) | reserved(1) | length(4)`，多位元組欄位一律 network byte order。有長度欄位，接收端才能在 TCP 位元組串流中切出完整訊息。

## 兩層 debug log

| 層級 | 方式 | 作用 |
|---|---|---|
| 編譯期 | `make DEBUG=1` | 決定 `LOG_DEBUG(...)` 是否被編進程式；沒開時被巨集換成 `((void)0)`，零成本 |
| 執行期 | `-v 0~3`、環境變數 `NETUTIL_LOG_LEVEL`、`kill -USR1 <server pid>` | 決定實際印出 ERROR / WARN / INFO / DEBUG 哪些等級 |

用一般版執行 `-v 3` 時，程式會提醒要用 `make DEBUG=1` 重編，才看得到 DEBUG 訊息。

## 並行測試（10 個以上 client）

```bash
./build/ns_server                       # 終端機 1
./scripts/concurrency_test.sh 12 6      # 終端機 2：12 個 client，各保持連線 6 秒
```

腳本會印出 server 的行程樹（1 個 parent + 12 個 child 同時存在）、child 數量、ESTABLISHED 連線數，最後列出每個 client 分別由哪個 child pid 服務。

## 穩健性機制與對照示範

server 預設全部開啟，用 `-D` 可個別關閉做對照。每個示範腳本會分別跑「關閉」和「開啟」兩次（使用 port 9999）。

| 腳本 | 異常情境 | 沒有機制 | 有機制 |
|---|---|---|---|
| `demo1_zombie.sh` | child 結束後沒人回收 | `ps` 看到一堆 `Z <defunct>` 殭屍行程 | SIGCHLD + `waitpid(WNOHANG)` 即時回收，log 記錄結束狀態 |
| `demo2_idle_timeout.sh` | client 連上後什麼都不送 | child 永遠卡在 `recv()`，資源被佔住 | `SO_RCVTIMEO` 逾時，主動通知並斷線 |
| `demo3_oversize.sh` | 宣稱 8192 bytes 的封包，buffer 只有 4096 | stack buffer overflow，child 被 `stack smashing detected` 殺掉（SIGABRT） | 驗證 magic 與長度，回覆 `bad packet` |
| `demo4_sigpipe.sh` | client 中途斷線，server 還在寫 | child 被 SIGPIPE（signal 13）殺掉 | 忽略 SIGPIPE、檢查 `send()` 回傳值，記錄後正常結束 |

其他機制：同時連線上限（`-m`，超過回覆 `server busy`）、`accept()`/`recv()`/`send()` 的 EINTR 處理、部分傳送處理（`send_all`/`recv_all`）、fd 用完（EMFILE）時暫停、`SO_REUSEADDR`、SIGINT/SIGTERM 優雅關閉；client 端有連線重試（指數退避）、回應逾時、忽略 SIGPIPE。

另外可以觀察到 fork 架構本身的隔離性：示範 3、4 裡 child 崩潰，parent 和其他 client 都不受影響。

## 封包擷取（tcpdump）

```bash
sudo tcpdump -i lo -nn -S 'tcp port 8888' -w capture.pcap   # 終端機 A
./build/ns_client ping                                        # 終端機 B
sudo tcpdump -nn -X -r capture.pcap                           # 分析
```

可以看到：`[S]`、`[S.]`、`[.]` 三向交握；client 送出 8 bytes 的 PING 標頭（`4e53 0400 0000 0000`）；server 回 25 bytes（8 bytes 標頭 + 17 bytes `PONG from pid ...`）；`[F.]` 雙方關閉連線。`capture.pcap` 也可以複製到 Windows 用 Wireshark 開啟。兩台電腦之間測試時，server 端改用 `-i eth0` 或直接在 Windows 上用 Wireshark 抓。
