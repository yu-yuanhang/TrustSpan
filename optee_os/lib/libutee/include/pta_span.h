// pta_span : for ConfInfer Modified by yyh : 2025 jul 16

#ifndef __PTA_SPAN_H__
#define __PTA_SPAN_H__

#define PTA_SPAN_UUID \
	{ 0x20260716, 0x9abc, 0x4def, \
	  { 0x80, 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff } }

/*
 * PROTECT 将物理连续区映射到调用 TA 的长期地址空间
 * 映射跨越后续 execute 命令保留
 * RELEASE 先解除 TA 虚拟映射 再撤销 TF-A 对物理区的保护
 */
#define PTA_SPAN_CMD_PROTECT	0
#define PTA_SPAN_CMD_RELEASE	1

// ################################################################################
// # for ConfInfer Modified by yyh : 2026 - Sep 17
// ################################################################################
/*
 * ConfInfer 并行平台 probe 的共享 region 生命周期命令。
 *
 * 命令由 ConfInfer User TA 通过 TEE_InvokeTACommand() 调用 Span PTA；
 * Span PTA 的 invoke_command() 再分派到同名 pta_span_shared_*()。
 *
 * CREATE_PROTECT：仅 controller 传入 REE 分配的物理地址和大小。PTA
 * 保护该物理区，映射 controller，并返回不透明 region handle 和固定 VA。
 * ATTACH：worker 仅传入 handle；PTA 从可信注册表读取物理地址、大小和
 * 固定 VA 后映射 worker，禁止 worker 选择这些敏感映射参数。
 * DETACH：worker 解除自己的固定 VA 映射并删除 attachment。
 * FREEZE_LAYOUT：controller 在完成 Image 校验和原地 packing 后提交已经由
 * TEE 验证的 static/runtime 页边界。PTA 将 controller 的 static 映射降为
 * 只读，并使后续 worker 以 static 只读、runtime 可写的权限建立映射。
 * RELEASE_SHARED：仅 controller 可调用；只在没有 worker attachment 时
 * 解除 controller 映射、调用 TF-A unprotect 并清空注册表。
 */
#define PTA_SPAN_CMD_CREATE_PROTECT	2
#define PTA_SPAN_CMD_ATTACH		3
#define PTA_SPAN_CMD_DETACH		4
#define PTA_SPAN_CMD_RELEASE_SHARED	5
/* 平台 probe：可信注册表快照与固定窗口/重叠映射拒绝检查。 */
#define PTA_SPAN_CMD_SHARED_STATUS	6
#define PTA_SPAN_CMD_CHECK_WINDOW	7
#define PTA_SPAN_CMD_FREEZE_LAYOUT	8

/* PROTECT 成功返回时 value[2] 的特殊值 表示请求失败但 Region 已归还。 */
#define PTA_SPAN_PROTECT_REGION_RELEASED	0x5350414eU

#endif /* __PTA_SPAN_H__ */
