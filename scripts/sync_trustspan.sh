#!/usr/bin/env bash
set -euo pipefail

# ./CI/TrustSpan/scripts/sync_trustspan.sh linux
# ./CI/TrustSpan/scripts/sync_trustspan.sh optee_os
# ./CI/TrustSpan/scripts/sync_trustspan.sh linux /path/to/linux
# ./CI/TrustSpan/scripts/sync_trustspan.sh optee_os /path/to/optee_os
#
# 参数指定需要同步的源树
# 源目录固定为 CI/TrustSpan/<参数>
# 第二个参数可直接指定目标目录
# 未提供第二个参数时，按 TrustSpan 源目录的实际物理位置寻找 FVP/FVP-3.22.0/<参数>
# 该脚本只按相对路径复制当前源文件 不删除目标中的旧文件
# 每次成功后会记录本次复制的目标路径 供后续删除脚本使用

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
TRUSTSPAN_DIR="$(cd "${SCRIPT_DIR}/.." && pwd -L)"
TREE_NAME="$1"

if [[ "${TREE_NAME}" != "linux" && "${TREE_NAME}" != "optee_os" ]]; then
	echo "unsupported source tree: ${TREE_NAME}" >&2
	exit 1
fi

SRC_DIR="${TRUSTSPAN_DIR}/${TREE_NAME}"
DEFAULT_TARGET_DIR="$(cd "${TRUSTSPAN_DIR}/../.." && pwd -P)/FVP/FVP-3.22.0/${TREE_NAME}"
# 如果第二个参数存在且不为空 使用 ${2}
# 如果第二个参数不存在或为空 使用 ${DEFAULT_TARGET_DIR}
TARGET_DIR="${2:-${DEFAULT_TARGET_DIR}}"
MANIFEST_PATH="${SCRIPT_DIR}/.trustspan_sync_${TREE_NAME}_manifest"

if [[ "${TARGET_DIR}" != /* ]]; then
	TARGET_DIR="${INVOKE_PWD}/${TARGET_DIR}"
fi

if [[ ! -d "${SRC_DIR}" ]]; then
	echo "missing source directory: ${SRC_DIR}" >&2
	exit 1
fi

if [[ ! -d "${TARGET_DIR}" ]]; then
	echo "missing target directory: ${TARGET_DIR}" >&2
	exit 1
fi

TARGET_DIR_CANONICAL="$(cd "${TARGET_DIR}" && pwd -P)"

if [[ "$(cd "${SRC_DIR}" && pwd -P)" == "${TARGET_DIR_CANONICAL}" ]]; then
	echo "source and target directories must differ" >&2
	exit 1
fi

copied_rel_paths=()

while IFS= read -r -d '' src; do
	rel_path="${src#${SRC_DIR}/}"
	dst="${TARGET_DIR}/${rel_path}"
	mkdir -p "$(dirname "${dst}")"
	install -m 0644 "${src}" "${dst}"
	copied_rel_paths+=("${rel_path}")
	echo "copied: ${rel_path}"
done < <(find -H "${SRC_DIR}" -type f -print0 | sort -z)

manifest_tmp="$(mktemp "${MANIFEST_PATH}.tmp.XXXXXX")"
{
	printf '%s\n' "${TARGET_DIR_CANONICAL}"
	printf '%s\n' "${copied_rel_paths[@]}" | sort -u
} > "${manifest_tmp}"
mv -f "${manifest_tmp}" "${MANIFEST_PATH}"

echo "sync complete"
echo "source: ${SRC_DIR}"
echo "target: ${TARGET_DIR}"
echo "manifest: ${MANIFEST_PATH}"
