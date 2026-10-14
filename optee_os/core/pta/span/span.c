/* SPDX-License-Identifier: BSD-2-Clause */

#include <inttypes.h>
#include <kernel/refcount.h>
#include <kernel/mutex.h>
#include <kernel/pseudo_ta.h>
#include <kernel/span.h>
#include <kernel/thread_arch.h>
#include <kernel/ts_manager.h>
#include <kernel/user_mode_ctx.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <mm/mobj.h>
#include <mm/vm.h>
#include <string_ext.h>
#include <trace.h>
#include <util.h>

#include <pta_span.h>
#include <sm/trustspan_smc.h>

#include "span.h"

#define PTA_NAME "span.pta"

#ifndef CFG_CONFINFER_SHARED_SPAN_VA
#define CFG_CONFINFER_SHARED_SPAN_VA	0
#endif
#ifndef CFG_CONFINFER_SHARED_SPAN_SIZE
#define CFG_CONFINFER_SHARED_SPAN_SIZE	0
#endif

static struct mobj *span_secure_phys_alloc(paddr_t pa, size_t size,
					   uint32_t mem_type);
static TEE_Result span_call_tfa_protect(paddr_t phys_addr, size_t size);
static TEE_Result span_call_tfa_unprotect(paddr_t phys_addr, size_t size);

// ################################################################################
// # for ConfInfer Modified by yyh : 2026 - Sep 17
// ################################################################################
/*
 * 阶段 5.0 的共享 TrustSpan 注册表。
 *
 * 当前 FVP 的 CFG_NUM_THREADS=2，因此本轮只维护一个共享 region：
 * controller 和一个 worker 各有独立 User TA 上下文、独立页表和安全
 * 执行槽，但其页表把同一物理区映射到同一固定 VA。注册表是这份“同一
 * 物理区、同一 VA、多个 uctx attachment”契约的唯一可信来源。
 *
 * 调用链：
 * ConfInfer CA -> ConfInfer User TA -> Span PTA 的
 * CREATE_PROTECT / FREEZE_LAYOUT / ATTACH / DETACH / RELEASE_SHARED 命令。
 * CREATE 只接收 controller 给出的物理地址和大小；之后 ATTACH 只接收
 * handle，所有 worker 映射参数均从本注册表读取。
 *
 * 状态机：
 * EMPTY -> CREATING -> ACTIVE -> RELEASING -> EMPTY。
 * 每个 attachment 独立经历 EMPTY -> ATTACHING -> ATTACHED ->
 * DETACHING 或 DYING -> EMPTY。span_shared_lock 保护注册表和所有
 * attachment 状态；释放锁后才执行 vm_map()、vm_unmap() 和 TF-A SMC，
 * 避免在可能阻塞的操作期间持锁。
 *
 * 此路径与下方旧的单 owner PROTECT/RELEASE 路径相互独立；旧路径不支持
 * 多个隔离 User TA 共享同一固定 VA。
 *
 * =================================================================
 *
 * 创建流程：CREATE_PROTECT 调用链：
 * REE CA
 * -> ConfInfer User TA
 *    -> Span PTA
 *        -> pta_span_shared_create()
 *            -> span_shared_create()
 *                -> TF-A protect
 *                -> vm_map()
 * pta_span_shared_create() 接收：
 * 	value[0] = 物理地址
 * 	value[1] = 区域大小
 * 只有 controller 可以在这个阶段提交物理地址
 *
 * 随后 span_shared_create()：
 *   1. 检查当前是否已经存在其他共享 region；若存在则返回 BUSY；
 *   2. 生成新的 region handle，并把物理地址、大小和固定 VA 写入
 *      Span PTA 的可信注册表；
 *   3. 将 region 标记为 CREATING，防止 worker 在保护和映射完成前
 *      提前调用 ATTACH；
 *   4. 释放注册表锁，调用 TF-A protect，把物理区从 REE 所有权
 *      转移到安全世界；
 *   5. 调用 span_try_map_shared_to_ta()，该函数再调用 vm_map()
 *      并携带 VM_FLAG_CONFINFER_SHARED_SPAN，把 controller 的
 *      页表映射到固定 VA；
 *   6. 映射成功后把 attachment[0] 记录为 controller，并将 region
 *      状态改为 ACTIVE；
 *   7. 返回 handle 和 controller 使用的固定 VA。
 *
 * 如果 TF-A protect 成功但 vm_map() 失败，则先执行 unprotect，成功后
 * 才清空注册表；归还失败保留 RELEASING 和 controller 身份供后续回收。
 * CREATE 成功返回后，worker 只需要提交 handle，不能再提交物理地址、
 * 区域大小或虚拟地址。
 */
#define SPAN_SHARED_MAX_ATTACHMENTS	CFG_NUM_THREADS

/*
 *	EMPTY
 *	| CREATE_PROTECT
 *	CREATING
 *	| protect/map 成功
 *	ACTIVE
 *	| RELEASE_SHARED 或 controller 销毁
 *	RELEASING
 *	| unmap/unprotect 成功
 *	EMPTY
 */
enum span_shared_region_state {
	SPAN_SHARED_REGION_EMPTY = 0,	// 当前没有有效共享 region
	SPAN_SHARED_REGION_CREATING,	// region 已经登记到全局表，但还没有完成完整创建流程
	SPAN_SHARED_REGION_ACTIVE,		// 表示 region 已经完整可用
	SPAN_SHARED_REGION_RELEASING,	// 表示 region 正在销毁，已经不允许建立新的 worker attachment
};
/*
 * 单个 User TA attachment 的生命周期状态。
 * DYING 不由显式 DETACH 设置，而是由 user_ta.c 的 free_utc() 在 TA
 * 销毁前调用 span_uctx_destroying() 设置；随后 span_uctx_destroyed()
 * 在页表撤销后清除该记录。
 */
enum span_shared_attachment_state {
	SPAN_SHARED_ATTACHMENT_EMPTY = 0,
	SPAN_SHARED_ATTACHMENT_ATTACHING,
	SPAN_SHARED_ATTACHMENT_ATTACHED,
	SPAN_SHARED_ATTACHMENT_DETACHING,
	SPAN_SHARED_ATTACHMENT_DYING,
};
/*
 * 每个 attachment 绑定一个 uctx 与该 uctx 页表中的固定 VA。
 * is_controller 区分 region 创建者和 worker：只有 controller 可以
 * RELEASE_SHARED，controller 不能调用 DETACH。
 */
struct span_shared_attachment {
	struct user_mode_ctx *uctx;
	vaddr_t va;
	enum span_shared_attachment_state state;
	bool is_controller;
};
/*
 * 单一共享 region 的可信元数据。物理地址、大小和 shared_va 只在
 * CREATE_PROTECT 时写入；ATTACH 不接受这些参数，只读取此结构。
 * owner_uuid 将 handle 绑定为 ConfInfer TA 专用，防止其他 User TA
 * 使用猜测到的 handle 附着该区域。
 */
struct span_shared_region {
	paddr_t phys_addr;
	size_t size;
	vaddr_t shared_va;
	uint32_t handle;
	TEE_UUID owner_uuid;
	enum span_shared_region_state state;
	bool controller_map_removed;
	bool unprotect_in_progress;
	bool layout_freezing;
	bool layout_frozen;
	size_t static_span_size;
	size_t runtime_span_offset;
	size_t runtime_span_size;
	struct span_shared_attachment attachments[SPAN_SHARED_MAX_ATTACHMENTS];
};

static struct mutex span_shared_lock = MUTEX_INITIALIZER;
static struct span_shared_region span_shared_region;
static uint32_t span_shared_next_handle;
/* 由 STATUS probe 读取；计数跨 region 生命周期保留，均在锁内更新。 */
static uint32_t span_shared_protect_calls;
static uint32_t span_shared_unprotect_calls;

// 当前这个 span 被设计为仅仅接收 来自 ConfInfer User TA 的 PTA session
// 这里直接固定 用作检测
static const TEE_UUID span_confinfer_uuid = {
	0x7dd54ee6, 0x2f13, 0x4f1d,
	{ 0xa3, 0xe6, 0x0f, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f }
};

