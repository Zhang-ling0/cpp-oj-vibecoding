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
#   make build JOBS=1              手工降并发（见下面的 JOBS）
#
# ---- JOBS：为什么默认是 2 而不是 -j（= CPU 数）或 -j8 ----
# 开发机实测 2 核 / 1967MB（SPEC §0「开发机硬约束」）。这不是"核数=2 所以 -j2 刚好"，
# 而是【内存】卡住的：mysqld 常驻后可用内存常只剩 300MB 上下，而带 sanitizer 的
# 单个翻译单元峰值可达数百 MB —— 并发一高就进 swap/OOM killer，表现为构建"卡死"。
# 实测 ASan 构建 -j4 会挂住，-j1 正常。日常构建 -j2、sanitizer -j1 是这条约束的落法。
# 传 JOBS=1/2 可临时覆盖；不要改成 -j 或 -j$(nproc)。
# =============================================================================
CXX          ?= g++-12            # D-13：判题编译器锁 12
BUILD_DIR    ?= build
BUILD_TYPE   ?= RelWithDebInfo
CMAKE        ?= cmake
CTEST        ?= ctest
JOBS         ?= 2                 # SPEC §0「开发机硬约束」：并发上限，不是"核数"

.PHONY: all configure build run test clean

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) \
		-DCMAKE_CXX_COMPILER=$(CXX) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS)

test: build
	# --no-tests=error：ARGS 里的过滤串写错时，ctest 默认返回 0（看着像"全过"）
	# 不加 -j：198 条用例串行才 18 秒，并发只会和构建/DB 抢 CPU 与内存（见文件头 JOBS 说明）
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