#!/usr/bin/env bash
# 示範 2：閒置連線 —— SO_RCVTIMEO 逾時機制
# client 連上後什麼都不送（模擬當機的 client 或 slowloris 攻擊）
source "$(dirname "$0")/common.sh"
for mode in without with; do
    if [ $mode = without ]; then banner "沒有逾時機制 (-D timeout)"; start_server -t 3 -D timeout
    else banner "有逾時機制 (-t 3)"; start_server -t 3; fi
    $BIN/ns_client -p "$PORT" --idle 6 &
    CL=$!
    sleep 4.5
    echo "[4.5 秒後]"; show_children
    wait $CL
    stop_server
done
