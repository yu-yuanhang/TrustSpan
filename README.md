# TrustSpan 结论索引

## 先看答案

当前 `Base FVP` 需要记住的结论只有 4 条：

1. 模型默认主 RAM 总量是 `4 GB`，来源是模型参数 `bp.dram_size=4`
2. 这 `4 GB` 不是连续放在一个地址段里，而是映射成两段系统物理地址窗口
3. 当前平台上用的不是泛泛的 `TZASC`，而是 `TZC-400`
4. 当前 `TZC-400` 的默认配置者是 `TF-A`

## 文档分工

- [FVP_安全内存与默认硬件信息梳理.md](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/CI/TrustSpan/FVP_安全内存与默认硬件信息梳理.md:1)
  回答：`Base FVP` 的默认内存总量、系统物理地址窗口、安全相关保留区分别是什么

- [FVP_TZASC_TZC专项梳理.md](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/CI/TrustSpan/FVP_TZASC_TZC专项梳理.md:1)
  回答：当前平台上的 `TZC-400` 是谁配置、默认 region 怎么配

- [TZC-400.md](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/CI/TrustSpan/TZC-400.md:1)
  回答：`TZC-400` 寄存器、位定义、当前默认值怎么看

## 当前项目最重要的判断

`TrustSpan` 后续如果要做 demo，核心不是“再理解一遍 FVP”，而是验证这件事：

- 修改 `TF-A` 里 `TZC-400` 的 region 配置后，`REE/Linux` 对目标地址段的访问结果会发生可观测变化

这件事一旦做通，项目就进入可验证状态。

# 闭环测试设计

## 要回答的问题

验证一件事：

- `REE/Linux` 运行时先分配一块连续物理内存
- 把物理地址传给 `TEE`
- `TEE` 触发安全配置修改
- `REE/Linux` 再访问同一块内存时失败

## 方案

链路：

- `Linux driver -> OP-TEE PTA -> SMC -> TF-A -> TZC-400`

分工：

- `REE/Linux`：分配连续物理内存，切换前后各访问一次
- `OP-TEE`：校验参数，转发请求
- `TF-A`：运行时重配 `TZC-400`

## 要改哪里

### 1. REE/Linux

新增一个最小测试驱动：

- 用 `dma_alloc_coherent()` 或 `CMA` 分配连续物理内存
- 打印 `phys addr` 和 `size`
- 切换前先读写一次
- 调 `OP-TEE` 发送 `phys addr + size`
- 切换后再读写一次

### 2. OP-TEE

新增一个最小 `PTA`：

- 输入：`phys addr`、`size`
- 校验：`4KB` 对齐、地址落在 non-secure DRAM、不能和已有 secure 区重叠
- 通过 `SMC` 转发给 `TF-A`

### 3. TF-A

新增一个运行时 `SMC` 入口：

- 收到 `phys addr + size`
- 重新拆分 DRAM1 的 `TZC region`
- 把目标区间改成 `secure only`
- 重新 enable `TZC filters`

关键代码位置：

- [trusted-firmware-a/plat/arm/common/arm_tzc400.c](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/common/arm_tzc400.c:22)
- [trusted-firmware-a/include/plat/arm/common/plat_arm.h](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/include/plat/arm/common/plat_arm.h:65)

## 验证方法

1. `REE/Linux` 分配一块连续物理内存
2. 切换前访问成功
3. 把地址传给 `TEE`
4. `TF-A` 把该区间改成 secure
5. `REE/Linux` 再访问同一块地址
6. 预期访问失败

## 成功标准

- 同一块由 `REE/Linux` 运行时分配出来的连续物理内存，在安全配置切换前可访问，切换后不可访问。
