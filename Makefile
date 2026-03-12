# gPlat Project Makefile
# Supports Linux only (g++ compiler)
# Features: Incremental build, Automatic dependency tracking
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

CXX := g++
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
CREATEQ_SRCS := $(wildcard $(CREATEQ_DIR)/*.main.cpp) # Avoid duplicate mains if any? No, just *.cpp
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

# ==========================================
# 4. Targets
# ==========================================

.PHONY: all clean directories help

all: directories $(GPLAT_BIN) $(HIGPLAT_LIB) $(CREATEQ_BIN) $(CREATEB_BIN) $(TOOLGPLAT_BIN)
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


clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR) $(BIN_DIR) $(LIB_DIR)
	@echo "OK"

help:
	@echo "Available targets:"
	@echo "  all      : Build all modules (gplat, higplat, createq, createb, toolgplat)"
	@echo "  clean    : Remove build directories and binaries"

# Include dependency files
-include $(GPLAT_OBJS:.o=.d)
-include $(HIGPLAT_OBJS:.o=.d)
-include $(CREATEQ_OBJS:.o=.d)
-include $(CREATEB_OBJS:.o=.d)
-include $(TOOLGPLAT_OBJS:.o=.d)