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
TOOLGPLAT_LDFLAGS := -lreadline -L$(LIB_DIR) -lhigplat -Wl,-rpath,'$$ORIGIN/../lib'

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

.PHONY: all clean directories help \
	gplat higplat createq createb toolgplat snap7 s7ioserver \
	clean-gplat clean-higplat clean-createq clean-createb clean-toolgplat clean-snap7 clean-s7ioserver

all: directories $(HIGPLAT_LIB) $(SNAP7_LIB) $(GPLAT_BIN) $(CREATEQ_BIN) $(CREATEB_BIN) $(TOOLGPLAT_BIN) $(S7IOSERVER_BIN)
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

clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR) $(BIN_DIR) $(LIB_DIR)
	@echo "OK"

# --- Help ---
help:
	@echo "Available targets:"
	@echo "  all                    : Build all modules (higplat, snap7, gplat, createq, createb, toolgplat, s7ioserver)"
	@echo "  gplat||s7ioserver|...  : Build a single target"
	@echo "  clean                  : Remove build directories and binaries"
	@echo "  clean-<target>         : clean one target (e.g. clean-gplat)"

# Include dependency files
-include $(GPLAT_OBJS:.o=.d)
-include $(HIGPLAT_OBJS:.o=.d)
-include $(CREATEQ_OBJS:.o=.d)
-include $(CREATEB_OBJS:.o=.d)
-include $(TOOLGPLAT_OBJS:.o=.d)
-include $(S7IOSERVER_OBJS:.o=.d)