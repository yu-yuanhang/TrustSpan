#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_file="$test_dir/../optee_os/core/pta/span/span.c"
test_tmp="$(mktemp -d)"
trap 'rm -rf "$test_tmp"' EXIT
# 直接编译生产文件中的状态机，不在测试内另写一套模拟实现。
awk '/^#define SPAN_SHARED_MAX_ATTACHMENTS/ { copy = 1 }
     /^struct span_secure_phys_mobj/ { copy = 0 }
     copy { print }' "$source_file" > "$test_tmp/span_lifecycle.inc"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-unused-function -pthread \
    -I"$test_tmp" "$test_dir/span_lifecycle_test.c" -o "$test_tmp/span_lifecycle_test"
"$test_tmp/span_lifecycle_test"