static bool span_shared_window_is_valid(size_t region_size)
{
#if defined(CFG_CONFINFER_SHARED_SPAN_VA) && \
	defined(CFG_CONFINFER_SHARED_SPAN_SIZE)
	vaddr_t user_va_base = 0;
	size_t user_va_size = 0;
	vaddr_t user_va_end = 0;
	vaddr_t shared_va_end = 0;

	/*
	 * CREATE_PROTECT 创建流程在这里完成第一道窗口检查：
	 *
	 * REE CA
	 *   -> ConfInfer User TA
	 *      -> Span PTA
	 *         -> pta_span_shared_create()
	 *            -> span_shared_create()
	 *               -> span_shared_window_is_valid()
	 *
	 * span_shared_window_is_valid() 只检查固定 VA 配置、页对齐、请求
	 * 大小和 user-TA VA 范围，不建立映射，也不调用 TF-A。检查失败时
	 * 直接返回 BAD_PARAMETERS，后续 protect 和 vm_map 都不会发生。
	 */
	if (!region_size || !CFG_CONFINFER_SHARED_SPAN_VA ||
	    !CFG_CONFINFER_SHARED_SPAN_SIZE ||
	    !IS_ALIGNED(CFG_CONFINFER_SHARED_SPAN_VA, SMALL_PAGE_SIZE) ||
	    !IS_ALIGNED(CFG_CONFINFER_SHARED_SPAN_SIZE, SMALL_PAGE_SIZE) ||
	    region_size > CFG_CONFINFER_SHARED_SPAN_SIZE)
		return false;

	core_mmu_get_user_va_range(&user_va_base, &user_va_size);
	if (ADD_OVERFLOW(user_va_base, user_va_size, &user_va_end) ||
	    ADD_OVERFLOW(CFG_CONFINFER_SHARED_SPAN_VA,
			 CFG_CONFINFER_SHARED_SPAN_SIZE, &shared_va_end))
		return false;

	return CFG_CONFINFER_SHARED_SPAN_VA >= user_va_base &&
	       shared_va_end <= user_va_end;
#else
	(void)region_size;
	return false;
#endif
}

static bool span_is_confinfer_caller(struct ts_session *session)
{
	/*
	 * span_get_confinfer_uctx() 中被调用
	 * 共享命令只接受来自 ConfInfer User TA UUID 的 PTA session；
	 * 普通 TA 即使知道 PTA UUID、命令号或 region handle，也会被拒绝。
	 */
	return session && is_user_ta_ctx(session->ctx)
		&& !memcmp(&session->ctx->uuid, &span_confinfer_uuid,
		    sizeof(span_confinfer_uuid));
}

static TEE_Result span_get_confinfer_uctx(struct user_mode_ctx **uctx)
{
	struct ts_session *session = ts_get_calling_session();

	/*
	 * 调用者：CREATE_PROTECT、ATTACH、DETACH、FREEZE_LAYOUT、
	 * RELEASE_SHARED 五个共享生命周期命令，以及只读平台探测命令。
	 * 从当前 PTA 调用 session 取得实际 User TA 上下文，而不是接收
	 * REE 传入的上下文或 VA。后续 CREATE/ATTACH/DETACH/FREEZE/RELEASE
	 * 都以此 uctx 为 attachment 身份；身份或 UUID 不匹配时返回
	 * ACCESS_DENIED。
	 */
	if (!uctx || !span_is_confinfer_caller(session))
		return TEE_ERROR_ACCESS_DENIED;

	*uctx = to_user_mode_ctx(session->ctx);
	return TEE_SUCCESS;
}

static unsigned int span_shared_find_attachment_locked(
	struct user_mode_ctx *uctx)
{
	unsigned int n = 0;

	/*
	 * 调用者：ATTACH、DETACH、RELEASE_SHARED 与两个 uctx 销毁钩子。
	 * 调用时必须已持有 span_shared_lock。返回 attachment 下标；没有
	 * 匹配项时返回 SPAN_SHARED_MAX_ATTACHMENTS，而不是暴露指针给调用者。
	 */
	for (n = 0; n < SPAN_SHARED_MAX_ATTACHMENTS; n++) {
		if (span_shared_region.attachments[n].state !=
		    SPAN_SHARED_ATTACHMENT_EMPTY &&
		    span_shared_region.attachments[n].uctx == uctx)
			return n;
	}

	return SPAN_SHARED_MAX_ATTACHMENTS;
}

static unsigned int span_shared_find_empty_attachment_locked(void)
{
	unsigned int n = 0;

	/*
	 * 调用者：span_shared_attach()。在锁内寻找可供新 worker 使用的
	 * attachment 槽；所有槽已占用时 ATTACH 返回 BUSY。上限等于
	 * CFG_NUM_THREADS，不能因为 REE 创建更多线程而超出安全执行槽。
	 */
	for (n = 0; n < SPAN_SHARED_MAX_ATTACHMENTS; n++) {
		if (span_shared_region.attachments[n].state ==
		    SPAN_SHARED_ATTACHMENT_EMPTY)
			return n;
	}

	return SPAN_SHARED_MAX_ATTACHMENTS;
}

static bool span_shared_has_worker_locked(void)
{
	unsigned int n = 0;

	/*
	 * 调用者：span_shared_release()。controller 请求释放时，只要有
	 * 非 controller 的 attachment（包括 ATTACHING、ATTACHED、
	 * DETACHING 或 DYING）就返回 BUSY，防止解除保护时 worker 仍可能
	 * 使用该物理区。调用时必须持有 span_shared_lock。
	 */
	for (n = 0; n < SPAN_SHARED_MAX_ATTACHMENTS; n++) {
		struct span_shared_attachment *attachment =
			&span_shared_region.attachments[n];

		if (attachment->state != SPAN_SHARED_ATTACHMENT_EMPTY &&
		    !attachment->is_controller)
			return true;
	}

	return false;
}

static void span_shared_reset_locked(void)
{
	/*
	 * 调用者：
	 * - CREATE 失败后的回滚；
	 * - controller 正常 RELEASE_SHARED 成功；
	 * - controller 或最后一个 worker 异常销毁并完成 unprotect。
	 *
	 * 必须持有 span_shared_lock。清零后旧 handle、物理地址和 attachment
	 * 都不可再用；next_handle 不清零，以降低旧 handle 被立即复用的风险。
	 */
	memset(&span_shared_region, 0, sizeof(span_shared_region));
}

static void span_shared_clear_attachment_locked(unsigned int index)
{
	/*
	 * 调用者：ATTACH 映射失败、DETACH 成功、以及 uctx 销毁后的收尾。
	 * 必须持有 span_shared_lock。仅清除指定槽，不改变 region 状态；
	 * region 是否可 unprotect 由 span_shared_finish_release() 统一判断。
	 */
	if (index < SPAN_SHARED_MAX_ATTACHMENTS)
		memset(&span_shared_region.attachments[index], 0,
		       sizeof(span_shared_region.attachments[index]));
}


/*
 * 调用者：RELEASE、CREATE 回滚、ATTACH 回滚、DETACH 和 destroyed hook。
 * 本函数自行持锁检查最后映射是否消失；unprotect_in_progress 在锁内领取
 * 唯一收尾权，防止两个 context 的销毁钩子在锁外重复调用 TF-A。
 * controller 已 unmap 的记录可以暂留，用于 unprotect 失败后的 RELEASE 重试。
 * 失败保留注册表与物理范围，绝不把仍受保护的页误报为 EMPTY。
 */
// ################################################################################
// # for ConfInfer Modified by yyh : 2026 - Sep 20
// ################################################################################
static TEE_Result span_shared_finish_release(void)
{
	TEE_Result res = TEE_SUCCESS;
	paddr_t pa = 0;
	size_t size = 0;
	unsigned int n = 0;

	mutex_lock(&span_shared_lock);
	if (span_shared_region.state != SPAN_SHARED_REGION_RELEASING) {
		mutex_unlock(&span_shared_lock);
		return TEE_SUCCESS;
	}
	for (n = 0; n < SPAN_SHARED_MAX_ATTACHMENTS; n++) {
		struct span_shared_attachment *a =
			&span_shared_region.attachments[n];

		if (a->state != SPAN_SHARED_ATTACHMENT_EMPTY &&
		    !(a->is_controller && span_shared_region.controller_map_removed)) {
			mutex_unlock(&span_shared_lock);
			return TEE_SUCCESS;
		}
	}
	if (span_shared_region.unprotect_in_progress) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_BUSY;
	}
	span_shared_region.unprotect_in_progress = true;
	span_shared_unprotect_calls++;
	pa = span_shared_region.phys_addr;
	size = span_shared_region.size;
	mutex_unlock(&span_shared_lock);

	res = span_call_tfa_unprotect(pa, size);
	mutex_lock(&span_shared_lock);
	if (!res)
		span_shared_reset_locked();
	else
		span_shared_region.unprotect_in_progress = false;
	mutex_unlock(&span_shared_lock);
	return res;
}

