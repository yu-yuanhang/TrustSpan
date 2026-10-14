#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
build_root="${CONFINFER_BUILD_ROOT:-$(cd "$test_dir/../../.." && pwd)/FVP/FVP-3.22.0}"
source_dir="$build_root/trusted-firmware-a/plat/arm/common/span"
arm_common_dir="$build_root/trusted-firmware-a/plat/arm/common"
test_tmp="$(mktemp -d)"
trap 'rm -rf "$test_tmp"' EXIT
# 使用生产固定 pool 配置、访问切换和 SMC 状态机，桩只替换硬件及 EL3 ABI 边界。
awk '/^void span_tzc_boot_setup/ { copy = 1 } copy { print }' \
    "$arm_common_dir/arm_tzc400.c" | \
    awk '/^void arm_tzc400_setup/ { exit } { print }' \
    > "$test_tmp/tzc_boot_setup.inc"
awk '/^static bool span_range_within/ { copy = 1 } copy { print }' \
    "$source_dir/span_tzc.c" > "$test_tmp/tzc_transition.inc"
awk '/^void span_configure_region_access/ { copy = 1 } \
     /^void span_probe_region_attr/ { copy = 0 } copy { print }' \
    "$source_dir/span_probe.c" > "$test_tmp/tzc_access.inc"
awk '/^static spinlock_t span_tzc_lock/ { copy = 1 } copy { print }' \
    "$source_dir/span_sip_svc.c" > "$test_tmp/span_sip.inc"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
    -I"$test_tmp" -I"$source_dir" "$test_dir/tzc_transition_test.c" \
    -o "$test_tmp/tzc_transition_test"
"$test_tmp/tzc_transition_test"
