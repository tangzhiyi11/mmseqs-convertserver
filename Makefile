# Makefile for convertserver
# 独立编译，不依赖 MMseqs2 库

CXX = g++
CXXFLAGS = -O3 -std=c++11 -pthread -Wall -Wno-unused-function
LDFLAGS = -lpthread

# 目标文件
TARGETS = convertserver convertalis-fast

# 源文件
SRCDIR = src
OBJDIR = obj

# 默认目标
all: $(TARGETS)

# convertserver
convertserver: $(SRCDIR)/convertserver.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built convertserver"

# convertalis-fast
convertalis-fast: $(SRCDIR)/convertalis_fast.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built convertalis-fast"

# 清理
clean:
	rm -f $(TARGETS)
	rm -rf $(OBJDIR)

# 安装
install: all
	install -m 755 convertserver /usr/local/bin/
	install -m 755 convertalis-fast /usr/local/bin/

# 测试
test: all
	@echo "Testing convertserver..."
	@./convertserver --help 2>/dev/null || true
	@echo ""
	@echo "Testing convertalis-fast..."
	@./convertalis-fast --help 2>/dev/null || true

.PHONY: all clean install test