static TEE_Result span_map_shared_slice(struct user_mode_ctx *uctx,
					struct mobj *mobj, size_t mobj_offset,
					size_t size, vaddr_t requested_va,
					uint32_t prot)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	vaddr_t map_va = requested_va;

	/*
	 * 调用者：span_try_map_shared_to_ta()。
	 *
	 * mobj 始终覆盖完整共享 region；本函数只使用 mobj_offset 选择其中
	 * 一个页对齐切片。这样 static/runtime 虽然在页表中是两个权限不同的
	 * vm_region，但仍引用同一个物理对象，不会产生两份相互独立的物理
	 * 身份。requested_va 必须原样返回，禁止 VM allocator 改选地址。
	 */
	if (!uctx || !mobj || !requested_va || !size ||
	    !IS_ALIGNED(mobj_offset, SMALL_PAGE_SIZE) ||
	    !IS_ALIGNED(size, SMALL_PAGE_SIZE) ||
	    !mobj_check_offset_and_len(mobj, mobj_offset, size) ||
	    (prot != (TEE_MATTR_UR | TEE_MATTR_PR) &&
	     prot != (TEE_MATTR_URW | TEE_MATTR_PRW)))
		return TEE_ERROR_BAD_PARAMETERS;

	res = vm_map(uctx, &map_va, size, prot,
		     VM_FLAG_SHAREABLE | VM_FLAG_CONFINFER_SHARED_SPAN,
		     mobj, mobj_offset);
	if (res)
		return res;
	if (map_va != requested_va) {
		(void)vm_unmap(uctx, map_va, size);
		return TEE_ERROR_ACCESS_CONFLICT;
	}

	return TEE_SUCCESS;
}

static TEE_Result span_try_map_shared_to_ta(struct user_mode_ctx *uctx,
					     paddr_t pa, size_t size,
					     vaddr_t requested_va,
					     bool layout_frozen,
					     size_t static_span_size,
					     size_t runtime_span_offset,
					     size_t runtime_span_size,
					     vaddr_t *va)
{
	struct mobj *mobj = NULL;
	TEE_Result res = TEE_SUCCESS;
	vaddr_t runtime_va = 0;

	/*
	 * CREATE 阶段尚未验证 Image，因此 controller 先获得整段可写映射。
	 * FREEZE_LAYOUT 后，worker attachment 必须按可信注册表中的页边界拆成
	 * static 只读和 runtime 可写两个相邻映射；VA 仍与 controller 完全一致。
	 */
	if (!uctx || !va || !requested_va || !size ||
	    !IS_ALIGNED(pa, SMALL_PAGE_SIZE) ||
	    !IS_ALIGNED(size, SMALL_PAGE_SIZE))
		return TEE_ERROR_BAD_PARAMETERS;
	mobj = span_secure_phys_alloc(pa, size, TEE_MATTR_MEM_TYPE_CACHED);
	if (!mobj)
		return TEE_ERROR_OUT_OF_MEMORY;
	if (!layout_frozen) {
		res = span_map_shared_slice(uctx, mobj, 0, size, requested_va,
					    TEE_MATTR_URW | TEE_MATTR_PRW);
		if (!res)
			*va = requested_va;
		mobj_put(mobj);
		return res;
	}
	if (!static_span_size || !runtime_span_size ||
	    static_span_size != runtime_span_offset ||
	    static_span_size > size || runtime_span_size > size - runtime_span_offset ||
	    runtime_span_offset + runtime_span_size != size ||
	    ADD_OVERFLOW(requested_va, runtime_span_offset, &runtime_va) ||
	    !IS_ALIGNED(static_span_size, SMALL_PAGE_SIZE) ||
	    !IS_ALIGNED(runtime_span_offset, SMALL_PAGE_SIZE) ||
	    !IS_ALIGNED(runtime_span_size, SMALL_PAGE_SIZE))
		res = TEE_ERROR_BAD_PARAMETERS;
	else
		res = span_map_shared_slice(uctx, mobj, 0, static_span_size,
					    requested_va,
					    TEE_MATTR_UR | TEE_MATTR_PR);

	if (res)
		goto out;
	res = span_map_shared_slice(uctx, mobj, runtime_span_offset,
				    runtime_span_size, runtime_va,
				    TEE_MATTR_URW | TEE_MATTR_PRW);
	if (res) {
		(void)vm_unmap(uctx, requested_va, static_span_size);
		goto out;
	}
	*va = requested_va;
out:
	mobj_put(mobj);
	return res;
}

static TEE_Result span_shared_create(struct user_mode_ctx *uctx,
				     paddr_t phys_addr, size_t size,
				     uint32_t *handle, vaddr_t *shared_va)
{
	TEE_Result res = TEE_SUCCESS;
	vaddr_t mapped_va = 0;
	uint32_t new_handle = 0;
	TEE_Result rollback_res = TEE_SUCCESS;

	/*
	 * CREATE_PROTECT 的完整执行流程：
	 *
	 * REE CA
	 *   -> ConfInfer User TA
	 *      -> Span PTA
	 *         -> pta_span_shared_create()
	 *            -> span_shared_create()
	 *               -> TF-A protect
	 *                  -> span_try_map_shared_to_ta()
	 *                     -> vm_map()
	 *
	 * pta_span_shared_create() 接收：
	 *   params[0].value = 物理地址 phys_addr
	 *   params[1].value = 区域大小 size
	 *
	 * 只有 controller 在 CREATE_PROTECT 阶段提交物理地址。进入本函数
	 * 前，pta_span_shared_create() 已经完成以下检查：
	 *   1. 当前 session 属于 ConfInfer User TA；
	 *   2. 参数类型符合两个 VALUE_INPUT 和两个 VALUE_OUTPUT；
	 *   3. 物理区当前仍属于 non-secure buffer。
	 *
	 * 本函数随后按以下顺序运行：
	 *   1. 检查 span_shared_region 是否为空；已有 region 则返回 BUSY；
	 *   2. 生成新的不透明 handle，并登记物理地址、大小、固定 VA；
	 *   3. 将 region 状态设置为 CREATING，防止其他 worker 提前 ATTACH；
	 *   4. 释放 span_shared_lock，调用 TF-A protect；
	 *   5. 调用 span_try_map_shared_to_ta()，以固定 VA 建立 controller
	 *      的用户页表映射。该函数内部通过 vm_map() 使用
	 *      VM_FLAG_CONFINFER_SHARED_SPAN；
	 *   6. 映射成功后重新加锁，将 attachment[0] 记录为 controller；
	 *   7. 将 region 状态改为 ACTIVE；
	 *   8. 返回 handle 和 shared_va。
	 *
	 * 成功返回值：
	 *   params[2].value = region_handle
	 *   params[3].value = controller 使用的固定 VA
	 *
	 * 失败回滚：
	 *   - TF-A protect 失败：直接清空 CREATING 注册表；
	 *   - protect 成功但 vm_map() 失败：先调用 TF-A unprotect，成功后才清空
	 *     注册表；失败保留 RELEASING 以便重试；
	 *   - 任意回滚失败都不会把该 region 标记为 ACTIVE。
	 *
	 * 返回的 handle 是后续 ATTACH、DETACH 和 RELEASE_SHARED 的唯一
	 * 标识。worker 后续只提交该 handle，不再提交物理地址或虚拟地址。
	 */
	if (!uctx || !handle || !shared_va ||
	    !span_shared_window_is_valid(size))
		return TEE_ERROR_BAD_PARAMETERS;

	mutex_lock(&span_shared_lock);
	if (span_shared_region.state != SPAN_SHARED_REGION_EMPTY) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_BUSY;
	}

	new_handle = ++span_shared_next_handle;
	if (!new_handle)
		new_handle = ++span_shared_next_handle;
	span_shared_region.phys_addr = phys_addr;
	span_shared_region.size = size;
	span_shared_region.shared_va = CFG_CONFINFER_SHARED_SPAN_VA;
	span_shared_region.handle = new_handle;
	span_shared_region.owner_uuid = span_confinfer_uuid;
	span_shared_region.state = SPAN_SHARED_REGION_CREATING;
	span_shared_protect_calls++;
	mutex_unlock(&span_shared_lock);

	res = span_call_tfa_protect(phys_addr, size);
	if (res)
		goto out_reset;
	res = span_try_map_shared_to_ta(uctx, phys_addr, size,
					span_shared_region.shared_va, false,
					0, 0, 0, &mapped_va);
	if (res)
		goto out_unprotect;

	mutex_lock(&span_shared_lock);
	span_shared_region.attachments[0].uctx = uctx;
	span_shared_region.attachments[0].va = mapped_va;
	span_shared_region.attachments[0].state = SPAN_SHARED_ATTACHMENT_ATTACHED;
	span_shared_region.attachments[0].is_controller = true;
	span_shared_region.state = SPAN_SHARED_REGION_ACTIVE;
	mutex_unlock(&span_shared_lock);

	*handle = new_handle;
	*shared_va = mapped_va;
	return TEE_SUCCESS;

out_unprotect:
	/* 保留 controller 身份，以便失败回滚或 context 销毁继续收尾。 */
	mutex_lock(&span_shared_lock);
	span_shared_region.attachments[0].uctx = uctx;
	span_shared_region.attachments[0].is_controller = true;
	span_shared_region.attachments[0].state = SPAN_SHARED_ATTACHMENT_ATTACHED;
	span_shared_region.controller_map_removed = true;
	span_shared_region.state = SPAN_SHARED_REGION_RELEASING;
	mutex_unlock(&span_shared_lock);
	rollback_res = span_shared_finish_release();
	return rollback_res ? rollback_res : res;
