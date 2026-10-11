# =============================================================================
# Makefile —— 【薄封装，SPEC §9 T0.1 的 make build/run/test/clean 契约保留在此】
#
# 真正的构建规则全部在 CMakeLists.txt / tests/CMakeLists.txt 里，本文件只是转发，
# 以便「一条命令」的用法不变（§6.7.1 曾以"两个并存"为由排除 cmake，v1.8 起本文件
# 只剩转发、不再自带编译规则，该顾虑因此消解）。
#
# 常用：
#   make build                     编译 ./build/oj 与全部测试
#   make test                      跑 ctest（单测；端到端验收见 tests/run_tests.sh）
#   make run                       拉起 mysqld（若未运行）后启动 ./build/oj
#   make clean                     删除 build/
#   make test ARGS='-R Admin'      只跑名字匹配 Admin 的用例
# =============================================================================
CXX          ?= g++-12            # D-13：判题编译器锁 12
BUILD_DIR    ?= build
BUILD_TYPE   ?= RelWithDebInfo
CMAKE        ?= cmake
CTEST        ?= ctest

.PHONY: all configure build run test clean

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) \
		-DCMAKE_CXX_COMPILER=$(CXX) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j

test: build
	# --no-tests=error：ARGS 里的过滤串写错时，ctest 默认返回 0（看着像"全过"）
	$(CTEST) --test-dir $(BUILD_DIR) --no-tests=error --output-on-failure $(ARGS)

# T0.1：make run 负责拉起 mysqld（若未运行）+ 启动 ./oj
run: build
	@if ! pgrep -x mysqld >/dev/null 2>&1 && ! pgrep -x mysqld_safe >/dev/null 2>&1; then \
		echo "[make] 未检测到 mysqld，尝试拉起 mysqld_safe …"; \
		(mysqld_safe --skip-syslog >/dev/null 2>&1 || sudo mysqld_safe --skip-syslog >/dev/null 2>&1) & \
		for i in $$(seq 1 30); do \
			pgrep -x mysqld >/dev/null 2>&1 && break; \
			sleep 1; \
		done; \
	fi
	./$(BUILD_DIR)/oj

clean:
	rm -rf $(BUILD_DIR)