#!/usr/bin/env bash
set -euo pipefail


# 依次处理 linux 和 optee_os
# 每个目录先删除上次同步记录的文件 再同步当前源文件
# 最后同步 hello_TS 测试工程

if [[ $# -ne 0 ]]; then
	echo "usage: $0" >&2
	exit 1
fi

INVOKE_PWD="${PWD:-$(pwd)}"
SCRIPT_PATH="${BASH_SOURCE[0]}"
if [[ "${SCRIPT_PATH}" != /* ]]; then
	SCRIPT_PATH="${INVOKE_PWD}/${SCRIPT_PATH}"
fi

SCRIPT_DIR="$(cd "$(dirname "${SCRIPT_PATH}")" && pwd -P)"

for tree_name in linux optee_os; do
	manifest_path="${SCRIPT_DIR}/.trustspan_sync_${tree_name}_manifest"

	if [[ -f "${manifest_path}" ]]; then
		"${SCRIPT_DIR}/delete_trustspan.sh" "${tree_name}"
	fi
	"${SCRIPT_DIR}/sync_trustspan.sh" "${tree_name}"
done

# The FVP board keeps its generic DTS in the destination Linux tree. A path
# overlay can only reference a node already parsed by dtc, so keep the owned
# TrustSpan fragment at the end, after the board's reserved-memory node.
linux_root="$(cd "${SCRIPT_DIR}/../../../FVP/FVP-3.22.0/linux" && pwd -P)"
fvp_dts="${linux_root}/arch/arm64/boot/dts/arm/fvp-base-revc.dts"
sed -i '/^#include "trustspan-fvp\.dtsi"$/d' "${fvp_dts}"
printf '\n#include "trustspan-fvp.dtsi"\n' >> "${fvp_dts}"

"${SCRIPT_DIR}/sync_hello_ts.sh"

echo "TrustSpan resync complete"