out_reset:
	mutex_lock(&span_shared_lock);
	span_shared_reset_locked();
	mutex_unlock(&span_shared_lock);
	return res;
}

static TEE_Result span_shared_attach(struct user_mode_ctx *uctx,
				     uint32_t handle, vaddr_t *shared_va,
				     size_t *size)
{
	TEE_Result res = TEE_SUCCESS;
	paddr_t phys_addr = 0;
	size_t region_size = 0;
	vaddr_t requested_va = 0;
	vaddr_t mapped_va = 0;
	bool layout_frozen = false;
	size_t static_span_size = 0;
	size_t runtime_span_offset = 0;
	size_t runtime_span_size = 0;
	unsigned int index = SPAN_SHARED_MAX_ATTACHMENTS;

	/*
	 * 调用者：pta_span_shared_attach()，即 worker 的 ATTACH 命令。
	 *
	 * worker 只传 handle。本函数在锁内验证 ACTIVE 状态、handle 和
	 * ConfInfer owner UUID，拒绝重复 attachment；随后从注册表复制
	 * pa/size/fixed VA，预占一个 ATTACHING 槽并释放锁执行 vm_map()。
	 * 映射完成后再次持锁复核状态和槽归属，防止并发 RELEASE 或 uctx
	 * 销毁期间将映射误登记。复核失败时撤销刚建立的映射。
	 *
	 * 成功后该 worker 的页表拥有与 controller 相同的 VA，返回 VA 与
	 * size 仅用于 CA probe 交叉验证，不作为后续映射输入。
	 */
	if (!uctx || !handle || !shared_va || !size)
		return TEE_ERROR_BAD_PARAMETERS;

	mutex_lock(&span_shared_lock);
	if (span_shared_region.state != SPAN_SHARED_REGION_ACTIVE ||
	    span_shared_region.handle != handle ||
	    memcmp(&span_shared_region.owner_uuid, &span_confinfer_uuid,
		   sizeof(span_confinfer_uuid))) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_ITEM_NOT_FOUND;
	}
	if (span_shared_region.layout_freezing) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_BUSY;
	}
	if (span_shared_find_attachment_locked(uctx) !=
	    SPAN_SHARED_MAX_ATTACHMENTS) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_ACCESS_CONFLICT;
	}
	index = span_shared_find_empty_attachment_locked();
	if (index == SPAN_SHARED_MAX_ATTACHMENTS) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_BUSY;
	}

	phys_addr = span_shared_region.phys_addr;
	region_size = span_shared_region.size;
	requested_va = span_shared_region.shared_va;
	layout_frozen = span_shared_region.layout_frozen;
	static_span_size = span_shared_region.static_span_size;
	runtime_span_offset = span_shared_region.runtime_span_offset;
	runtime_span_size = span_shared_region.runtime_span_size;
	span_shared_region.attachments[index].uctx = uctx;
	span_shared_region.attachments[index].state =
		SPAN_SHARED_ATTACHMENT_ATTACHING;
	mutex_unlock(&span_shared_lock);

	res = span_try_map_shared_to_ta(uctx, phys_addr, region_size,
					requested_va, layout_frozen,
					static_span_size, runtime_span_offset,
					runtime_span_size, &mapped_va);

	mutex_lock(&span_shared_lock);
	if (res || span_shared_region.state != SPAN_SHARED_REGION_ACTIVE ||
	    span_shared_region.handle != handle ||
	    span_shared_region.attachments[index].uctx != uctx ||
	    span_shared_region.attachments[index].state !=
		SPAN_SHARED_ATTACHMENT_ATTACHING) {
		TEE_Result unmap_res = TEE_SUCCESS;

		/* 槽位必须保留到 unmap 完成，否则最后销毁者可能提前 unprotect。 */
		span_shared_region.attachments[index].va = mapped_va;
		span_shared_region.attachments[index].state =
			SPAN_SHARED_ATTACHMENT_DETACHING;
		mutex_unlock(&span_shared_lock);
		if (!res)
			unmap_res = vm_unmap(uctx, mapped_va, region_size);
		mutex_lock(&span_shared_lock);
		if (!unmap_res)
			span_shared_clear_attachment_locked(index);
		else
			span_shared_region.attachments[index].state =
				SPAN_SHARED_ATTACHMENT_ATTACHED;
		mutex_unlock(&span_shared_lock);
		(void)span_shared_finish_release();
		return res ? res : TEE_ERROR_ACCESS_CONFLICT;
	}

	span_shared_region.attachments[index].va = mapped_va;
	span_shared_region.attachments[index].state =
		SPAN_SHARED_ATTACHMENT_ATTACHED;
	mutex_unlock(&span_shared_lock);

	*shared_va = mapped_va;
	*size = region_size;
	return TEE_SUCCESS;
}

static TEE_Result span_shared_freeze_layout(struct user_mode_ctx *uctx,
					     uint32_t handle,
					     size_t static_span_size,
					     size_t runtime_span_offset,
					     size_t runtime_span_size)
{
	TEE_Result res = TEE_SUCCESS;
	vaddr_t shared_va = 0;
	unsigned int index = SPAN_SHARED_MAX_ATTACHMENTS;

	/*
	 * 调用者：pta_span_shared_freeze_layout()。
	 *
	 * ConfInfer controller 只有在 TEE 已经完成 Image 边界校验和参数原地
	 * packing 后才调用本函数。函数在锁内重新核对：调用者确为 controller、
	 * region 仍为 ACTIVE、尚无 worker attachment，并且 static/runtime
	 * 两段恰好覆盖 CREATE_PROTECT 登记的完整物理区。随后设置
	 * layout_freezing，阻止 ATTACH/RELEASE 在 vm_set_prot() 的锁外窗口进入。
	 *
	 * vm_set_prot() 只修改 controller 当前 uctx 中 static 页的权限；成功后
	 * 再把可信边界保存到 registry。后续 ATTACH 不接收这些边界，而是直接
	 * 读取 registry，并以 static 只读、runtime 可写方式建立映射。
	 */
	if (!uctx || !handle || !static_span_size || !runtime_span_size ||
	    static_span_size != runtime_span_offset ||
	    !IS_ALIGNED(static_span_size, SMALL_PAGE_SIZE) ||
	    !IS_ALIGNED(runtime_span_offset, SMALL_PAGE_SIZE) ||
	    !IS_ALIGNED(runtime_span_size, SMALL_PAGE_SIZE))
		return TEE_ERROR_BAD_PARAMETERS;

	mutex_lock(&span_shared_lock);
	if (span_shared_region.state != SPAN_SHARED_REGION_ACTIVE ||
	    span_shared_region.handle != handle ||
	    memcmp(&span_shared_region.owner_uuid, &span_confinfer_uuid,
		   sizeof(span_confinfer_uuid))) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_ITEM_NOT_FOUND;
	}
	index = span_shared_find_attachment_locked(uctx);
	if (index == SPAN_SHARED_MAX_ATTACHMENTS ||
	    !span_shared_region.attachments[index].is_controller ||
	    span_shared_region.attachments[index].state !=
		SPAN_SHARED_ATTACHMENT_ATTACHED) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_ACCESS_DENIED;
	}
	if (span_shared_region.layout_frozen) {
		res = span_shared_region.static_span_size == static_span_size &&
		      span_shared_region.runtime_span_offset == runtime_span_offset &&
		      span_shared_region.runtime_span_size == runtime_span_size ?
		      TEE_SUCCESS : TEE_ERROR_BAD_STATE;
		mutex_unlock(&span_shared_lock);
		return res;
	}
	if (span_shared_region.layout_freezing ||
	    span_shared_has_worker_locked()) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_BUSY;
	}
	if (runtime_span_offset > span_shared_region.size ||
	    runtime_span_size > span_shared_region.size - runtime_span_offset ||
	    runtime_span_offset + runtime_span_size != span_shared_region.size) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	shared_va = span_shared_region.shared_va;
	span_shared_region.layout_freezing = true;
	mutex_unlock(&span_shared_lock);

	res = vm_set_prot(uctx, shared_va, static_span_size,
			  TEE_MATTR_UR | TEE_MATTR_PR);

	mutex_lock(&span_shared_lock);
	if (span_shared_region.handle != handle ||
	    span_shared_region.state != SPAN_SHARED_REGION_ACTIVE ||
	    span_shared_find_attachment_locked(uctx) != index) {
		if (span_shared_region.handle == handle)
			span_shared_region.layout_freezing = false;
		mutex_unlock(&span_shared_lock);
		return res ? res : TEE_ERROR_BAD_STATE;
	}
	span_shared_region.layout_freezing = false;
	if (!res) {
		span_shared_region.static_span_size = static_span_size;
		span_shared_region.runtime_span_offset = runtime_span_offset;
		span_shared_region.runtime_span_size = runtime_span_size;
		span_shared_region.layout_frozen = true;
	}
	mutex_unlock(&span_shared_lock);
	return res;
}

