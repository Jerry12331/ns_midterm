#!/usr/bin/env bash
# 示範腳本共用的小函式
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
BIN=./build
PORT=${PORT:-9999}
SLOG=/tmp/ns_demo_server.log

[ -x $BIN/ns_server ] || { echo "請先執行 make"; exit 1; }

banner()  { echo; echo "================ $* ================"; }
start_server() {
    $BIN/ns_server -p "$PORT" "$@" > "$SLOG" 2>&1 &
    SRV=$!
    sleep 0.5
    echo "\$ ns_server -p $PORT $*   (pid $SRV)"
}
show_children() {
    echo "--- ns_server (pid $SRV) 的 child 行程（STAT 有 Z = 殭屍）---"
    ps -o pid,ppid,stat,etime,args --ppid "$SRV" || echo "  (沒有 child)"
}
stop_server() {
    local children
    children=$(pgrep -P "$SRV")
    kill "$SRV" 2>/dev/null
    # 最多等 3 秒讓 server 自己結束，還在就強制終止，避免腳本卡住
    for _ in $(seq 1 30); do kill -0 "$SRV" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$SRV" 2>/dev/null
    wait "$SRV" 2>/dev/null
    [ -n "$children" ] && kill $children 2>/dev/null   # 清掉殘留的 child
    sleep 0.3
    echo "--- server log ---"; cat "$SLOG"
}
