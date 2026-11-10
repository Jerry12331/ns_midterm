#!/usr/bin/env bash
# 示範 4：client 中途斷線 —— 忽略 SIGPIPE + 檢查 send() 回傳值
# client 要求 monitor 10 次，收到第一筆就斷線，server 還在持續推送
source "$(dirname "$0")/common.sh"
for mode in without with; do
    if [ $mode = without ]; then banner "沒有 SIGPIPE 處理 (-D sigpipe)"; start_server -D sigpipe
    else banner "有 SIGPIPE 處理"; start_server; fi
    $BIN/ns_client -p "$PORT" --abort-monitor
    sleep 4
    stop_server
done
