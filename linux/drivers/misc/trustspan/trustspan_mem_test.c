// SPDX-License-Identifier: GPL-2.0
/* ConfInfer system modification: SYS-001 (TrustSpan contiguous-memory driver). */
/*
 * TrustSpan 最小内存测试驱动。
 *
 * 当前只实现任务 1：
 * - 使用 coherent DMA/CMA 分配物理连续页
 * - 通过 ioctl 向用户态暴露 phys addr + size
 * - 通过 mmap() 把这段页映射到用户态
 * - 释放当前分配
 */

#include <linux/dma-direct.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <uapi/misc/trustspan_mem_test.h>

/* misc 设备名，注册后会对应生成 /dev/trustspan_mem_test。 */
#define TRUSTSPAN_MEM_TEST_NAME "trustspan_mem_test"
/* 第一版固定分配 2MB 连续物理页。 */
#define TRUSTSPAN_MEM_TEST_ALLOC_SIZE SZ_2M

struct trustspan_mem_ctx {
	struct device *dma_dev;
	void *cpu_addr;
	dma_addr_t dma_addr;
	phys_addr_t phys_addr;
	size_t size;
	bool allocated;
	pid_t owner_tgid;
	unsigned int mmap_refcnt;
	enum trustspan_mem_state state;
	struct mutex lock;
};

static struct trustspan_mem_ctx trustspan_ctx = {
	.size = TRUSTSPAN_MEM_TEST_ALLOC_SIZE,
	.state = TRUSTSPAN_MEM_STATE_IDLE,
};

static void trustspan_mem_fill_info(const struct trustspan_mem_ctx *ctx,
				    struct trustspan_mem_info *info)
{
	info->phys_addr = ctx->phys_addr;
	info->size = ctx->size;
	info->allocated = ctx->allocated;
	info->state = ctx->state;
}

/*
 * 调用者：TRUSTSPAN_MEM_TEST_IOC_ALLOC。
 *
 * 这里必须分配一段“实际物理地址连续”的内存，因为后续 TF-A/TZC
 * 会把 phys_addr 到 phys_addr + size 作为一个完整安全区间处理。
 * 不能继续使用 alloc_pages(get_order(size))：4 KiB 页配置下
 * MAX_ORDER=11 只支持到 order 10（4 MiB），请求一旦超过 4 MiB，
 * get_order() 就会得到无法分配的 order 11；而且即便提高 MAX_ORDER，
 * 长时间运行后也容易因为 buddy 碎片导致高阶分配失败。
 *
 * dma_alloc_coherent() 会优先从本平台已经保留的 CMA 区域按实际页数
 * 分配连续页，因此 0x45f000 只占用 0x45f000，而不是向上扩大成
 * 8 MiB buddy 块。dma_addr 是设备地址；FVP 使用 direct DMA，必须再由
 * dma_to_phys() 转成 CPU 物理地址后才能交给 TrustSpan/TF-A。
 */
static int trustspan_mem_do_alloc(struct trustspan_mem_ctx *ctx, size_t req_size)
{
	size_t alloc_size;

	if (ctx->allocated)
		return -EBUSY;
	if (!ctx->dma_dev)
		return -ENODEV;
	if (req_size > SIZE_MAX - (PAGE_SIZE - 1))
		return -EOVERFLOW;

	/*
	 * 保留默认配置作为兜底：
	 * - 用户态传 0，则按默认 2MB 分配
	 * - 其他情况按页对齐后分配
	 */
	alloc_size = req_size ? PAGE_ALIGN(req_size) : TRUSTSPAN_MEM_TEST_ALLOC_SIZE;
	ctx->size = alloc_size;
	ctx->cpu_addr = dma_alloc_coherent(ctx->dma_dev, ctx->size,
					  &ctx->dma_addr, GFP_KERNEL);
	if (!ctx->cpu_addr) {
		pr_err("%s: CMA/DMA alloc failed req=0x%zx aligned=0x%zx "
		       "pages=%zu\n",
		       TRUSTSPAN_MEM_TEST_NAME, req_size, alloc_size,
		       alloc_size >> PAGE_SHIFT);
		return -ENOMEM;
	}

	ctx->phys_addr = dma_to_phys(ctx->dma_dev, ctx->dma_addr);
	ctx->allocated = true;
	ctx->owner_tgid = current->tgid;
	ctx->mmap_refcnt = 0;
	ctx->state = TRUSTSPAN_MEM_STATE_ALLOCATED;

	pr_info("%s: alloc success\n", TRUSTSPAN_MEM_TEST_NAME);
	pr_info("%s: cpu_addr=%p dma=0x%llx phys=0x%llx size=0x%zx "
		"pages=%zu allocator=dma-coherent/CMA\n",
		TRUSTSPAN_MEM_TEST_NAME,
		ctx->cpu_addr, (unsigned long long)ctx->dma_addr,
		(unsigned long long)ctx->phys_addr, ctx->size,
		ctx->size >> PAGE_SHIFT);

	return 0;
}

