# Base FVP 内存答案版

## 1. 先给结论

当前 `Base FVP` 下，必须把 3 个概念分开：

1. 模型默认主 RAM 总量：`4 GB`
2. 系统物理地址窗口：两段
3. 安全相关保留区：`Trusted DRAM`、`OP-TEE reserved-memory`、`TZC secure window`

如果把这 3 件事混在一起，就会得出错误结论。

## 2. 模型默认主 RAM 到底多大

答案：

- 默认是 `4 GB`

直接来源是模型参数：

- `FVP_Base_RevC-2xAEMvA --list-params`
- 关键项：`bp.dram_size=4`
- 参数说明：`Size of main memory in gigabytes`

所以“当前默认主 RAM 有多大”这个问题，当前本地环境能落地的答案就是：

- `4 GB`

## 3. 这 4 GB 在系统物理地址空间里怎么映射

答案：

- 第一段：`0x80000000 - 0xffffffff`，`2 GB`
- 第二段：`0x880000000 - 0x8ffffffff`，`2 GB`

来源：

- [linux/arch/arm64/boot/dts/arm/fvp-base-revc.dts:168](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/linux/arch/arm64/boot/dts/arm/fvp-base-revc.dts:168)
- [trusted-firmware-a/include/plat/arm/common/arm_def.h:205](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/include/plat/arm/common/arm_def.h:205)
- [trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:66](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:66)

设备树原文：

```dts
reg = <0x00000000 0x80000000 0 0x80000000>,
      <0x00000008 0x80000000 0 0x80000000>;
```

这里表示的是：

- 两段 `system physical address window`
- 不是“1 段连续 DDR 从低地址排到高地址”

所以当前正确说法是：

- 当前模型有 `4 GB` 主 RAM
- 这 `4 GB` 被映射到两段系统物理地址窗口

## 4. 当前能确认的安全相关内存区

### 4.1 Trusted DRAM

- 区间：`0x06000000 - 0x07ffffff`
- 大小：`32 MB`

来源：

- [trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:43](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:43)
- [optee_os/core/arch/arm/plat-vexpress/conf.mk:71](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/optee_os/core/arch/arm/plat-vexpress/conf.mk:71)

### 4.2 Linux reserved-memory 里的 OP-TEE 区

- 区间：`0x83000000 - 0x83ffffff`
- 大小：`16 MB`

来源：

- [linux/arch/arm64/boot/dts/arm/fvp-base-revc.dts:187](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/linux/arch/arm64/boot/dts/arm/fvp-base-revc.dts:187)

注意：

- `OP-TEE` 自己实际配置的共享内存只用了其中前 `2 MB`
- 即 `0x83000000 - 0x831fffff`

来源：

- [optee_os/core/arch/arm/plat-vexpress/conf.mk:74](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/optee_os/core/arch/arm/plat-vexpress/conf.mk:74)

### 4.3 TZC 保护窗口

- 区间：`0xff000000 - 0xffffffff`
- 大小：`16 MB`

来源：

- [trusted-firmware-a/include/plat/arm/common/arm_def.h:145](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/include/plat/arm/common/arm_def.h:145)
- [optee_os/core/arch/arm/plat-vexpress/platform_config.h:94](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/optee_os/core/arch/arm/plat-vexpress/platform_config.h:94)

这个区不是“额外新增的 RAM”，而是：

- 从第一段 DRAM 顶部切出来的 `TZC secured window`

## 5. 当前默认硬件地址

当前和后续开发最相关的默认硬件基址：

- `TZC-400`：`0x2a4a0000`
- `SMMU v3`：`0x2b400000`
- `GICD`：`0x2f000000`
- `GICR`：`0x2f100000`
- `PCIe ECAM`：`0x40000000`

来源主要是：

- [linux/arch/arm64/boot/dts/arm/fvp-base-revc.dts](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/linux/arch/arm64/boot/dts/arm/fvp-base-revc.dts:1)
- [trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:287](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:287)

## 6. 当前这份文档真正回答的问题

如果只保留一句话，就是：

- `Base FVP` 默认主 RAM 是 `4 GB`，但它在系统物理地址空间里不是连续一段，而是映射成 `0x80000000-0xffffffff` 和 `0x880000000-0x8ffffffff` 两个窗口；安全相关区域再从这些窗口和独立 secure 区里切分出来。

## 7. 当前不能在本仓库里直接下结论的事

下面这个问题，当前仓库源码和本地启动参数不能直接证明：

- “底层 DDR 颗粒/控制器内部是否就是两片连续内存、还是模型内部再做了一层地址重映射”

当前本地能严格落地的只有：

- 模型总容量：`bp.dram_size=4`
- OS/TF-A 看到的系统物理地址窗口：两段

这两件事不能再混写成“物理内存连续布局”。
