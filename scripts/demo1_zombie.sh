#!/usr/bin/env bash
# 示範 1：殭屍行程 —— SIGCHLD handler + waitpid(WNOHANG)
source "$(dirname "$0")/common.sh"
for mode in without with; do
    if [ $mode = without ]; then banner "沒有回收機制 (-D zombie)"; start_server -D zombie
    else banner "有回收機制"; start_server; fi
    for i in 1 2 3 4 5; do $BIN/ns_client -p "$PORT" ping; done
    sleep 0.5
    show_children
    stop_server
done
