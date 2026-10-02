# gPlat Project Makefile
# Supports Linux only (g++ compiler)
# Features: Incremental build, Automatic dependency tracking
# Third party libraries (e.g. snap7) are built via their own makefiles and copied to $(LIB_DIR)
# Note: 如果需要添加新的模块，请按照以下步骤：
# 1. 按模板在 3 中定义 XXX_DIR/SRCS/OBJS/BIN/INCLUDES/LDFLAGS
# 2. 在 4 中添加编译和链接规则
# 3. 在 4 中将 $(XXX_BIN) 加入 all
# 4. 在 4 中添加 -include $(XXX_OBJS:.o=.d)
# 5. 在 4 中更新 help

# ==========================================
# 1. Platform Detection
# ==========================================
ifeq ($(OS),Windows_NT)
    $(error Error: Building on Windows is not supported due to POSIX threading model usage. Please use WSL or a Linux Virtual Machine.)
endif

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    $(error Error: Building on macOS is not supported because the server requires Linux epoll. Please use Docker or a Linux Virtual Machine.)
else ifneq ($(UNAME_S),Linux)
    $(error Unsupported OS: $(UNAME_S). Only Linux is supported.)
endif

CXX ?= g++
CXXFLAGS_PLATFORM :=
LDFLAGS_PLATFORM :=

# ==========================================
# 2. General Settings
# ==========================================
BUILD_DIR := build
BIN_DIR := bin
LIB_DIR := lib

# make ASAN=1 时：开启 AddressSanitizer/LeakSanitizer，并把产物隔离到 build/asan/{bin,lib}。
# bin 与 lib 必须同级，才能满足所有可执行的 rpath '$ORIGIN/../lib'。常规（非 ASan）构建完全不受影响。
# 利用已有的空钩子 CXXFLAGS_PLATFORM/LDFLAGS_PLATFORM，在 CXXFLAGS/LDFLAGS 取值（下面的 := 立即展开）之前注入。
ifeq ($(ASAN),1)
    CXXFLAGS_PLATFORM += -fsanitize=address -fno-omit-frame-pointer -O1
    LDFLAGS_PLATFORM  += -fsanitize=address
    BUILD_DIR := build/asan
    BIN_DIR   := build/asan/bin
    LIB_DIR   := build/asan/lib
endif

# Standard flags: C++17, Generate dependency info (-MMD -MP), Warnings, Debug symbols
CXXFLAGS := -std=c++17 -Wall -g -MMD -MP $(CXXFLAGS_PLATFORM)
LDFLAGS := $(LDFLAGS_PLATFORM)

# ==========================================
# 3. Module Definitions
# ==========================================