/*
 * 调用者：FREE ioctl 与模块退出路径。
 * dma_alloc_coherent() 分配的区域必须使用配对的 dma_free_coherent()
 * 释放，不能再调用 __free_pages()，否则 CMA 位图不会被正确归还。
 */
static void trustspan_mem_do_free(struct trustspan_mem_ctx *ctx)
{
	if (!ctx->allocated)
		return;

	dma_free_coherent(ctx->dma_dev, ctx->size, ctx->cpu_addr,
			  ctx->dma_addr);
	ctx->cpu_addr = NULL;
	ctx->dma_addr = 0;
	ctx->phys_addr = 0;
	ctx->allocated = false;
	ctx->owner_tgid = 0;
	ctx->mmap_refcnt = 0;
	ctx->state = TRUSTSPAN_MEM_STATE_IDLE;

	pr_info("%s: free success\n", TRUSTSPAN_MEM_TEST_NAME);
}

static void trustspan_mem_vma_open(struct vm_area_struct *vma)
{
	struct trustspan_mem_ctx *ctx = vma->vm_private_data;

	mutex_lock(&ctx->lock);
	ctx->mmap_refcnt++;
	if (ctx->allocated && ctx->state != TRUSTSPAN_MEM_STATE_PREPARED &&
	    ctx->state != TRUSTSPAN_MEM_STATE_PROTECTED)
		ctx->state = TRUSTSPAN_MEM_STATE_MAPPED;
	mutex_unlock(&ctx->lock);
}

static void trustspan_mem_vma_close(struct vm_area_struct *vma)
{
	struct trustspan_mem_ctx *ctx = vma->vm_private_data;

	mutex_lock(&ctx->lock);
	if (ctx->mmap_refcnt)
		ctx->mmap_refcnt--;
	if (ctx->allocated && !ctx->mmap_refcnt &&
	    ctx->state == TRUSTSPAN_MEM_STATE_MAPPED)
		ctx->state = TRUSTSPAN_MEM_STATE_ALLOCATED;
	mutex_unlock(&ctx->lock);
}

static const struct vm_operations_struct trustspan_mem_vm_ops = {
	.open = trustspan_mem_vma_open,
	.close = trustspan_mem_vma_close,
};

static int trustspan_mem_prepare_secure(struct trustspan_mem_ctx *ctx)
{
	if (!ctx->allocated)
		return -EINVAL;

	if (ctx->state == TRUSTSPAN_MEM_STATE_PROTECTED)
		return -EPERM;

	if (ctx->mmap_refcnt)
		return -EBUSY;

	ctx->state = TRUSTSPAN_MEM_STATE_PREPARED;
	pr_info("%s: prepared for secure handoff phys=0x%llx size=0x%zx\n",
		TRUSTSPAN_MEM_TEST_NAME,
		(unsigned long long)ctx->phys_addr, ctx->size);

	return 0;
}

static int trustspan_mem_mark_protected(struct trustspan_mem_ctx *ctx)
{
	if (!ctx->allocated)
		return -EINVAL;

	if (ctx->state != TRUSTSPAN_MEM_STATE_PREPARED)
		return -EPERM;

	ctx->state = TRUSTSPAN_MEM_STATE_PROTECTED;
	pr_info("%s: marked protected phys=0x%llx size=0x%zx\n",
		TRUSTSPAN_MEM_TEST_NAME,
		(unsigned long long)ctx->phys_addr, ctx->size);

	return 0;
}

/*
 * TA unload 成功表示 Span PTA 已解除映射并通知 TF-A 撤销保护。
 * 驱动据此将页面从 secure handoff 状态恢复为普通已分配状态。
 */
