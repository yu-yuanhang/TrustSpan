/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_MISC_TRUSTSPAN_MEM_TEST_H
#define _UAPI_MISC_TRUSTSPAN_MEM_TEST_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * 用户态和内核态共用的数据结构：
 * - phys_addr：分配结果对应的物理地址
 * - size：用户请求的大小，或内核实际分配后的大小
 * - allocated：当前是否已经完成分配
 */
struct trustspan_mem_info {
	__u64 phys_addr;
	__u64 size;
	__u32 allocated;
	__u32 state;
};

enum trustspan_mem_state {
	TRUSTSPAN_MEM_STATE_IDLE = 0,
	TRUSTSPAN_MEM_STATE_ALLOCATED = 1,	// 驱动已分配物理页 REE 可以 mmap 并读写
	TRUSTSPAN_MEM_STATE_MAPPED = 2,		// REE 当前已经 mmap 这段物理页
	// REE 已经 munmap 驱动认为这段页正在交给 Secure world 
	// 此时拒绝新的 mmap 和 FREE
	TRUSTSPAN_MEM_STATE_PREPARED = 3,
	// 	TA 已成功通过 TF-A 保护并使用这段页 REE 不可 mmap 不可 FREE
	TRUSTSPAN_MEM_STATE_PROTECTED = 4,
};

/*
 * ioctl 编码说明：
 * - ALLOC：用户态传入 size，内核返回 phys addr + 实际 size
 * - QUERY：查询当前分配状态
 * - FREE：释放当前分配
 * - PREPARE_SECURE：确认用户态映射已退出，准备交给 secure world
 * - MARK_PROTECTED：标记已经完成 protect，Linux 不再按普通页使用
 * - RELEASE_SECURE：确认 secure world 已归还，恢复为可释放状态
 * - CANCEL_SECURE：prepare 失败时取消 secure handoff，恢复为可重新映射状态
 */
#define TRUSTSPAN_MEM_TEST_IOC_MAGIC 'T'
#define TRUSTSPAN_MEM_TEST_IOC_ALLOC \
	_IOWR(TRUSTSPAN_MEM_TEST_IOC_MAGIC, 0x01, struct trustspan_mem_info)
#define TRUSTSPAN_MEM_TEST_IOC_QUERY \
	_IOR(TRUSTSPAN_MEM_TEST_IOC_MAGIC, 0x02, struct trustspan_mem_info)
#define TRUSTSPAN_MEM_TEST_IOC_FREE \
	_IO(TRUSTSPAN_MEM_TEST_IOC_MAGIC, 0x03)
#define TRUSTSPAN_MEM_TEST_IOC_PREPARE_SECURE \
	_IO(TRUSTSPAN_MEM_TEST_IOC_MAGIC, 0x04)
#define TRUSTSPAN_MEM_TEST_IOC_MARK_PROTECTED \
	_IO(TRUSTSPAN_MEM_TEST_IOC_MAGIC, 0x05)
#define TRUSTSPAN_MEM_TEST_IOC_RELEASE_SECURE \
	_IO(TRUSTSPAN_MEM_TEST_IOC_MAGIC, 0x06)
#define TRUSTSPAN_MEM_TEST_IOC_CANCEL_SECURE \
	_IO(TRUSTSPAN_MEM_TEST_IOC_MAGIC, 0x07)

#endif /* _UAPI_MISC_TRUSTSPAN_MEM_TEST_H */
