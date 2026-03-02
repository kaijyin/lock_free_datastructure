#!/bin/bash
# build.sh - 无锁数据结构项目构建脚本

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

BUILD_TYPE="Release"
CLEAN=false
RUN_CORRECTNESS=false
RUN_PERF=false

show_help() {
    echo "用法: $0 [选项]"
    echo ""
    echo "选项:"
    echo "  -h, --help       显示帮助"
    echo "  -d, --debug      Debug 模式（开启 ThreadSanitizer）"
    echo "  -r, --release    Release 模式（默认）"
    echo "  -c, --clean      清理后重建"
    echo "  --test           编译后运行正确性测试"
    echo "  --perf           编译后运行性能测试"
    echo "  --all            编译后运行所有测试"
}

for arg in "$@"; do
    case $arg in
        -h|--help)    show_help; exit 0 ;;
        -d|--debug)   BUILD_TYPE="Debug" ;;
        -r|--release) BUILD_TYPE="Release" ;;
        -c|--clean)   CLEAN=true ;;
        --test)       RUN_CORRECTNESS=true ;;
        --perf)       RUN_PERF=true ;;
        --all)        RUN_CORRECTNESS=true; RUN_PERF=true ;;
        *) echo -e "${RED}未知参数: $arg${NC}"; show_help; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo -e "${YELLOW}构建类型: $BUILD_TYPE${NC}"

if [ "$CLEAN" = true ] && [ -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}清理构建目录...${NC}"
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR" || exit 1

echo -e "${YELLOW}运行 CMake 配置...${NC}"
cmake -DCMAKE_BUILD_TYPE="$BUILD_TYPE" .. 2>&1
if [ $? -ne 0 ]; then
    echo -e "${RED}CMake 配置失败！${NC}"; exit 1
fi

echo -e "${YELLOW}编译...${NC}"
cmake --build . --parallel "$(nproc)" 2>&1
if [ $? -ne 0 ]; then
    echo -e "${RED}编译失败！${NC}"; exit 1
fi

echo -e "${GREEN}编译成功！${NC}"
echo "  正确性测试: $BUILD_DIR/correctness_test"
echo "  性能测试:   $BUILD_DIR/performance_test"

if [ "$RUN_CORRECTNESS" = true ]; then
    echo ""
    echo -e "${YELLOW}=== 运行正确性测试 ===${NC}"
    "$BUILD_DIR/correctness_test" --gtest_color=yes
fi

if [ "$RUN_PERF" = true ]; then
    echo ""
    echo -e "${YELLOW}=== 运行性能测试 ===${NC}"
    "$BUILD_DIR/performance_test"
fi