static int trustspan_mem_release_secure(struct trustspan_mem_ctx *ctx)
{
	if (!ctx->allocated)
		return -EINVAL;

	if (ctx->state != TRUSTSPAN_MEM_STATE_PROTECTED)
		return -EPERM;

	if (ctx->mmap_refcnt)
		return -EBUSY;

	ctx->state = TRUSTSPAN_MEM_STATE_ALLOCATED;
	pr_info("%s: secure ownership released phys=0x%llx size=0x%zx\n",
		TRUSTSPAN_MEM_TEST_NAME,
		(unsigned long long)ctx->phys_addr, ctx->size);
	return 0;
}

/* TA 明确归还失败的 PREPARED Region 后 才允许 REE 恢复映射 */
static int trustspan_mem_cancel_secure(struct trustspan_mem_ctx *ctx)
{
	if (!ctx->allocated)
		return -EINVAL;
	if (ctx->state != TRUSTSPAN_MEM_STATE_PREPARED)
		return -EPERM;
	if (ctx->mmap_refcnt)
		return -EBUSY;

	ctx->state = TRUSTSPAN_MEM_STATE_ALLOCATED;
	pr_info("%s: secure handoff cancelled phys=0x%llx size=0x%zx\n",
		TRUSTSPAN_MEM_TEST_NAME,
		(unsigned long long)ctx->phys_addr, ctx->size);
	return 0;
}

/*
 * 这里返回给用户态的是“用户虚拟地址映射”能力，而不是把内核虚拟地址直接暴露出去。
 * 用户态通过 mmap() 后拿到自己的 vaddr，再去访问同一块物理页。
 *
 * 由于底层已经改成 dma_alloc_coherent()，这里也必须使用配对的
 * dma_mmap_coherent()，由 DMA 子系统选择与内核 CPU 映射一致的页属性；
 * 不能继续用普通 remap_pfn_range() 自行猜测缓存属性。vm_ops 仍负责统计
 * fork/split/munmap 产生的 VMA 生命周期，保证 secure handoff 前映射数为零。
 */
static int trustspan_mem_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long vma_size = vma->vm_end - vma->vm_start;
	int ret = 0;

	mutex_lock(&trustspan_ctx.lock);

	if (!trustspan_ctx.allocated) {
		ret = -EINVAL;
		goto out;
	}
	if (trustspan_ctx.state == TRUSTSPAN_MEM_STATE_PREPARED ||
	    trustspan_ctx.state == TRUSTSPAN_MEM_STATE_PROTECTED) {
		ret = -EPERM;
		goto out;
	}

	if (vma->vm_pgoff || vma_size > trustspan_ctx.size) {
		ret = -EINVAL;
		goto out;
	}

	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
	ret = dma_mmap_coherent(trustspan_ctx.dma_dev, vma,
				trustspan_ctx.cpu_addr, trustspan_ctx.dma_addr,
				trustspan_ctx.size);
	if (!ret) {
		vma->vm_ops = &trustspan_mem_vm_ops;
		vma->vm_private_data = &trustspan_ctx;
		trustspan_ctx.mmap_refcnt++;
		trustspan_ctx.state = TRUSTSPAN_MEM_STATE_MAPPED;
		pr_info("%s: mmap user_vaddr=0x%lx phys=0x%llx size=0x%lx\n",
			TRUSTSPAN_MEM_TEST_NAME, vma->vm_start,
			(unsigned long long)trustspan_ctx.phys_addr, vma_size);
	}

out:
	mutex_unlock(&trustspan_ctx.lock);
	return ret;
}

/*
 * 这是 /dev/trustspan_mem_test 对外暴露的 ioctl 入口。
 *
 * 这里还用到了内核提供的接口：
 * - mutex_lock()/mutex_unlock()：串行化访问全局上下文，避免并发冲突
 * - copy_to_user()：把查询结果从内核空间拷贝到用户空间
 */
