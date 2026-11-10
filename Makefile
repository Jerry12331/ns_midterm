# 網路系統程式期中專題 — Makefile
#
#   make            一般版（LOG_DEBUG 不會被編進去）
#   make DEBUG=1    除錯版（編譯期開啟 DEBUG，加 -g -O0）
#   make clean      清除
#
# 產出（都在 build/）：
#   libnetutil.so   client/server 共用的動態函式庫
#   ns_server       server
#   ns_client       client

CC      ?= gcc
BUILD   := build
CFLAGS  := -Wall -Wextra -std=gnu11 -Iinclude -fstack-protector-strong
LDFLAGS := -L$(BUILD) -Wl,-rpath,'$$ORIGIN'   # 執行時到執行檔同目錄找 .so，不用設 LD_LIBRARY_PATH
LDLIBS  := -lnetutil

ifeq ($(DEBUG),1)
  CFLAGS += -DDEBUG -g -O0
else
  CFLAGS += -O2
endif

LIB    := $(BUILD)/libnetutil.so
SERVER := $(BUILD)/ns_server
CLIENT := $(BUILD)/ns_client
STAMP  := $(BUILD)/.cflags

.PHONY: all clean FORCE

all: $(LIB) $(SERVER) $(CLIENT)

$(BUILD):
	mkdir -p $@

# 編譯參數改變時（例如切換 DEBUG=1）自動全部重編
$(STAMP): FORCE | $(BUILD)
	@echo '$(CFLAGS)' | cmp -s - $@ || echo '$(CFLAGS)' > $@

$(LIB): lib/netutil.c include/netutil.h $(STAMP)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $<

$(SERVER): src/server.c include/netutil.h $(LIB) $(STAMP)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

$(CLIENT): src/client.c include/netutil.h $(LIB) $(STAMP)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(BUILD)
