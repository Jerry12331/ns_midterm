#!/usr/bin/env bash
# 並行測試：同時啟動 N 個 client（預設 12），每個連線保持 KEEP 秒（預設 6），
# 期間印出 server 行程樹與 TCP 連線數，證明 server 同時服務多個 client。
# 用法：先在另一個終端機執行 ./build/ns_server，再執行 ./scripts/concurrency_test.sh [N] [KEEP]
cd "$(dirname "$0")/.." || exit 1
N=${1:-12}; KEEP=${2:-6}; PORT=${PORT:-8888}
PARENT=$(pgrep -o -x ns_server) || { echo "請先啟動 ./build/ns_server -p $PORT"; exit 1; }
OUT=/tmp/ns_conc; rm -rf $OUT; mkdir -p $OUT

echo "[$(date +%T)] 同時啟動 $N 個 client，每個保持連線 $KEEP 秒 ..."
for i in $(seq 1 "$N"); do
    ./build/ns_client -p "$PORT" -k "$KEEP" sysinfo > $OUT/client_$i.log 2>&1 &
done
sleep 2

echo; echo "[$(date +%T)] === ns_server 行程樹（1 個 parent + 多個 child）==="
ps -o pid,ppid,stat,etime,args --forest -p "$PARENT" --ppid "$PARENT"
echo "child 行程數量 : $(pgrep -P "$PARENT" | wc -l)"
if command -v ss >/dev/null; then
    echo "ESTABLISHED 連線數 (server 端 :$PORT) : $(ss -Htn state established "( sport = :$PORT )" | wc -l)"
fi

wait
echo; echo "[$(date +%T)] === 結果 ==="
echo "成功收到系統資訊的 client : $(grep -l 'Served by' $OUT/*.log | wc -l) / $N"
echo "各 client 分別由哪個 child 服務："
grep -h 'Served by' $OUT/*.log | sort
