# Base FVP 完整物理地址空间与安全/非安全划分

## 1. 这份文档只回答一个问题

你要追的不是“Linux 看到了多少内存”，而是：

- `Base FVP` 的系统物理地址空间里
- 哪些区间是 RAM
- 哪些区间是安全内存
- 哪些区间是非安全内存
- 哪些区间是外设/MMIO

这份文档只按这个视角写。

## 2. 最终结论

当前默认配置下，`Base FVP` 的关键物理地址划分如下：

### 2.1 安全世界独占物理内存

- `0x00000000 - 0x03ffffff`
  含义：`Trusted ROM`
  属性：安全

- `0x06000000 - 0x07ffffff`
  含义：`Trusted DRAM`
  属性：安全

- `0xff000000 - 0xffffffff`
  含义：`TZC protected DRAM window`
  属性：安全

### 2.2 非安全主 RAM

- `0x80000000 - 0xfeffffff`
  含义：第一段非安全 DRAM
  属性：非安全

- `0x880000000 - 0x8ffffffff`
  含义：第二段非安全 DRAM
  属性：非安全

### 2.3 特殊保留区

- `0x83000000 - 0x83ffffff`
  含义：Linux `reserved-memory` 里的 `OP-TEE` 预留区
  属性：非安全物理地址中的保留区，Linux 不直接拿来当普通页分配

- `0x83000000 - 0x831fffff`
  含义：`OP-TEE shared memory`
  属性：共享区，不是 secure-only

### 2.4 典型外设/MMIO

- `0x18000000 - 0x187fffff`
  含义：`VRAM`

- `0x2a4a0000`
  含义：`TZC-400`

- `0x2b400000`
  含义：`SMMU v3`

- `0x2f000000`
  含义：`GICD`

- `0x2f100000`
  含义：`GICR`

- `0x40000000`
  含义：`PCIe ECAM`

## 3. 这几个区间到底是不是物理地址

上面所有地址，都是：

- `system physical address`

都不是虚拟地址。

区别只在于：

- 有的是安全世界用的物理地址
- 有的是非安全世界用的物理地址
- 有的是 RAM
- 有的是 MMIO

## 4. 当前主 RAM 到底多大

这个问题单独回答：

- 模型默认主 RAM 总量是 `4 GB`

来源：

- `FVP_Base_RevC-2xAEMvA --list-params`
- `bp.dram_size=4`

这 `4 GB` 没有连续铺在一个地址段里，而是被映射成：

- `0x80000000 - 0xffffffff`，`2 GB`
- `0x880000000 - 0x8ffffffff`，`2 GB`

所以：

- “物理内存总量”是 `4 GB`
- “物理地址窗口”是两段

这两句话必须同时成立。

## 5. 完整物理地址空间视角下的划分表

| 物理地址区间 | 大小 | 类型 | 安全属性 | 当前用途 |
| --- | --- | --- | --- | --- |
| `0x00000000 - 0x03ffffff` | 64 MB | ROM | 安全 | Trusted ROM |
| `0x04000000 - 0x05ffffff` | 32 MB | 未在当前文档作为主结论使用 | 不在本次重点 | 地址空间中的其他区间 |
| `0x06000000 - 0x07ffffff` | 32 MB | RAM | 安全 | Trusted DRAM |
| `0x0c000000 - 0x0fffffff` | 64 MB | Flash | 设备区 | FLASH1 |
| `0x14000000 - 0x17ffffff` | 64 MB | SRAM/PSRAM | 设备区 | PSRAM |
| `0x18000000 - 0x187fffff` | 8 MB | RAM-like/MMIO reserved | 保留区 | VRAM |
| `0x20000000 - 0x2c1fffff` | 约 194 MB | MMIO | 设备区 | DEVICE0 |
| `0x2e000000 - 0x2e00ffff` | 64 KB | SRAM | 非安全 | NSRAM |
| `0x2f000000 - ...` | 若干 | MMIO | 设备区 | GIC/TZC/SMMU 等 |
| `0x7fe00000 - 0x7fffffff` | 2 MB | MMIO | 设备区 | DEVICE2 |
| `0x80000000 - 0xfeffffff` | `0x7f000000` | RAM | 非安全 | DRAM1 非安全部分 |
| `0xff000000 - 0xffffffff` | 16 MB | RAM | 安全 | DRAM1 顶部 TZC secure window |
| `0x880000000 - 0x8ffffffff` | 2 GB | RAM | 非安全 | DRAM2 |

说明：

- 这张表的重点是“安全/非安全/外设怎么分”
- 不是想把所有地址洞都解释成某种 RAM
- 没列成结论的空洞区间，表示当前代码里没有把它作为项目核心内存区使用

## 6. 为什么 `0x06000000 - 0x07ffffff` 也是物理地址

因为它来自平台物理内存定义：

- [trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:43](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:43)
- [optee_os/core/arch/arm/plat-vexpress/conf.mk:71](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/optee_os/core/arch/arm/plat-vexpress/conf.mk:71)

它表示的是：

- 安全世界使用的一段物理 DRAM

不是虚拟地址。

你之前一直觉得冲突，是因为把下面两件事当成了一件事：

1. Linux 可见主 RAM 的物理地址窗口
2. 整个 SoC/FVP 的完整物理地址空间

实际上：

- `0x06000000 - 0x07ffffff` 属于第 2 类
- `0x80000000 - 0xffffffff`、`0x880000000 - 0x8ffffffff` 同时属于第 1 类和第 2 类

## 7. DRAM1 是怎么被切成安全和非安全的

当前默认不是把第一段 DRAM 全部给 Linux。

第一段 DRAM 总范围：

- `0x80000000 - 0xffffffff`

其中：

- `0x80000000 - 0xfeffffff`
  是非安全 DRAM

- `0xff000000 - 0xffffffff`
  是安全 DRAM 窗口

来源：

- [trusted-firmware-a/include/plat/arm/common/arm_def.h:145](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/include/plat/arm/common/arm_def.h:145)
- [trusted-firmware-a/include/plat/arm/common/arm_def.h:197](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/include/plat/arm/common/arm_def.h:197)

也就是说，当前默认切法是：

- DRAM1 顶部 `16 MB` 划成安全区
- DRAM2 全部是非安全区

## 8. 和当前项目最相关的一句话

如果只保留一句话，就是：

- `Base FVP` 当前默认的完整物理地址空间里，同时存在独立的安全物理内存区 `0x06000000-0x07ffffff`、DRAM1 顶部的安全窗口 `0xff000000-0xffffffff`、以及两段非安全主 RAM，其中 `TZC-400` 负责把这些 RAM 物理区间切成 secure / non-secure 访问属性。

## 9. 本文档的来源文件

- [trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/board/fvp/include/platform_def.h:1)
- [trusted-firmware-a/plat/arm/board/fvp/fvp_def.h](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/plat/arm/board/fvp/fvp_def.h:1)
- [trusted-firmware-a/include/plat/arm/common/arm_def.h](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/trusted-firmware-a/include/plat/arm/common/arm_def.h:96)
- [linux/arch/arm64/boot/dts/arm/fvp-base-revc.dts](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/linux/arch/arm64/boot/dts/arm/fvp-base-revc.dts:168)
- [optee_os/core/arch/arm/plat-vexpress/conf.mk](/home/yyh/2.Programs/2.workplace/FVP/FVP-3.22.0/optee_os/core/arch/arm/plat-vexpress/conf.mk:65)