static TEE_Result span_shared_detach(struct user_mode_ctx *uctx,
				     uint32_t handle)
{
	TEE_Result res = TEE_SUCCESS;
	vaddr_t mapped_va = 0;
	size_t size = 0;
	unsigned int index = SPAN_SHARED_MAX_ATTACHMENTS;

	/*
	 * 调用者：pta_span_shared_detach()，仅允许 worker 在正常完成后
	 * 调用。controller attachment 被显式拒绝，必须走 RELEASE_SHARED。
	 *
	 * 本函数先在锁内将 ATTACHED 改为 DETACHING，释放锁后 vm_unmap()
	 * 该 worker 的固定 VA；unmap 成功才清空 attachment。若 unmap
	 * 失败，状态恢复为 ATTACHED，使 controller RELEASE 仍能返回 BUSY，
	 * 而不会错误地撤销仍可访问映射的物理页保护。
	 */
	if (!uctx || !handle)
		return TEE_ERROR_BAD_PARAMETERS;

	mutex_lock(&span_shared_lock);
	if ((span_shared_region.state != SPAN_SHARED_REGION_ACTIVE &&
	     span_shared_region.state != SPAN_SHARED_REGION_RELEASING) ||
	    span_shared_region.handle != handle) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_ITEM_NOT_FOUND;
	}
	index = span_shared_find_attachment_locked(uctx);
	if (index == SPAN_SHARED_MAX_ATTACHMENTS ||
	    span_shared_region.attachments[index].is_controller) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_ACCESS_DENIED;
	}
	if (span_shared_region.attachments[index].state !=
	    SPAN_SHARED_ATTACHMENT_ATTACHED) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_BUSY;
	}

	mapped_va = span_shared_region.attachments[index].va;
	size = span_shared_region.size;
	span_shared_region.attachments[index].state =
		SPAN_SHARED_ATTACHMENT_DETACHING;
	mutex_unlock(&span_shared_lock);

	res = vm_unmap(uctx, mapped_va, size);
	mutex_lock(&span_shared_lock);
	if (res) {
		span_shared_region.attachments[index].state =
			SPAN_SHARED_ATTACHMENT_ATTACHED;
	} else {
		span_shared_clear_attachment_locked(index);
	}
	mutex_unlock(&span_shared_lock);

	/* controller 先退出时，最后一个显式 DETACH 同样负责归还物理页。 */
	if (!res)
		res = span_shared_finish_release();
	return res;
}

static TEE_Result span_shared_release(struct user_mode_ctx *uctx,
				      uint32_t handle)
{
	TEE_Result res = TEE_SUCCESS;
	vaddr_t mapped_va = 0;
	size_t size = 0;
	unsigned int index = SPAN_SHARED_MAX_ATTACHMENTS;
	bool need_unmap = false;

	/*
	 * 调用者：pta_span_shared_release()，仅 controller 可调用。
	 *
	 * ACTIVE 状态下若 span_shared_has_worker_locked() 为真，立即返回
	 * BUSY；这覆盖所有 worker attachment 的中间状态。无 worker 后，
	 * 函数将 region 改为 RELEASING，先撤销 controller 的固定 VA，
	 * 再调用 TF-A unprotect，最后清空注册表。
	 *
	 * 状态先变为 RELEASING 的原因是禁止并发 ATTACH 在 controller
	 * 已开始释放后重新加入。若 controller vm_unmap() 失败则恢复
	 * ACTIVE；TF-A unprotect 失败则保留 RELEASING 和登记信息，以免
	 * 在保护状态不确定时错误复用 handle 或物理区。
	 */
	if (!uctx || !handle)
		return TEE_ERROR_BAD_PARAMETERS;

	mutex_lock(&span_shared_lock);
	if (span_shared_region.handle != handle ||
	    (span_shared_region.state != SPAN_SHARED_REGION_ACTIVE &&
	     span_shared_region.state != SPAN_SHARED_REGION_RELEASING) ||
	    memcmp(&span_shared_region.owner_uuid, &span_confinfer_uuid,
		   sizeof(span_confinfer_uuid))) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_ITEM_NOT_FOUND;
	}
	index = span_shared_find_attachment_locked(uctx);
	if (index == SPAN_SHARED_MAX_ATTACHMENTS ||
	    !span_shared_region.attachments[index].is_controller) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_ACCESS_DENIED;
	}
	if (span_shared_has_worker_locked()) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_BUSY;
	}
	if (span_shared_region.layout_freezing) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_BUSY;
	}

	span_shared_region.state = SPAN_SHARED_REGION_RELEASING;
	need_unmap = !span_shared_region.controller_map_removed;
	mapped_va = span_shared_region.attachments[index].va;
	size = span_shared_region.size;
	mutex_unlock(&span_shared_lock);

	if (need_unmap) {
		res = vm_unmap(uctx, mapped_va, size);
		if (res) {
			mutex_lock(&span_shared_lock);
			span_shared_region.state = SPAN_SHARED_REGION_ACTIVE;
			mutex_unlock(&span_shared_lock);
			return res;
		}

		mutex_lock(&span_shared_lock);
		span_shared_region.controller_map_removed = true;
		mutex_unlock(&span_shared_lock);
	}

	return span_shared_finish_release();
}

void span_uctx_destroying(struct user_mode_ctx *uctx)
{
	unsigned int index = SPAN_SHARED_MAX_ATTACHMENTS;

	/*
	 * 调用者：core/kernel/user_ta.c 的 free_utc()，在 vm_info_final()
	 * 前调用，覆盖正常关闭与 TEE_Panic() 后的强制销毁。
	 *
	 * 函数不执行 vm_unmap() 或 TF-A unprotect：此时通用 VM 仍拥有该
	 * uctx 的页表。它只在锁内把 attachment 标为 DYING；若销毁者是
	 * controller，同时把 region 标为 RELEASING，阻止后续 worker
	 * ATTACH。实际删除记录和可能的 unprotect 交给下一阶段。
	 */
	if (!uctx)
		return;

	mutex_lock(&span_shared_lock);
	index = span_shared_find_attachment_locked(uctx);
	if (index != SPAN_SHARED_MAX_ATTACHMENTS) {
		span_shared_region.attachments[index].state =
			SPAN_SHARED_ATTACHMENT_DYING;
		if (span_shared_region.attachments[index].is_controller)
			span_shared_region.state = SPAN_SHARED_REGION_RELEASING;
	}
	mutex_unlock(&span_shared_lock);
}

void span_uctx_destroyed(struct user_mode_ctx *uctx)
{
	unsigned int index = SPAN_SHARED_MAX_ATTACHMENTS;

	/*
	 * free_utc() 在 vm_info_final() 后调用：此时才能删除 DYING 记录。
	 * 实际 unprotect 由统一收尾函数领取，重复 hook 不会重复归还物理页。
	 */
	if (!uctx)
		return;
	mutex_lock(&span_shared_lock);
	index = span_shared_find_attachment_locked(uctx);
	if (index != SPAN_SHARED_MAX_ATTACHMENTS &&
	    span_shared_region.attachments[index].state ==
		SPAN_SHARED_ATTACHMENT_DYING)
		span_shared_clear_attachment_locked(index);
	mutex_unlock(&span_shared_lock);
	(void)span_shared_finish_release();
}

struct span_secure_phys_mobj {
	struct mobj mobj;
	paddr_t pa;
	uint32_t mem_type;
};

static struct span_secure_phys_mobj *to_span_secure_phys_mobj(struct mobj *mobj)
{
	return container_of(mobj, struct span_secure_phys_mobj, mobj);
}

static TEE_Result span_secure_phys_get_pa(struct mobj *mobj, size_t offs,
					  size_t granule, paddr_t *pa)
{
	struct span_secure_phys_mobj *smobj = to_span_secure_phys_mobj(mobj);
	paddr_t p = 0;

	if (!pa)
		return TEE_ERROR_GENERIC;
	if (!mobj_check_offset_and_len(mobj, offs, 1))
		return TEE_ERROR_GENERIC;

	p = smobj->pa + offs;

	if (granule) {
		if (granule != SMALL_PAGE_SIZE &&
		    granule != CORE_MMU_PGDIR_SIZE)
			return TEE_ERROR_GENERIC;
		p &= ~(granule - 1);
	}

	*pa = p;
	return TEE_SUCCESS;
}

static TEE_Result span_secure_phys_get_mem_type(struct mobj *mobj,
						uint32_t *mem_type)
{
	struct span_secure_phys_mobj *smobj = to_span_secure_phys_mobj(mobj);

	if (!mem_type)
		return TEE_ERROR_GENERIC;

	*mem_type = smobj->mem_type;
	return TEE_SUCCESS;
}

static bool span_secure_phys_matches(struct mobj *mobj __unused,
				     enum buf_is_attr attr)
{
	return attr == CORE_MEM_SEC;
}