# --- Module: gplat (Main Server) ---
GPLAT_DIR := gplat
GPLAT_SRCS := $(wildcard $(GPLAT_DIR)/*.cxx)
GPLAT_OBJS := $(patsubst $(GPLAT_DIR)/%.cxx, $(BUILD_DIR)/$(GPLAT_DIR)/%.o, $(GPLAT_SRCS))
GPLAT_BIN := $(BIN_DIR)/gplat
GPLAT_LDLIBS := -lpthread -L$(LIB_DIR) -lhigplat -Wl,-rpath,'$$ORIGIN/../lib'

# --- Module: higplat (Shared Library) ---
HIGPLAT_DIR := higplat
HIGPLAT_SRCS := $(wildcard $(HIGPLAT_DIR)/*.cpp)
HIGPLAT_OBJS := $(patsubst $(HIGPLAT_DIR)/%.cpp, $(BUILD_DIR)/$(HIGPLAT_DIR)/%.o, $(HIGPLAT_SRCS))
HIGPLAT_LIB := $(LIB_DIR)/libhigplat.so
HIGPLAT_CXXFLAGS := -fPIC

# --- Module: createq (Tool) ---
CREATEQ_DIR := createq
CREATEQ_SRCS := $(wildcard $(CREATEQ_DIR)/*.cpp)
CREATEQ_OBJS := $(patsubst $(CREATEQ_DIR)/%.cpp, $(BUILD_DIR)/$(CREATEQ_DIR)/%.o, $(CREATEQ_SRCS))
CREATEQ_BIN := $(BIN_DIR)/createq
CREATEQ_INCLUDES := -Iinclude
# Link against higplat. Using $ORIGIN allows running from bin/ without setting LD_LIBRARY_PATH
CREATEQ_LDFLAGS := -L$(LIB_DIR) -lhigplat -Wl,-rpath,'$$ORIGIN/../lib'

# --- Module: createb (Tool) ---
CREATEB_DIR := createb
CREATEB_SRCS := $(wildcard $(CREATEB_DIR)/*.cpp)
CREATEB_OBJS := $(patsubst $(CREATEB_DIR)/%.cpp, $(BUILD_DIR)/$(CREATEB_DIR)/%.o, $(CREATEB_SRCS))
CREATEB_BIN := $(BIN_DIR)/createb
CREATEB_INCLUDES := -Iinclude
CREATEB_LDFLAGS := -L$(LIB_DIR) -lhigplat -Wl,-rpath,'$$ORIGIN/../lib'

# --- Module: toolgplat (Tool) ---
TOOLGPLAT_DIR := toolgplat
TOOLGPLAT_SRCS := $(wildcard $(TOOLGPLAT_DIR)/*.cpp)
TOOLGPLAT_OBJS := $(patsubst $(TOOLGPLAT_DIR)/%.cpp, $(BUILD_DIR)/$(TOOLGPLAT_DIR)/%.o, $(TOOLGPLAT_SRCS))
TOOLGPLAT_BIN := $(BIN_DIR)/toolgplat
TOOLGPLAT_INCLUDES := -Iinclude
TOOLGPLAT_LDFLAGS := -lreadline -lpthread -L$(LIB_DIR) -lhigplat -Wl,-rpath,'$$ORIGIN/../lib'

# --- Module: testapp (Tool) ---
TESTAPP_DIR := testapp
TESTAPP_SRCS := $(wildcard $(TESTAPP_DIR)/*.cpp)
TESTAPP_OBJS := $(patsubst $(TESTAPP_DIR)/%.cpp, $(BUILD_DIR)/$(TESTAPP_DIR)/%.o, $(TESTAPP_SRCS))
TESTAPP_BIN := $(BIN_DIR)/testapp
TESTAPP_INCLUDES := -Iinclude
TESTAPP_LDFLAGS := -lpthread -L$(LIB_DIR) -lhigplat -Wl,-rpath,'$$ORIGIN/../lib'

# --- Module: testapp2 (Tool) ---
TESTAPP2_DIR := testapp2
TESTAPP2_SRCS := $(wildcard $(TESTAPP2_DIR)/*.cpp)
TESTAPP2_OBJS := $(patsubst $(TESTAPP2_DIR)/%.cpp, $(BUILD_DIR)/$(TESTAPP2_DIR)/%.o, $(TESTAPP2_SRCS))
TESTAPP2_BIN := $(BIN_DIR)/testapp2
TESTAPP2_INCLUDES := -Iinclude
TESTAPP2_LDFLAGS := -lpthread -L$(LIB_DIR) -lhigplat -Wl,-rpath,'$$ORIGIN/../lib'

# --- Module: testapp3 (Tool) ---
TESTAPP3_DIR := testapp3
TESTAPP3_SRCS := $(wildcard $(TESTAPP3_DIR)/*.cpp)
TESTAPP3_OBJS := $(patsubst $(TESTAPP3_DIR)/%.cpp, $(BUILD_DIR)/$(TESTAPP3_DIR)/%.o, $(TESTAPP3_SRCS))
TESTAPP3_BIN := $(BIN_DIR)/testapp3
TESTAPP3_INCLUDES := -Iinclude
TESTAPP3_LDFLAGS := -lpthread -L$(LIB_DIR) -lhigplat -Wl,-rpath,'$$ORIGIN/../lib'

# --- Module: testapp4 (Tool) ---
TESTAPP4_DIR := testapp4
TESTAPP4_SRCS := $(wildcard $(TESTAPP4_DIR)/*.cpp)
TESTAPP4_OBJS := $(patsubst $(TESTAPP4_DIR)/%.cpp, $(BUILD_DIR)/$(TESTAPP4_DIR)/%.o, $(TESTAPP4_SRCS))
TESTAPP4_BIN := $(BIN_DIR)/testapp4
TESTAPP4_INCLUDES := -Iinclude
TESTAPP4_LDFLAGS := -lpthread -L$(LIB_DIR) -lhigplat -Wl,-rpath,'$$ORIGIN/../lib'

# --- Module: testapp5 (Tool) ---
TESTAPP5_DIR := testapp5
TESTAPP5_SRCS := $(wildcard $(TESTAPP5_DIR)/*.cpp)
TESTAPP5_OBJS := $(patsubst $(TESTAPP5_DIR)/%.cpp, $(BUILD_DIR)/$(TESTAPP5_DIR)/%.o, $(TESTAPP5_SRCS))
TESTAPP5_BIN := $(BIN_DIR)/testapp5
TESTAPP5_INCLUDES := -Iinclude
TESTAPP5_LDFLAGS := -lpthread -L$(LIB_DIR) -lhigplat -Wl,-rpath,'$$ORIGIN/../lib'

# --- Module: testapp6 (Tool) ---
TESTAPP6_DIR := testapp6
TESTAPP6_SRCS := $(wildcard $(TESTAPP6_DIR)/*.cpp)
TESTAPP6_OBJS := $(patsubst $(TESTAPP6_DIR)/%.cpp, $(BUILD_DIR)/$(TESTAPP6_DIR)/%.o, $(TESTAPP6_SRCS))
TESTAPP6_BIN := $(BIN_DIR)/testapp6
TESTAPP6_INCLUDES := -Iinclude
TESTAPP6_LDFLAGS := -lpthread -L$(LIB_DIR) -lhigplat -Wl,-rpath,'$$ORIGIN/../lib'

# --- Module: testapp7 (Tool) ---
TESTAPP7_DIR := testapp7
TESTAPP7_SRCS := $(wildcard $(TESTAPP7_DIR)/*.cpp)
TESTAPP7_OBJS := $(patsubst $(TESTAPP7_DIR)/%.cpp, $(BUILD_DIR)/$(TESTAPP7_DIR)/%.o, $(TESTAPP7_SRCS))
TESTAPP7_BIN := $(BIN_DIR)/testapp7
TESTAPP7_INCLUDES := -Iinclude
TESTAPP7_LDFLAGS := -lpthread -L$(LIB_DIR) -lhigplat -Wl,-rpath,'$$ORIGIN/../lib'

# --- Module: testsuite (End-to-end Automated Test Suite) ---
# 源码分三层：入口 testsuite/*.cpp、框架 testsuite/framework/*.cpp、用例 testsuite/cases/*.cpp。
# 全部静态链接 libhigplat.so（与其它 testapp 一致）：用例通过网络 API 打本套件自建的 sandbox 实例，
# 白盒校验器另行直接 mmap sandbox 文件。scripts/ 下的 gen_fixtures.cpp 不在此 glob 内（单独成目标，避免重复 main）。
TESTSUITE_DIR      := testsuite
TESTSUITE_SRCS     := $(wildcard $(TESTSUITE_DIR)/*.cpp) \
                      $(wildcard $(TESTSUITE_DIR)/framework/*.cpp) \
                      $(wildcard $(TESTSUITE_DIR)/cases/*.cpp)
TESTSUITE_OBJS     := $(patsubst $(TESTSUITE_DIR)/%.cpp, $(BUILD_DIR)/$(TESTSUITE_DIR)/%.o, $(TESTSUITE_SRCS))
TESTSUITE_BIN      := $(BIN_DIR)/testsuite
TESTSUITE_INCLUDES := -Iinclude -I$(TESTSUITE_DIR)
TESTSUITE_LDFLAGS  := -lpthread -L$(LIB_DIR) -lhigplat -Wl,-rpath,'$$ORIGIN/../lib'

# --- Module: gen_fixtures (standalone fixture generator) ---
# 复用 testsuite 框架里的 fixtures 定义（framework/fixtures.o），提供一个独立可执行，
# 便于手工把测试所需的 BOARD/QUEUE 通过本地 API 灌入指定 qbd 目录（scripts/fixtures.sh 会调用它）。
GEN_FIXTURES_SRC   := $(TESTSUITE_DIR)/scripts/gen_fixtures.cpp
GEN_FIXTURES_OBJ   := $(BUILD_DIR)/$(TESTSUITE_DIR)/scripts/gen_fixtures.o
GEN_FIXTURES_DEP   := $(BUILD_DIR)/$(TESTSUITE_DIR)/framework/fixtures.o
GEN_FIXTURES_BIN   := $(BIN_DIR)/gen_fixtures

# --- Module: snap7 (Third-party Shared Library) ---
SNAP7_BUILD_DIR := snap7/build/linux
SNAP7_UPSTREAM_LIB := snap7/build/bin/linux/libsnap7.so
SNAP7_LIB := $(LIB_DIR)/libsnap7.so

# --- Module: s7ioserver (Tool) ---
S7IOSERVER_DIR := s7ioserver
S7IOSERVER_SRCS := $(wildcard $(S7IOSERVER_DIR)/*.cpp)
S7IOSERVER_OBJS := $(patsubst $(S7IOSERVER_DIR)/%.cpp, $(BUILD_DIR)/$(S7IOSERVER_DIR)/%.o, $(S7IOSERVER_SRCS))
S7IOSERVER_BIN := $(BIN_DIR)/s7ioserver
S7IOSERVER_INCLUDES := -Iinclude -I$(S7IOSERVER_DIR)
S7IOSERVER_LDFLAGS := -lpthread -L$(LIB_DIR) -lhigplat -lsnap7 -Wl,-rpath,'$$ORIGIN/../lib'

# ==========================================
# 4. Targets
# ==========================================

.PHONY: all clean directories help install \
	gplat higplat createq createb toolgplat snap7 s7ioserver \
	testapp testapp2 testapp3 testapp4 testapp5 testapp6 testapp7 \
	clean-gplat clean-higplat clean-createq clean-createb clean-toolgplat clean-snap7 clean-s7ioserver \
	clean-testapp clean-testapp2 clean-testapp3 clean-testapp4 clean-testapp5 clean-testapp6 clean-testapp7 \
	testsuite gen_fixtures test test-asan test-destructive test-stress asan-build clean-testsuite

all: directories $(HIGPLAT_LIB) $(SNAP7_LIB) $(GPLAT_BIN) $(CREATEQ_BIN) $(CREATEB_BIN) $(TOOLGPLAT_BIN) $(S7IOSERVER_BIN) $(TESTAPP_BIN) $(TESTAPP2_BIN) $(TESTAPP3_BIN) $(TESTAPP4_BIN) $(TESTAPP5_BIN) $(TESTAPP6_BIN) $(TESTAPP7_BIN) $(TESTSUITE_BIN)
	@echo "OK"

gplat: directories $(HIGPLAT_LIB) $(GPLAT_BIN)
	@echo "OK"

higplat: directories $(HIGPLAT_LIB)
	@echo "OK"

createq: directories $(HIGPLAT_LIB) $(CREATEQ_BIN)
	@echo "OK"

createb: directories $(HIGPLAT_LIB) $(CREATEB_BIN)
	@echo "OK"

toolgplat: directories $(HIGPLAT_LIB) $(TOOLGPLAT_BIN)
	@echo "OK"

testapp: directories $(HIGPLAT_LIB) $(TESTAPP_BIN)
	@echo "OK"

testapp2: directories $(HIGPLAT_LIB) $(TESTAPP2_BIN)
	@echo "OK"

testapp3: directories $(HIGPLAT_LIB) $(TESTAPP3_BIN)
	@echo "OK"

testapp4: directories $(HIGPLAT_LIB) $(TESTAPP4_BIN)
	@echo "OK"

testapp5: directories $(HIGPLAT_LIB) $(TESTAPP5_BIN)
	@echo "OK"

testapp6: directories $(HIGPLAT_LIB) $(TESTAPP6_BIN)
	@echo "OK"

testapp7: directories $(HIGPLAT_LIB) $(TESTAPP7_BIN)
	@echo "OK"

# 编译测试套件本体 + 独立的 fixture 生成器
testsuite: directories $(HIGPLAT_LIB) $(TESTSUITE_BIN) $(GEN_FIXTURES_BIN)
	@echo "OK"

# 常规回归：默认排除破坏性/压力用例（由套件自建 sandbox + 自启 gplat，不触碰任何既有实例）
test: gplat testsuite
	@echo "Running regression suite (non-destructive)..."
	@$(TESTSUITE_BIN)

# 内部目标：仅编译 ASan 变体需要的子集（higplat/gplat/createb/createq/testsuite，不含 snap7/s7ioserver）
asan-build: directories $(HIGPLAT_LIB) $(GPLAT_BIN) $(CREATEB_BIN) $(CREATEQ_BIN) $(TESTSUITE_BIN) $(GEN_FIXTURES_BIN)
	@echo "ASan build OK -> $(BIN_DIR)"

# ASan/LSan 泄漏敏感用例：先以 ASAN=1 重新编出隔离构建树，再用该实例跑
test-asan:
	@$(MAKE) ASAN=1 asan-build
	@echo "Running leak-sensitive suite under ASan..."
	@build/asan/bin/testsuite --expect-asan --tag asan

# 破坏性用例：每条在子进程 fork 隔离下运行，崩溃后由 fixture 重启 server
test-destructive: gplat testsuite
	@echo "Running destructive suite (fork-isolated)..."
	@$(TESTSUITE_BIN) --tag destructive --run-destructive

# 压力/基准：非 ASan，正确性+无泄漏为判据，性能数字仅报告
test-stress: gplat testsuite
	@echo "Running stress/benchmark suite..."
	@$(TESTSUITE_BIN) --tag stress --run-stress

snap7: directories $(SNAP7_LIB)
	@echo "OK"

s7ioserver: directories $(HIGPLAT_LIB) $(SNAP7_LIB) $(S7IOSERVER_BIN)
	@echo "OK"

directories:
	@mkdir -p $(BIN_DIR) $(LIB_DIR)

# --- Rules for gplat ---
$(GPLAT_BIN): $(GPLAT_OBJS) $(HIGPLAT_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(GPLAT_OBJS) $(GPLAT_LDLIBS) -o $@

$(BUILD_DIR)/$(GPLAT_DIR)/%.o: $(GPLAT_DIR)/%.cxx
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) -I$(GPLAT_DIR) -c $< -o $@

# --- Rules for higplat ---
$(HIGPLAT_LIB): $(HIGPLAT_OBJS)
	@echo "Linking Shared Lib $@"
	@$(CXX) -shared $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/$(HIGPLAT_DIR)/%.o: $(HIGPLAT_DIR)/%.cpp
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) $(HIGPLAT_CXXFLAGS) -c $< -o $@

# --- Rules for createq ---
$(CREATEQ_BIN): $(CREATEQ_OBJS) $(HIGPLAT_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(CREATEQ_OBJS) $(CREATEQ_LDFLAGS) -o $@

$(BUILD_DIR)/$(CREATEQ_DIR)/%.o: $(CREATEQ_DIR)/%.cpp
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) $(CREATEQ_INCLUDES) -c $< -o $@

# --- Rules for createb ---
$(CREATEB_BIN): $(CREATEB_OBJS) $(HIGPLAT_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(CREATEB_OBJS) $(CREATEB_LDFLAGS) -o $@

$(BUILD_DIR)/$(CREATEB_DIR)/%.o: $(CREATEB_DIR)/%.cpp
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) $(CREATEB_INCLUDES) -c $< -o $@

# --- Rules for toolgplat ---
$(TOOLGPLAT_BIN): $(TOOLGPLAT_OBJS) $(HIGPLAT_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(TOOLGPLAT_OBJS) $(TOOLGPLAT_LDFLAGS) -o $@

$(BUILD_DIR)/$(TOOLGPLAT_DIR)/%.o: $(TOOLGPLAT_DIR)/%.cpp
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) $(TOOLGPLAT_INCLUDES) -c $< -o $@

# --- Rules for testapp ---
$(TESTAPP_BIN): $(TESTAPP_OBJS) $(HIGPLAT_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(TESTAPP_OBJS) $(TESTAPP_LDFLAGS) -o $@

$(BUILD_DIR)/$(TESTAPP_DIR)/%.o: $(TESTAPP_DIR)/%.cpp
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) $(TESTAPP_INCLUDES) -c $< -o $@

# --- Rules for testapp2 ---
$(TESTAPP2_BIN): $(TESTAPP2_OBJS) $(HIGPLAT_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(TESTAPP2_OBJS) $(TESTAPP2_LDFLAGS) -o $@

$(BUILD_DIR)/$(TESTAPP2_DIR)/%.o: $(TESTAPP2_DIR)/%.cpp
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) $(TESTAPP2_INCLUDES) -c $< -o $@

# --- Rules for testapp3 ---
$(TESTAPP3_BIN): $(TESTAPP3_OBJS) $(HIGPLAT_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(TESTAPP3_OBJS) $(TESTAPP3_LDFLAGS) -o $@

$(BUILD_DIR)/$(TESTAPP3_DIR)/%.o: $(TESTAPP3_DIR)/%.cpp
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) $(TESTAPP3_INCLUDES) -c $< -o $@

# --- Rules for testapp4 ---
$(TESTAPP4_BIN): $(TESTAPP4_OBJS) $(HIGPLAT_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(TESTAPP4_OBJS) $(TESTAPP4_LDFLAGS) -o $@

$(BUILD_DIR)/$(TESTAPP4_DIR)/%.o: $(TESTAPP4_DIR)/%.cpp
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) $(TESTAPP4_INCLUDES) -c $< -o $@

# --- Rules for testapp5 ---
$(TESTAPP5_BIN): $(TESTAPP5_OBJS) $(HIGPLAT_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(TESTAPP5_OBJS) $(TESTAPP5_LDFLAGS) -o $@

$(BUILD_DIR)/$(TESTAPP5_DIR)/%.o: $(TESTAPP5_DIR)/%.cpp
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) $(TESTAPP5_INCLUDES) -c $< -o $@

# --- Rules for testapp6 ---
$(TESTAPP6_BIN): $(TESTAPP6_OBJS) $(HIGPLAT_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(TESTAPP6_OBJS) $(TESTAPP6_LDFLAGS) -o $@

$(BUILD_DIR)/$(TESTAPP6_DIR)/%.o: $(TESTAPP6_DIR)/%.cpp
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) $(TESTAPP6_INCLUDES) -c $< -o $@

# --- Rules for testapp7 ---
$(TESTAPP7_BIN): $(TESTAPP7_OBJS) $(HIGPLAT_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(TESTAPP7_OBJS) $(TESTAPP7_LDFLAGS) -o $@

$(BUILD_DIR)/$(TESTAPP7_DIR)/%.o: $(TESTAPP7_DIR)/%.cpp
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) $(TESTAPP7_INCLUDES) -c $< -o $@

# --- Rules for testsuite ---
$(TESTSUITE_BIN): $(TESTSUITE_OBJS) $(HIGPLAT_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(TESTSUITE_OBJS) $(TESTSUITE_LDFLAGS) -o $@

# 同一条模式规则覆盖 testsuite/ 下的 入口/framework/cases/scripts 四处（mkdir -p $(@D) 自动建子目录）
$(BUILD_DIR)/$(TESTSUITE_DIR)/%.o: $(TESTSUITE_DIR)/%.cpp
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) $(TESTSUITE_INCLUDES) -c $< -o $@

# --- Rules for gen_fixtures (standalone, reuses framework/fixtures.o) ---
$(GEN_FIXTURES_BIN): $(GEN_FIXTURES_OBJ) $(GEN_FIXTURES_DEP) $(HIGPLAT_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(GEN_FIXTURES_OBJ) $(GEN_FIXTURES_DEP) $(TESTSUITE_LDFLAGS) -o $@

# --- Rules for snap7 third-party ---
$(SNAP7_LIB):
	@echo "Building third-party snap7 via upstream makefile"
	@$(MAKE) -C $(SNAP7_BUILD_DIR) all
	@cp -f $(SNAP7_UPSTREAM_LIB) $@

# --- Rules for s7ioserver ---
$(S7IOSERVER_BIN): $(S7IOSERVER_OBJS) $(HIGPLAT_LIB) $(SNAP7_LIB)
	@echo "Linking $@"
	@$(CXX) $(LDFLAGS) $(S7IOSERVER_OBJS) $(S7IOSERVER_LDFLAGS) -o $@

$(BUILD_DIR)/$(S7IOSERVER_DIR)/%.o: $(S7IOSERVER_DIR)/%.cpp
	@mkdir -p $(@D)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) $(S7IOSERVER_INCLUDES) -c $< -o $@

# --- Clean rules ---
clean-gplat:
	@echo "Cleaning gplat artifacts..."
	@rm -f $(GPLAT_BIN)
	@rm -rf $(BUILD_DIR)/$(GPLAT_DIR)
	@echo "OK"

clean-higplat:
	@echo "Cleaning higplat artifacts..."
	@rm -f $(HIGPLAT_LIB)
	@rm -rf $(BUILD_DIR)/$(HIGPLAT_DIR)
	@echo "OK"

clean-createq:
	@echo "Cleaning createq artifacts..."
	@rm -f $(CREATEQ_BIN)
	@rm -rf $(BUILD_DIR)/$(CREATEQ_DIR)
	@echo "OK"

clean-createb:
	@echo "Cleaning createb artifacts..."
	@rm -f $(CREATEB_BIN)
	@rm -rf $(BUILD_DIR)/$(CREATEB_DIR)
	@echo "OK"

clean-toolgplat:
	@echo "Cleaning toolgplat artifacts..."
	@rm -f $(TOOLGPLAT_BIN)
	@rm -rf $(BUILD_DIR)/$(TOOLGPLAT_DIR)
	@echo "OK"

clean-testapp:
	@echo "Cleaning testapp artifacts..."
	@rm -f $(TESTAPP_BIN)
	@rm -rf $(BUILD_DIR)/$(TESTAPP_DIR)
	@echo "OK"

clean-testapp2:
	@echo "Cleaning testapp2 artifacts..."
	@rm -f $(TESTAPP2_BIN)
	@rm -rf $(BUILD_DIR)/$(TESTAPP2_DIR)
	@echo "OK"

clean-testapp3:
	@echo "Cleaning testapp3 artifacts..."
	@rm -f $(TESTAPP3_BIN)
	@rm -rf $(BUILD_DIR)/$(TESTAPP3_DIR)
	@echo "OK"

clean-testapp4:
	@echo "Cleaning testapp4 artifacts..."
	@rm -f $(TESTAPP4_BIN)
	@rm -rf $(BUILD_DIR)/$(TESTAPP4_DIR)
	@echo "OK"

clean-testapp5:
	@echo "Cleaning testapp5 artifacts..."
	@rm -f $(TESTAPP5_BIN)
	@rm -rf $(BUILD_DIR)/$(TESTAPP5_DIR)
	@echo "OK"

clean-testapp6:
	@echo "Cleaning testapp6 artifacts..."
	@rm -f $(TESTAPP6_BIN)
	@rm -rf $(BUILD_DIR)/$(TESTAPP6_DIR)
	@echo "OK"

clean-testapp7:
	@echo "Cleaning testapp7 artifacts..."
	@rm -f $(TESTAPP7_BIN)
	@rm -rf $(BUILD_DIR)/$(TESTAPP7_DIR)
	@echo "OK"

clean-testsuite:
	@echo "Cleaning testsuite artifacts..."
	@rm -f $(TESTSUITE_BIN) $(GEN_FIXTURES_BIN)
	@rm -rf $(BUILD_DIR)/$(TESTSUITE_DIR)
	@rm -rf build/asan
	@echo "OK"

clean-snap7:
	@echo "Cleaning snap7 artifacts via upstream makefile"
	@$(MAKE) -C $(SNAP7_BUILD_DIR) clean >/dev/null 2>&1 || true
	@rm -f $(SNAP7_LIB)
	@echo "OK"

clean-s7ioserver:
	@echo "Cleaning s7ioserver artifacts..."
	@rm -f $(S7IOSERVER_BIN)
	@rm -rf $(BUILD_DIR)/$(S7IOSERVER_DIR)
	@echo "OK"

install: $(HIGPLAT_LIB) $(SNAP7_LIB)
	@echo "Installing shared libraries to /usr/local/lib/"
	@cp -f $(HIGPLAT_LIB) /usr/local/lib/
	@cp -f $(SNAP7_LIB) /usr/local/lib/
	@ldconfig
	@echo "OK"

clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR) $(BIN_DIR) $(LIB_DIR)
	@echo "OK"

# --- Help ---
help:
	@echo "The compiled binaries will be located in the 'bin/' directory, and the shared library will be in 'lib/'."
	@echo "And the dependency files (.d) and the object files (.o) will be located in 'build/'."
	@echo "Available targets:"
	@echo "  all                    : Build all modules (higplat, snap7, gplat, createq, createb, toolgplat, s7ioserver, testapp, testapp2, testapp3, testapp4, testapp5, testapp6, testapp7, testsuite)"
	@echo "  gplat|s7ioserver|...   : Build a single target"
	@echo "  testsuite              : Build the end-to-end test suite (+ gen_fixtures generator)"
	@echo "  test                   : Build and run the regression suite (non-destructive, self-hosted sandbox)"
	@echo "  test-asan              : Rebuild higplat+gplat+tools+suite with ASan (build/asan/) and run leak-sensitive cases"
	@echo "  test-destructive       : Run destructive cases (fork-isolated; server restarted per case)"
	@echo "  test-stress            : Run stress/benchmark cases (correctness + no-leak gate; perf reported only)"
	@echo "  clean                  : Remove build directories and binaries"
	@echo "  clean-<target>         : clean one target (e.g. clean-gplat, clean-testsuite)"
	@echo "  install                : Install libhigplat.so and libsnap7.so to /usr/local/lib/ (requires sudo)"
	@echo "  help                   : Show this help message"

# Include dependency files
-include $(GPLAT_OBJS:.o=.d)
-include $(HIGPLAT_OBJS:.o=.d)
-include $(CREATEQ_OBJS:.o=.d)
-include $(CREATEB_OBJS:.o=.d)
-include $(TOOLGPLAT_OBJS:.o=.d)
-include $(TESTAPP_OBJS:.o=.d)
-include $(TESTAPP2_OBJS:.o=.d)
-include $(TESTAPP3_OBJS:.o=.d)
-include $(TESTAPP4_OBJS:.o=.d)
-include $(TESTAPP5_OBJS:.o=.d)
-include $(TESTAPP6_OBJS:.o=.d)
-include $(TESTAPP7_OBJS:.o=.d)
-include $(TESTSUITE_OBJS:.o=.d)
-include $(GEN_FIXTURES_OBJ:.o=.d)
-include $(S7IOSERVER_OBJS:.o=.d)