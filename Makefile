# Makefile for memory allocator project
# 自动生成静态库 libmemory.a 和动态库 libmemory.so，并支持安装和清理

# 源文件和头文件路径
SRC_DIR := src
INC_DIR := include
SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:.c=.o)

# 库名
STATIC_LIB := libmemory.a
SHARED_LIB := libmemory.so

# 编译器和参数
CC ?= gcc
# -MMD -MP 生成头文件依赖，避免头文件修改后 .o 不重建导致新旧代码混编
CFLAGS := -O2 -Wall -Wextra -I$(INC_DIR) -fPIC -MMD -MP
AR := ar
ARFLAGS := rcs

# 安装路径
PREFIX ?= /usr/local
INCLUDE_INSTALL_DIR := $(PREFIX)/include/memory
LIB_INSTALL_DIR := $(PREFIX)/lib

# 测试程序
TEST_SRCS := tests/test_main.c tests/check.c
TEST_BINS := $(TEST_SRCS:.c=)

.PHONY: all static shared clean install uninstall test

# 默认目标：同时生成静态库和动态库
all: static shared

# 生成静态库
static: $(STATIC_LIB)

$(STATIC_LIB): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^

# 生成动态库
shared: $(SHARED_LIB)

$(SHARED_LIB): $(OBJS)
	$(CC) -shared -o $@ $^

# 编译所有 .c 为 .o
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# 引入头文件依赖（.d 文件由 -MMD -MP 自动生成）
-include $(OBJS:.o=.d)

# 清理目标
clean:
	rm -f $(SRC_DIR)/*.o $(SRC_DIR)/*.d $(STATIC_LIB) $(SHARED_LIB) $(TEST_BINS)

# 编译并运行测试
test: all $(TEST_BINS)
	./tests/test_main
	./tests/check

# 测试程序：链接静态库
tests/test_main: tests/test_main.c $(STATIC_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(STATIC_LIB) -lpthread

tests/check: tests/check.c $(STATIC_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(STATIC_LIB) -lpthread

# 安装库和头文件（只安装公开头文件 memory/lmalloc.h，
# 内部头文件不属于公共API；曾误装到 memory/memory/ 嵌套目录导致
# #include <memory/lmalloc.h> 找不到）
install: all
	install -d $(LIB_INSTALL_DIR)
	install -m 644 $(STATIC_LIB) $(LIB_INSTALL_DIR)/
	install -m 755 $(SHARED_LIB) $(LIB_INSTALL_DIR)/
	install -d $(INCLUDE_INSTALL_DIR)
	install -m 644 $(INC_DIR)/memory/lmalloc.h $(INCLUDE_INSTALL_DIR)/

# 卸载
uninstall:
	rm -f $(LIB_INSTALL_DIR)/$(STATIC_LIB) $(LIB_INSTALL_DIR)/$(SHARED_LIB)
	rm -f $(INCLUDE_INSTALL_DIR)/lmalloc.h
	rmdir --ignore-fail-on-non-empty $(INCLUDE_INSTALL_DIR) 2>/dev/null || true

# 用法说明
# make static   # 生成静态库 libmemory.a
# make shared   # 生成动态库 libmemory.so
# make all      # 同时生成静态和动态库
# make clean    # 清理所有中间文件和库
# make install PREFIX=/your/path  # 安装库和头文件
# make uninstall # 卸载库和头文件 