#!/usr/bin/env bash
set -euo pipefail

# 同步 TrustSpan 的 hello_TS 测试工程到 optee_examples
# 每次同步都会删除目标 hello_TS 目录后重新完整复制

INVOKE_PWD="${PWD:-$(pwd)}"
SCRIPT_PATH="${BASH_SOURCE[0]}"
if [[ "${SCRIPT_PATH}" != /* ]]; then
	SCRIPT_PATH="${INVOKE_PWD}/${SCRIPT_PATH}"
fi

SCRIPT_DIR="$(cd "$(dirname "${SCRIPT_PATH}")" && pwd -P)"
TRUSTSPAN_DIR="$(cd "${SCRIPT_DIR}/.." && pwd -P)"
FVP_DIR="$(cd "${TRUSTSPAN_DIR}/../.." && pwd -P)/FVP/FVP-3.22.0"
SRC_DIR="${TRUSTSPAN_DIR}/hello_TS"
TARGET_ROOT_DIR="${FVP_DIR}/optee_examples"
TARGET_DIR="${TARGET_ROOT_DIR}/hello_TS"

if [[ ! -d "${SRC_DIR}" ]]; then
	echo "missing source directory: ${SRC_DIR}" >&2
	exit 1
fi

if [[ ! -d "${TARGET_ROOT_DIR}" ]]; then
	echo "missing target directory: ${TARGET_ROOT_DIR}" >&2
	exit 1
fi

rm -rf "${TARGET_DIR}"
cp -a "${SRC_DIR}" "${TARGET_DIR}"

echo "sync complete"
echo "source: ${SRC_DIR}"
echo "target: ${TARGET_DIR}"
