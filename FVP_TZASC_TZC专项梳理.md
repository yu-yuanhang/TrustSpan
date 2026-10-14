# Base FVP 的 TZC 配置答案版

## 1. 先给结论

当前平台上的关键事实只有 5 条：

1. 当前平台使用的是 `TZC-400`
2. `Base FVP` 才有这套配置路径，`Foundation FVP` 不是当前目标
3. 当前默认由 `TF-A` 在 `EL3` 配置 `TZC-400`
4. 当前默认只使用 `filter0`
5. 当前默认把 `0xff000000 - 0xffffffff` 设成 `secure only`

## 2. 谁在配置 TZC

主配置者：

- `TF-A`

代码入口：

- [trusted-firmware-a/plat/arm/board/fvp/fvp_security.c](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/board/fvp/fvp_security.c:1)
- [trusted-firmware-a/plat/arm/common/arm_tzc400.c:22](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/common/arm_tzc400.c:22)

`OP-TEE` 当前做的事是：

- 映射 `TZC-400` 寄存器
- 调 `tzc_init()`
- 调 `tzc_dump_state()`

来源：

- [optee_os/core/arch/arm/plat-vexpress/main.c:33](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/optee_os/core/arch/arm/plat-vexpress/main.c:33)

所以当前结论很明确：

- 真正改默认权限，先改 `TF-A`

## 3. 当前平台上的 TZC 硬件信息

### 3.1 基址

- `TZC-400` 基址：`0x2a4a0000`

来源：

- [trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:287](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:287)
- [optee_os/core/arch/arm/plat-vexpress/platform_config.h:132](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/optee_os/core/arch/arm/plat-vexpress/platform_config.h:132)

### 3.2 当前连接的 filter

平台注释已经写明：

- `filter0`：CPU clusters
- `filter2`：LCD
- `filter1`、`filter3`：未连接

但当前软件只配置：

- `PLAT_ARM_TZC_FILTERS = TZC_400_REGION_ATTR_FILTER_BIT(0)`

也就是：

- 当前默认只让 `filter0` 参与 region 判定

来源：

- [trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:272](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:272)
- [trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:288](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:288)

### 3.3 模型复位时暴露的 TZC 能力

模型参数里可以直接看到：

- `bp.tzc_400.rst_build_config=0x3003f08`

这表示模型本身带有 `TZC-400` 实现，并暴露 reset 配置寄存器值。

来源：

- `FVP_Base_RevC-2xAEMvA --list-params`

## 4. 当前默认 region 配置

来源：

- [trusted-firmware-a/plat/arm/common/arm_tzc400.c:28](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/common/arm_tzc400.c:28)
- [trusted-firmware-a/include/plat/arm/common/arm_def.h:145](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/include/plat/arm/common/arm_def.h:145)

当前默认 region 结论如下。

### region0

- 范围：所有未被其他 region 覆盖的地址
- secure 属性：`TZC_REGION_S_NONE`
- NSAID 权限：`0`
- 含义：默认拒绝

### region1

- 范围：`0xff000000 - 0xffffffff`
- secure 属性：`TZC_REGION_S_RDWR`
- NSAID 权限：`0`
- 含义：`secure only`

### region2

- 范围：`0x80000000 - 0xfeffffff`
- secure 属性：`TZC_REGION_S_NONE`
- NSAID 权限：`PLAT_ARM_TZC_NS_DEV_ACCESS`
- 含义：第一段非安全 DRAM

### region3

- 范围：`0x880000000 - 0x8ffffffff`
- secure 属性：`TZC_REGION_S_NONE`
- NSAID 权限：`PLAT_ARM_TZC_NS_DEV_ACCESS`
- 含义：第二段非安全 DRAM

## 5. 当前放行的 NSAID

当前白名单来自：

- [trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:290](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:290)
- [trusted-firmware-a/plat/arm/board/fvp/fvp_def.h](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/board/fvp/fvp_def.h:1)

当前允许的 NSAID：

- `0`：`FVP_NSAID_DEFAULT`
- `1`：`FVP_NSAID_PCI`
- `8`：`FVP_NSAID_VIRTIO`
- `9`：`FVP_NSAID_AP`
- `15`：`FVP_NSAID_VIRTIO_OLD`

汇总值：

- `PLAT_ARM_TZC_NS_DEV_ACCESS = 0x83038303`

## 6. 当前配置流程

`TF-A` 的实际流程就是：

1. `tzc400_init()`
2. `tzc400_disable_filters()`
3. `tzc400_configure_region0(TZC_REGION_S_NONE, 0)`
4. 依次配置 `region1`、`region2`、`region3`
5. `tzc400_set_action(TZC_ACTION_ERR)`
6. `tzc400_enable_filters()`

来源：

- [trusted-firmware-a/plat/arm/common/arm_tzc400.c:36](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/common/arm_tzc400.c:36)

## 7. 对项目最有用的最终答案

如果后续要做 `TrustSpan demo`，真正要改的是：

- `TF-A` 里 `region1/region2/region3` 的范围和权限

当前默认保护语义一句话概括就是：

- 第一段 DRAM 顶部 `16 MB` 被 `TZC-400` 锁成 `secure only`，两段主 DRAM 其余部分只对白名单 NSAID 开放非安全访问，其余地址默认拒绝。
