#!/usr/bin/env bash
set -euo pipefail

# 调用方式
# ./delete_trustspan.sh linux
# ./delete_trustspan.sh optee_os
# ./delete_trustspan.sh optee_os /path/to/optee_os
#
# 根据同步脚本生成的 manifest 删除已同步文件
# 不扫描目标目录 只删除 manifest 中记录的相对路径
# 第二个参数可用于确认目标目录是否与 manifest 记录一致

if [[ $# -lt 1 || $# -gt 2 ]]; then
	echo "usage: $0 <linux|optee_os> [target-directory]" >&2
	exit 1
fi

INVOKE_PWD="${PWD:-$(pwd)}"
SCRIPT_PATH="${BASH_SOURCE[0]}"
if [[ "${SCRIPT_PATH}" != /* ]]; then
	SCRIPT_PATH="${INVOKE_PWD}/${SCRIPT_PATH}"
fi

SCRIPT_DIR="$(cd "$(dirname "${SCRIPT_PATH}")" && pwd -P)"
TREE_NAME="$1"

if [[ "${TREE_NAME}" != "linux" && "${TREE_NAME}" != "optee_os" ]]; then
	echo "unsupported source tree: ${TREE_NAME}" >&2
	exit 1
fi

MANIFEST_PATH="${SCRIPT_DIR}/.trustspan_sync_${TREE_NAME}_manifest"
if [[ ! -f "${MANIFEST_PATH}" ]]; then
	echo "missing manifest: ${MANIFEST_PATH}" >&2
	exit 1
fi

TARGET_DIR="$(sed -n '1p' "${MANIFEST_PATH}")"
if [[ -z "${TARGET_DIR}" || ! -d "${TARGET_DIR}" ]]; then
	echo "invalid manifest target: ${TARGET_DIR}" >&2
	exit 1
fi

TARGET_DIR_CANONICAL="$(cd "${TARGET_DIR}" && pwd -P)"
if [[ $# -eq 2 ]]; then
	REQUESTED_TARGET="$2"
	if [[ "${REQUESTED_TARGET}" != /* ]]; then
		REQUESTED_TARGET="${INVOKE_PWD}/${REQUESTED_TARGET}"
	fi
	if [[ ! -d "${REQUESTED_TARGET}" ]]; then
		echo "missing target directory: ${REQUESTED_TARGET}" >&2
		exit 1
	fi
	if [[ "$(cd "${REQUESTED_TARGET}" && pwd -P)" != "${TARGET_DIR_CANONICAL}" ]]; then
		echo "target directory does not match manifest" >&2
		exit 1
	fi
fi

while IFS= read -r rel_path; do
	[[ -z "${rel_path}" ]] && continue
	if [[ "${rel_path}" == /* || "${rel_path}" == .. ||
	      "${rel_path}" == ../* || "${rel_path}" == *"/../"* ]]; then
		echo "invalid manifest path: ${rel_path}" >&2
		exit 1
	fi

	rm -f "${TARGET_DIR_CANONICAL}/${rel_path}"
	echo "removed: ${rel_path}"
done < <(sed -n '2,$p' "${MANIFEST_PATH}")

rm -f "${MANIFEST_PATH}"
echo "delete complete"
echo "target: ${TARGET_DIR_CANONICAL}"
