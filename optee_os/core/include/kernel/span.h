// SPDX-License-Identifier: BSD-2-Clause
// ################################################################################
// # for ConfInfer Modified by yyh : 2026 - Sep 17
// ################################################################################
#ifndef KERNEL_SPAN_H
#define KERNEL_SPAN_H

struct user_mode_ctx;

/*
 * User TA 销毁路径使用的 Span 生命周期钩子。
 *
 * 调用者是 core/kernel/user_ta.c 的 free_utc()：
 * - span_uctx_destroying() 在 vm_info_final() 前运行，将该 uctx 在
 *   Span PTA 注册表中的 attachment 标为 DYING。此时页表仍存在，不能
 *   提前 unprotect；
 * - span_uctx_destroyed() 在 vm_info_final() 后运行。此时通用 VM 已
 *   撤销该 TA 的页表映射，函数删除 attachment；若该 region 已进入
 *   RELEASING 且没有其他 attachment，才调用 TF-A unprotect。
 *
 * 这两个函数由 Span PTA 实现，用于覆盖 worker panic、异常关闭 session
 * 和正常 TA 销毁，避免注册表保留悬空 uctx 或提前归还仍被 worker 使用的页。
 */
void span_uctx_destroying(struct user_mode_ctx *uctx);
void span_uctx_destroyed(struct user_mode_ctx *uctx);

#endif /* KERNEL_SPAN_H */