static void span_secure_phys_free(struct mobj *mobj)
{
	free(to_span_secure_phys_mobj(mobj));
}

static const struct mobj_ops span_secure_phys_mobj_ops = {
	.get_pa = span_secure_phys_get_pa,
	.get_mem_type = span_secure_phys_get_mem_type,
	.matches = span_secure_phys_matches,
	.free = span_secure_phys_free,
};

static struct mobj *span_secure_phys_alloc(paddr_t pa, size_t size,
					   uint32_t mem_type)
{
	struct span_secure_phys_mobj *smobj = NULL;

	if (!size || !IS_ALIGNED(pa, SMALL_PAGE_SIZE) ||
	    !IS_ALIGNED(size, SMALL_PAGE_SIZE))
		return NULL;

	smobj = calloc(1, sizeof(*smobj));
	if (!smobj)
		return NULL;

	smobj->mobj.ops = &span_secure_phys_mobj_ops;
	smobj->mobj.size = size;
	smobj->mobj.phys_granule = SMALL_PAGE_SIZE;
	refcount_set(&smobj->mobj.refc, 1);
	smobj->pa = pa;
	smobj->mem_type = mem_type;

	return &smobj->mobj;
}

static TEE_Result span_call_tfa_protect(paddr_t phys_addr, size_t size)
{
	struct thread_smc_args args = { };
	uint32_t hi = 0;
	uint32_t lo = 0;

	// ################################################################################
	// # for ConfInfer Modified by yyh : 2026 - Sep 20
	// ################################################################################
	/*
	 * 原注释把 protect 视为必定成功，保留原假设供审计：
	 * “错误码只向上传递，不设计硬件部分生效后的恢复协议”。
	 *
	 * 当前 shared 路径不再依赖该假设。TF-A 返回值在这里转换为
	 * TEE_Result；span_shared_create() 在 protect 失败时清除 CREATING
	 * 注册表，在 protect 成功但固定 VA 映射失败时进入统一 RELEASE
	 * 收尾。若回滚 unprotect 失败，注册表会保留物理范围和 RELEASING
	 * 状态供重试，绝不把仍受保护的页报告为 EMPTY。
	 */
	args.a0 = TRUSTSPAN_SMC_PROTECT_MEM;
	reg_pair_from_64(phys_addr, &hi, &lo);
	args.a1 = hi;
	args.a2 = lo;

	reg_pair_from_64(size, &hi, &lo);
	args.a3 = hi;
	args.a4 = lo;

	thread_smccc(&args);

	switch (args.a0) {
	case TRUSTSPAN_SMC_OK:
		return TEE_SUCCESS;
	case TRUSTSPAN_SMC_E_INVALID_PARAM:
		return TEE_ERROR_BAD_PARAMETERS;
	case TRUSTSPAN_SMC_E_DENIED:
		return TEE_ERROR_ACCESS_DENIED;
	case TRUSTSPAN_SMC_E_NOT_SUPPORTED:
		return TEE_ERROR_NOT_SUPPORTED;
	case TRUSTSPAN_SMC_E_TZC_FAILED:
	default:
		return TEE_ERROR_GENERIC;
	}
}

static TEE_Result span_call_tfa_unprotect(paddr_t phys_addr, size_t size)
{
	struct thread_smc_args args = { };
	uint32_t hi = 0;
	uint32_t lo = 0;

	/*
	 * 原注释假设 unprotect 必定成功，并提出在此覆盖整段内存。
	 * 当前 shared 路径由 span_shared_finish_release() 在锁内领取唯一
	 * 收尾权、在锁外调用本函数；失败时保持 RELEASING，允许安全重试。
	 *
	 * Span PTA 只知道物理范围，不知道 ModelImage 中哪些字节是需要返回
	 * REE 的最终输出，不能在这里无条件清零整个 region。敏感临时区的
	 * 擦除必须由后续 Image/runtime 协议依据 TEE 已验证的子区间，在最后
	 * attachment 释放和 unprotect 之前完成；阶段 5.0 的合成页不承载模型。
	 */
	args.a0 = TRUSTSPAN_SMC_UNPROTECT_MEM;
	reg_pair_from_64(phys_addr, &hi, &lo);
	args.a1 = hi;
	args.a2 = lo;

	reg_pair_from_64(size, &hi, &lo);
	args.a3 = hi;
	args.a4 = lo;

	thread_smccc(&args);

	switch (args.a0) {
	case TRUSTSPAN_SMC_OK:
		return TEE_SUCCESS;
	case TRUSTSPAN_SMC_E_INVALID_PARAM:
		return TEE_ERROR_BAD_PARAMETERS;
	case TRUSTSPAN_SMC_E_DENIED:
		return TEE_ERROR_ACCESS_DENIED;
	case TRUSTSPAN_SMC_E_NOT_SUPPORTED:
		return TEE_ERROR_NOT_SUPPORTED;
	case TRUSTSPAN_SMC_E_TZC_FAILED:
	default:
		return TEE_ERROR_GENERIC;
	}
}

static TEE_Result span_try_map_to_ta(struct user_mode_ctx *uctx, paddr_t pa,
				     size_t size, vaddr_t *va)
{
	struct mobj *mobj = NULL;
	uint32_t prot = TEE_MATTR_URW | TEE_MATTR_PRW;
	TEE_Result res = TEE_ERROR_GENERIC;
	vaddr_t map_va = 0;

	if (!size || !IS_ALIGNED(pa, SMALL_PAGE_SIZE) ||
	    !IS_ALIGNED(size, SMALL_PAGE_SIZE))
		return TEE_ERROR_BAD_PARAMETERS;

	mobj = span_secure_phys_alloc(pa, size, TEE_MATTR_MEM_TYPE_CACHED);
	if (!mobj)
		return TEE_ERROR_OUT_OF_MEMORY;

	// 模型镜像数据供后续执行命令使用
	// 因此必须属于 TA 上下文 而不是在PTA命令返回时被清理掉的临时参数映射
	// VM_FLAG_EPHEMERAL 应改为普通的用户 TA 映射，即 flags 传 0
	// vm_map(uctx, &map_va, size, prot, VM_FLAG_EPHEMERAL, mobj, 0);
	res = vm_map(uctx, &map_va, size, prot, 0, mobj, 0);
	mobj_put(mobj);
	if (res)
		return res;

	*va = map_va;
	return TEE_SUCCESS;
}

TEE_Result span_handle_protect(struct user_mode_ctx *uctx, paddr_t phys_addr,
			       size_t size, struct span_request *req)
{
	TEE_Result res = TEE_SUCCESS;

	if (!uctx || !req || !size)
		return TEE_ERROR_BAD_PARAMETERS;

	if (!IS_ALIGNED(phys_addr, SMALL_PAGE_SIZE) ||
	    !IS_ALIGNED(size, SMALL_PAGE_SIZE))
		return TEE_ERROR_BAD_PARAMETERS;

	req->phys_addr = phys_addr;
	req->size = size;
	req->ta_vaddr = 0;
	req->mapped = false;

	res = span_try_map_to_ta(uctx, phys_addr, size, &req->ta_vaddr);
	if (!res)
		req->mapped = true;

	return res;
}

TEE_Result span_handle_release(struct user_mode_ctx *uctx, vaddr_t ta_vaddr,
			       size_t size)
{
	paddr_t phys_addr = 0;
	TEE_Result res = TEE_SUCCESS;

	if (!uctx || !ta_vaddr || !size)
		return TEE_ERROR_BAD_PARAMETERS;

	/*
	 * 映射解除后无法再从 TA 页表查询物理地址
	 * 因此先记录 phys_addr
	 * 再按固定顺序解除 TA 映射并撤销 TZC 保护
	 * 当前实验假设 unprotect 必定成功 因而不保留额外状态用于失败重试
	 */
	res = vm_va2pa(uctx, (void *)ta_vaddr, &phys_addr);
	if (res)
		return res;

	res = vm_unmap(uctx, ta_vaddr, size);
	if (res)
		return res;

	return span_call_tfa_unprotect(phys_addr, size);
}

