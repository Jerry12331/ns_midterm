#!/usr/bin/env bash
# 示範 3：超大封包 —— 封包長度驗證，防止 buffer overflow
# server 的接收 buffer 只有 4096 bytes，client 宣稱並送出 8192 bytes
source "$(dirname "$0")/common.sh"
for mode in without with; do
    if [ $mode = without ]; then banner "沒有長度驗證 (-D validate)"; start_server -D validate
    else banner "有長度驗證"; start_server; fi
    $BIN/ns_client -p "$PORT" --oversize 8192
    sleep 0.5
    echo "server 本身還活著嗎？"; $BIN/ns_client -p "$PORT" ping
    stop_server
done