static long trustspan_mem_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct trustspan_mem_info info = { };
	long ret = 0;

	mutex_lock(&trustspan_ctx.lock);

	switch (cmd) {
	case TRUSTSPAN_MEM_TEST_IOC_ALLOC:
		if (copy_from_user((void *)&info, (void __user *)arg, sizeof(info))) {
			ret = -EFAULT;
			break;
		}

		ret = trustspan_mem_do_alloc(&trustspan_ctx, info.size);
		if (!ret) {
			trustspan_mem_fill_info(&trustspan_ctx, &info);
			if (copy_to_user((void __user *)arg, &info, sizeof(info)))
				ret = -EFAULT;
		}
		break;
	case TRUSTSPAN_MEM_TEST_IOC_QUERY:
		trustspan_mem_fill_info(&trustspan_ctx, &info);
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			ret = -EFAULT;
		break;
	case TRUSTSPAN_MEM_TEST_IOC_PREPARE_SECURE:
		ret = trustspan_mem_prepare_secure(&trustspan_ctx);
		break;
	case TRUSTSPAN_MEM_TEST_IOC_MARK_PROTECTED:
		ret = trustspan_mem_mark_protected(&trustspan_ctx);
		break;
	case TRUSTSPAN_MEM_TEST_IOC_RELEASE_SECURE:
		ret = trustspan_mem_release_secure(&trustspan_ctx);
		break;
	case TRUSTSPAN_MEM_TEST_IOC_CANCEL_SECURE:
		ret = trustspan_mem_cancel_secure(&trustspan_ctx);
		break;
	case TRUSTSPAN_MEM_TEST_IOC_FREE:
		if (trustspan_ctx.state == TRUSTSPAN_MEM_STATE_PREPARED ||
		    trustspan_ctx.state == TRUSTSPAN_MEM_STATE_PROTECTED) {
			ret = -EPERM;
			break;
		}

		if (trustspan_ctx.mmap_refcnt) {
			ret = -EBUSY;
			break;
		}

		trustspan_mem_do_free(&trustspan_ctx);
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&trustspan_ctx.lock);

	return ret;
}

static const struct file_operations trustspan_mem_fops = {
	.owner = THIS_MODULE,
	/* 用户态执行 ioctl(fd, ...) 时，内核会回调到这里。 */
	.unlocked_ioctl = trustspan_mem_ioctl,
	.mmap = trustspan_mem_mmap,
#ifdef CONFIG_COMPAT
	.compat_ioctl = trustspan_mem_ioctl,
#endif
};

static struct miscdevice trustspan_mem_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = TRUSTSPAN_MEM_TEST_NAME,
	.fops = &trustspan_mem_fops,
};

/* 模块加载时调用的初始化函数。 */
static int trustspan_mem_probe(struct platform_device *pdev)
{
	int ret = 0;

	mutex_init(&trustspan_ctx.lock);
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	/*
	 * memory-region 指向 TrustSpan 独占的 CMA pool。该 pool 在 TF-A 启动
	 * 阶段已经对应独立 TZC region，因此运行期只切换访问属性，不再重切
	 * Linux 正在使用的 DRAM region。
	 */
	ret = of_reserved_mem_device_init(&pdev->dev);
	if (ret) {
		pr_err("%s: failed to bind reserved CMA pool: %d\n",
		       TRUSTSPAN_MEM_TEST_NAME, ret);
		return ret;
	}

	trustspan_ctx.dma_dev = &pdev->dev;
	/* misc_register() 会为这个 platform device 创建用户态控制节点。 */
	ret = misc_register(&trustspan_mem_miscdev);
	if (ret) {
		trustspan_ctx.dma_dev = NULL;
		of_reserved_mem_device_release(&pdev->dev);
		return ret;
	}
	platform_set_drvdata(pdev, &trustspan_ctx);
	return 0;
}

static int trustspan_mem_remove(struct platform_device *pdev)
{
	mutex_lock(&trustspan_ctx.lock);
	trustspan_mem_do_free(&trustspan_ctx);
	mutex_unlock(&trustspan_ctx.lock);

	misc_deregister(&trustspan_mem_miscdev);
	trustspan_ctx.dma_dev = NULL;
	of_reserved_mem_device_release(&pdev->dev);
	return 0;
}

static const struct of_device_id trustspan_mem_of_match[] = {
	{ .compatible = "trustspan,secure-cma" },
	{ }
};
MODULE_DEVICE_TABLE(of, trustspan_mem_of_match);

static struct platform_driver trustspan_mem_driver = {
	.probe = trustspan_mem_probe,
	.remove = trustspan_mem_remove,
	.driver = {
		.name = TRUSTSPAN_MEM_TEST_NAME,
		.of_match_table = trustspan_mem_of_match,
	},
};
module_platform_driver(trustspan_mem_driver);

MODULE_DESCRIPTION("TrustSpan minimal memory allocation test driver");
MODULE_LICENSE("GPL");