static TEE_Result pta_span_protect(uint32_t ptypes,
				   TEE_Param params[TEE_NUM_PARAMS])
{
	const uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
						TEE_PARAM_TYPE_VALUE_INPUT,
						TEE_PARAM_TYPE_VALUE_OUTPUT,
						TEE_PARAM_TYPE_VALUE_OUTPUT);
	struct ts_session *s = NULL;
	struct user_mode_ctx *uctx = NULL;
	struct span_request req = { };
	paddr_t phys_addr = 0;
	size_t size = 0;
	TEE_Result res = TEE_SUCCESS;

	if (ptypes != exp_pt)
		return TEE_ERROR_BAD_PARAMETERS;

	// s = ts_get_current_session()
	s = ts_get_calling_session();
	if (!s || !is_user_mode_ctx(s->ctx))
		return TEE_ERROR_ACCESS_DENIED;

	uctx = to_user_mode_ctx(s->ctx);
	phys_addr = ((uint64_t)params[0].value.a << 32) | params[0].value.b;
	size = ((uint64_t)params[1].value.a << 32) | params[1].value.b;

	if (!tee_pbuf_is_non_sec(phys_addr, size))
		return TEE_ERROR_BAD_PARAMETERS;

	IMSG("%s: received protect phys=0x%016" PRIx64 " size=0x%016" PRIx64,
	     PTA_NAME, (uint64_t)phys_addr, (uint64_t)size);

	/*
	 * 先由 TF-A 将 Region 交给 secure world 再建立 TA 映射。
	 * 当前实验假设 TZASC protect 必定成功。
	 * 下方回滚只覆盖 protect 成功后 TA 映射建立失败的情形。
	 */
	res = span_call_tfa_protect(phys_addr, size);
	if (res) {
		EMSG("%s: TF-A protect failed: 0x%x", PTA_NAME, res);
		return res;
	}
	res = span_handle_protect(uctx, phys_addr, size, &req);
	if (res) {
		TEE_Result rollback_res = TEE_SUCCESS;

		EMSG("%s: protect request failed: 0x%x", PTA_NAME, res);
		rollback_res = span_call_tfa_unprotect(phys_addr, size);
		if (rollback_res) {
			EMSG("%s: protect rollback failed: 0x%x", PTA_NAME,
			     rollback_res);
			return rollback_res;
		}
		params[2].value.a = PTA_SPAN_PROTECT_REGION_RELEASED;
		params[2].value.b = 0;
		params[3].value.a = 0;
		params[3].value.b = 0;
		return TEE_SUCCESS;
	}

	IMSG("%s: mapped into TA va=0x%016" PRIx64, PTA_NAME,
	     (uint64_t)req.ta_vaddr);
	user_mode_ctx_print_mappings(uctx);

	reg_pair_from_64(req.ta_vaddr, &params[2].value.a, &params[2].value.b);
	reg_pair_from_64(req.size, &params[3].value.a, &params[3].value.b);

	return TEE_SUCCESS;
}

static TEE_Result pta_span_release(uint32_t ptypes,
				   TEE_Param params[TEE_NUM_PARAMS])
{
	const uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
						TEE_PARAM_TYPE_VALUE_INPUT,
						TEE_PARAM_TYPE_NONE,
						TEE_PARAM_TYPE_NONE);
	struct ts_session *s = NULL;
	struct user_mode_ctx *uctx = NULL;
	vaddr_t ta_vaddr = 0;
	size_t size = 0;

	if (ptypes != exp_pt)
		return TEE_ERROR_BAD_PARAMETERS;

	s = ts_get_calling_session();
	if (!s || !is_user_mode_ctx(s->ctx))
		return TEE_ERROR_ACCESS_DENIED;

	uctx = to_user_mode_ctx(s->ctx);
	ta_vaddr = ((uint64_t)params[0].value.a << 32) | params[0].value.b;
	size = ((uint64_t)params[1].value.a << 32) | params[1].value.b;

	return span_handle_release(uctx, ta_vaddr, size);
}

// ################################################################################
// # for ConfInfer Modified by yyh : 2026 - Sep 17
// ################################################################################
/*
 * ConfInfer 共享 region PTA 命令入口。
 *
 * 五个生命周期入口均先调用 span_get_confinfer_uctx()，使当前 PTA session 对应的
 * User TA uctx 成为可信身份；它们绝不接受 REE 提供的 uctx、worker VA
 * 或 worker 物理地址。
 *
 * CREATE_PROTECT 的 value[0]/value[1] 是 controller 提交的物理地址和
 * 大小；成功后 value[2] 返回 handle，value[3] 返回固定 VA。
 * ATTACH/DETACH/FREEZE_LAYOUT/RELEASE_SHARED 都以 value[0] 的 handle
 * 绑定已有注册表。FREEZE_LAYOUT 还接收 TEE 已验证的两段页边界；ATTACH
 * 额外在 value[1]/value[2] 返回注册表中的 VA 和大小，供 CA probe
 * 验证，不允许调用者把它们作为输入覆盖注册表。
 */
static TEE_Result pta_span_shared_create(uint32_t ptypes,
					  TEE_Param params[TEE_NUM_PARAMS])
{
	const uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
						TEE_PARAM_TYPE_VALUE_INPUT,
						TEE_PARAM_TYPE_VALUE_OUTPUT,
						TEE_PARAM_TYPE_VALUE_OUTPUT);
	struct user_mode_ctx *uctx = NULL;
	paddr_t phys_addr = 0;
	size_t size = 0;
	vaddr_t shared_va = 0;
	uint32_t handle = 0;
	TEE_Result res = TEE_SUCCESS;

	/*
	 * 仅 CREATE 在 TF-A protect 前检查输入物理区仍属于 non-secure
	 * buffer。protect 成功后，该地址不再允许 REE 访问；ATTACH 不重复
	 * 接收物理地址，因此不会出现 worker 以另一物理区替换共享 region。
	 */
	if (ptypes != exp_pt)
		return TEE_ERROR_BAD_PARAMETERS;
	res = span_get_confinfer_uctx(&uctx);
	if (res)
		return res;

	phys_addr = ((uint64_t)params[0].value.a << 32) | params[0].value.b;
	size = ((uint64_t)params[1].value.a << 32) | params[1].value.b;
	if (!tee_pbuf_is_non_sec(phys_addr, size))
		return TEE_ERROR_BAD_PARAMETERS;

	res = span_shared_create(uctx, phys_addr, size, &handle, &shared_va);
	if (res)
		return res;

	params[2].value.a = handle;
	params[2].value.b = 0;
	reg_pair_from_64(shared_va, &params[3].value.a, &params[3].value.b);
	IMSG("%s: created shared region handle=%" PRIu32 " va=0x%016" PRIx64,
	     PTA_NAME, handle, (uint64_t)shared_va);
	return TEE_SUCCESS;
}

static TEE_Result pta_span_shared_attach(uint32_t ptypes,
					  TEE_Param params[TEE_NUM_PARAMS])
{
	const uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
						TEE_PARAM_TYPE_VALUE_OUTPUT,
						TEE_PARAM_TYPE_VALUE_OUTPUT,
						TEE_PARAM_TYPE_NONE);
	struct user_mode_ctx *uctx = NULL;
	vaddr_t shared_va = 0;
	size_t size = 0;
	TEE_Result res = TEE_SUCCESS;

	/*
	 * 调用 span_shared_attach()。params[0] 只有 handle；worker 的
	 * uctx 由当前 session 取得。成功后的 VA/size 是 PTA 从注册表
	 * 返回的观察值，不参与映射决策。
	 */
	if (ptypes != exp_pt)
		return TEE_ERROR_BAD_PARAMETERS;
	res = span_get_confinfer_uctx(&uctx);
	if (res)
		return res;

	res = span_shared_attach(uctx, params[0].value.a, &shared_va, &size);
	if (res)
		return res;

	reg_pair_from_64(shared_va, &params[1].value.a, &params[1].value.b);
	reg_pair_from_64(size, &params[2].value.a, &params[2].value.b);
	IMSG("%s: attached shared region handle=%" PRIu32
	     " va=0x%016" PRIx64, PTA_NAME, params[0].value.a,
	     (uint64_t)shared_va);
	return TEE_SUCCESS;
}

static TEE_Result pta_span_shared_detach(uint32_t ptypes,
					  TEE_Param params[TEE_NUM_PARAMS])
{
	const uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
						TEE_PARAM_TYPE_NONE,
						TEE_PARAM_TYPE_NONE,
						TEE_PARAM_TYPE_NONE);
	struct user_mode_ctx *uctx = NULL;
	TEE_Result res = TEE_SUCCESS;

	/*
	 * 调用 span_shared_detach()。该命令只接受 handle；由当前 session
	 * 确认调用者是具体 worker，故其他 worker 不能解除其映射。
	 */
	if (ptypes != exp_pt)
		return TEE_ERROR_BAD_PARAMETERS;
	res = span_get_confinfer_uctx(&uctx);
	if (res)
		return res;

	return span_shared_detach(uctx, params[0].value.a);
}

static TEE_Result pta_span_shared_freeze_layout(
	uint32_t ptypes, TEE_Param params[TEE_NUM_PARAMS])
{
	const uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
						TEE_PARAM_TYPE_VALUE_INPUT,
						TEE_PARAM_TYPE_VALUE_INPUT,
						TEE_PARAM_TYPE_VALUE_INPUT);
	struct user_mode_ctx *uctx = NULL;
	uint64_t static_span_size = 0;
	uint64_t runtime_span_offset = 0;
	uint64_t runtime_span_size = 0;
	TEE_Result res = TEE_SUCCESS;

	/*
	 * 调用者：ConfInfer controller 的 ModelImage prepare 路径。
	 * params[0].a 是 CREATE_PROTECT 返回的 handle；params[1]、params[2]
	 * 和 params[3] 分别是 TEE 已验证的 static 大小、runtime 起点和
	 * runtime 大小，后三项均用 value.a/value.b 传递 64 位值。
	 *
	 * 本入口只负责 ABI 解码、size_t 截断检查和调用者 uctx 鉴权；是否
	 * 完整覆盖 region、是否有 worker、是否已经冻结以及 vm_set_prot()
	 * 的状态转换全部由 span_shared_freeze_layout() 在可信注册表中完成。
	 */
	if (ptypes != exp_pt || !params[0].value.a || params[0].value.b)
		return TEE_ERROR_BAD_PARAMETERS;
	res = span_get_confinfer_uctx(&uctx);
	if (res)
		return res;

	static_span_size = ((uint64_t)params[1].value.a << 32) |
			   params[1].value.b;
	runtime_span_offset = ((uint64_t)params[2].value.a << 32) |
			      params[2].value.b;
	runtime_span_size = ((uint64_t)params[3].value.a << 32) |
			    params[3].value.b;
	if ((uint64_t)(size_t)static_span_size != static_span_size ||
	    (uint64_t)(size_t)runtime_span_offset != runtime_span_offset ||
	    (uint64_t)(size_t)runtime_span_size != runtime_span_size)
		return TEE_ERROR_BAD_PARAMETERS;

	res = span_shared_freeze_layout(uctx, params[0].value.a,
					(size_t)static_span_size,
					(size_t)runtime_span_offset,
					(size_t)runtime_span_size);
	if (!res)
		IMSG("%s: froze shared layout handle=%" PRIu32
		     " static=0x%" PRIx64 " runtime_off=0x%" PRIx64
		     " runtime=0x%" PRIx64,
		     PTA_NAME, params[0].value.a, static_span_size,
		     runtime_span_offset, runtime_span_size);
	return res;
}

static TEE_Result pta_span_shared_release(uint32_t ptypes,
					   TEE_Param params[TEE_NUM_PARAMS])
{
	const uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
						TEE_PARAM_TYPE_NONE,
						TEE_PARAM_TYPE_NONE,
						TEE_PARAM_TYPE_NONE);
	struct user_mode_ctx *uctx = NULL;
	TEE_Result res = TEE_SUCCESS;

	/*
	 * 调用 span_shared_release()。该命令只接受 handle；函数内部以
	 * attachment.is_controller 再次鉴权，并检查所有 worker 已离开。
	 */
	if (ptypes != exp_pt)
		return TEE_ERROR_BAD_PARAMETERS;
	res = span_get_confinfer_uctx(&uctx);
	if (res)
		return res;

	return span_shared_release(uctx, params[0].value.a);
}


/*
 * 调用者：ConfInfer 平台 probe 的 STATUS 命令。
 * 仅输出控制元数据，不暴露内核指针或敏感页内容。slot+1 为 0 表示调用
 * uctx 尚未登记，controller 与 worker 的不同 slot 证明 registry 身份不同。
 */
static TEE_Result pta_span_shared_status(uint32_t ptypes,
					TEE_Param params[TEE_NUM_PARAMS])
{
	struct user_mode_ctx *uctx = NULL;
	unsigned int n = 0;
	unsigned int count = 0;
	unsigned int index = 0;
	TEE_Result res = span_get_confinfer_uctx(&uctx);

	if (res)
		return res;
	if (ptypes != TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
				     TEE_PARAM_TYPE_VALUE_OUTPUT,
				     TEE_PARAM_TYPE_VALUE_OUTPUT,
				     TEE_PARAM_TYPE_VALUE_OUTPUT))
		return TEE_ERROR_BAD_PARAMETERS;
	mutex_lock(&span_shared_lock);
	for (n = 0; n < SPAN_SHARED_MAX_ATTACHMENTS; n++)
		if (span_shared_region.attachments[n].state !=
		    SPAN_SHARED_ATTACHMENT_EMPTY)
			count++;
	index = span_shared_find_attachment_locked(uctx);
	params[0].value.a = span_shared_region.state;
	params[0].value.b = count;
	params[1].value.a = span_shared_protect_calls;
	params[1].value.b = span_shared_unprotect_calls;
	params[2].value.a = index == SPAN_SHARED_MAX_ATTACHMENTS ? 0 : index + 1;
	params[2].value.b = SPAN_SHARED_MAX_ATTACHMENTS;
	reg_pair_from_64(CFG_CONFINFER_SHARED_SPAN_VA,
			 &params[3].value.a, &params[3].value.b);
	mutex_unlock(&span_shared_lock);
	return TEE_SUCCESS;
}

/*
 * 调用者：已 attached 的 ConfInfer 平台 probe。只使用 registry 的物理页。
 * 验证普通映射不能占用保留窗口末页，以及带内部标记的映射不能覆盖已有
 * attachment。意外成功时先撤销测试映射再报错；全程不持 registry lock。
 */
static TEE_Result pta_span_shared_check_window(uint32_t ptypes)
{
	struct user_mode_ctx *uctx = NULL;
	struct mobj *mobj = NULL;
	paddr_t pa = 0;
	vaddr_t base = 0;
	vaddr_t va = 0;
	unsigned int index = 0;
	TEE_Result res = span_get_confinfer_uctx(&uctx);
	unsigned int test = 0;

	if (res)
		return res;
	if (ptypes != TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE, TEE_PARAM_TYPE_NONE,
				     TEE_PARAM_TYPE_NONE, TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	mutex_lock(&span_shared_lock);
	index = span_shared_find_attachment_locked(uctx);
	if (index == SPAN_SHARED_MAX_ATTACHMENTS ||
	    span_shared_region.attachments[index].state !=
		SPAN_SHARED_ATTACHMENT_ATTACHED ||
	    span_shared_region.size >= CFG_CONFINFER_SHARED_SPAN_SIZE) {
		mutex_unlock(&span_shared_lock);
		return TEE_ERROR_BAD_STATE;
	}
	pa = span_shared_region.phys_addr;
	base = span_shared_region.shared_va;
	mutex_unlock(&span_shared_lock);
	mobj = span_secure_phys_alloc(pa, SMALL_PAGE_SIZE, TEE_MATTR_MEM_TYPE_CACHED);
	if (!mobj)
		return TEE_ERROR_OUT_OF_MEMORY;
	for (test = 0; test < 2; test++) {
		va = test ? base :
			base + CFG_CONFINFER_SHARED_SPAN_SIZE - SMALL_PAGE_SIZE;
		res = vm_map(uctx, &va, SMALL_PAGE_SIZE, TEE_MATTR_URW | TEE_MATTR_PRW,
			     VM_FLAG_SHAREABLE |
			     (test ? VM_FLAG_CONFINFER_SHARED_SPAN : 0), mobj, 0);
		if (!res) {
			(void)vm_unmap(uctx, va, SMALL_PAGE_SIZE);
			res = TEE_ERROR_SECURITY;
			break;
		}
		if (res != TEE_ERROR_ACCESS_CONFLICT)
			break;
		res = TEE_SUCCESS;
	}
	mobj_put(mobj);
	return res;
}

static TEE_Result invoke_command(void *sess_ctx __unused, uint32_t cmd_id,
				 uint32_t ptypes,
				 TEE_Param params[TEE_NUM_PARAMS])
{
	/*
	 * ConfInfer User TA 经 PTA UUID 进入这里。旧 PROTECT/RELEASE 保持
	 * 原单 TA 语义；五个共享生命周期命令进入上方共享注册表状态机，
	 * 二者不混用。STATUS/CHECK_WINDOW 只用于读取或验证平台状态。
	 */
	switch (cmd_id) {
	case PTA_SPAN_CMD_PROTECT:
		return pta_span_protect(ptypes, params);
	case PTA_SPAN_CMD_RELEASE:
		return pta_span_release(ptypes, params);
	case PTA_SPAN_CMD_CREATE_PROTECT:
		return pta_span_shared_create(ptypes, params);
	case PTA_SPAN_CMD_ATTACH:
		return pta_span_shared_attach(ptypes, params);
	case PTA_SPAN_CMD_DETACH:
		return pta_span_shared_detach(ptypes, params);
	case PTA_SPAN_CMD_FREEZE_LAYOUT:
		return pta_span_shared_freeze_layout(ptypes, params);
	case PTA_SPAN_CMD_RELEASE_SHARED:
		return pta_span_shared_release(ptypes, params);
	case PTA_SPAN_CMD_SHARED_STATUS:
		return pta_span_shared_status(ptypes, params);
	case PTA_SPAN_CMD_CHECK_WINDOW:
		return pta_span_shared_check_window(ptypes);
	default:
		return TEE_ERROR_NOT_IMPLEMENTED;
	}
}

pseudo_ta_register(.uuid = PTA_SPAN_UUID, .name = PTA_NAME,
		   .flags = PTA_DEFAULT_FLAGS,
		   .invoke_command_entry_point = invoke_command);
