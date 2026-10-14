/* 状态机测试：VM/TF-A 用可注入失败的桩代替，状态迁移来自生产 span.c。 */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t TEE_Result;
typedef uintptr_t vaddr_t;
typedef uintptr_t paddr_t;
typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } TEE_UUID;
#define CFG_NUM_THREADS 2
/* 与 FVP 的 ConfInfer 固定共享窗口配置保持一致。 */
#define CFG_CONFINFER_SHARED_SPAN_VA ((uintptr_t)0x7f800000)
#define CFG_CONFINFER_SHARED_SPAN_SIZE ((size_t)0x800000)
#define SMALL_PAGE_SIZE 4096
#define IS_ALIGNED(v, a) (((v) & ((a) - 1)) == 0)
#define ADD_OVERFLOW(a, b, out) __builtin_add_overflow(a, b, out)
#define TEE_SUCCESS 0
#define TEE_ERROR_BAD_PARAMETERS 1
#define TEE_ERROR_BUSY 2
#define TEE_ERROR_ITEM_NOT_FOUND 3
#define TEE_ERROR_ACCESS_CONFLICT 4
#define TEE_ERROR_ACCESS_DENIED 5
#define TEE_ERROR_OUT_OF_MEMORY 6
#define TEE_ERROR_GENERIC 7
#define TEE_ERROR_BAD_STATE 8
#define TEE_MATTR_MEM_TYPE_CACHED 0
#define TEE_MATTR_UR (1U << 0)
#define TEE_MATTR_UW (1U << 1)
#define TEE_MATTR_PR (1U << 2)
#define TEE_MATTR_PW (1U << 3)
#define TEE_MATTR_URW (TEE_MATTR_UR | TEE_MATTR_UW)
#define TEE_MATTR_PRW (TEE_MATTR_PR | TEE_MATTR_PW)
#define VM_FLAG_SHAREABLE 1
#define VM_FLAG_CONFINFER_SHARED_SPAN 2
struct mutex { pthread_mutex_t native; };
#define MUTEX_INITIALIZER { PTHREAD_MUTEX_INITIALIZER }
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->native)); }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->native)); }
struct user_mode_ctx { bool mapped; TEE_UUID uuid; };
struct ts_session { struct user_mode_ctx *ctx; };
static struct ts_session *ts_get_calling_session(void) { return NULL; }
static bool is_user_ta_ctx(void *ctx) { return ctx != NULL; }
static struct user_mode_ctx *to_user_mode_ctx(struct user_mode_ctx *ctx) { return ctx; }
static void core_mmu_get_user_va_range(vaddr_t *base, size_t *size)
{ *base = 0x40000000; *size = 0x40000000; }
struct mobj { size_t size; };
static struct mobj fake_mobj;
static TEE_Result map_failure, unmap_failure, set_prot_failure;
static TEE_Result protect_failure, unprotect_failure;
static bool secure;
static unsigned int protects, unprotects;
static struct user_mode_ctx controller, worker;
static void (*during_map)(void);
static void (*during_unprotect)(void);
static struct mobj *span_secure_phys_alloc(paddr_t pa, size_t size, uint32_t type)
{ (void)pa; (void)type; fake_mobj.size = size; return &fake_mobj; }
static void mobj_put(struct mobj *m) { (void)m; }
static bool mobj_check_offset_and_len(struct mobj *m, size_t off, size_t size)
{ return m && off <= m->size && size <= m->size - off; }
static TEE_Result vm_map(struct user_mode_ctx *ctx, vaddr_t *va, size_t size,
                         uint32_t prot, uint32_t flags, struct mobj *m, size_t off)
{
    (void)prot; (void)flags;
    if (map_failure) return map_failure;
    assert(secure && mobj_check_offset_and_len(m, off, size));
    assert(*va == CFG_CONFINFER_SHARED_SPAN_VA + off);
    if (!off) assert(!ctx->mapped);
    ctx->mapped = true;
    if (during_map) during_map();
    return TEE_SUCCESS;
}
static TEE_Result vm_unmap(struct user_mode_ctx *ctx, vaddr_t va, size_t size)
{
    (void)va; (void)size;
    if (unmap_failure) return unmap_failure;
    assert(secure && ctx->mapped);
    ctx->mapped = false;
    return TEE_SUCCESS;
}
static TEE_Result vm_set_prot(struct user_mode_ctx *ctx, vaddr_t va, size_t size,
                              uint32_t prot)
{
    assert(ctx->mapped && va == CFG_CONFINFER_SHARED_SPAN_VA && size);
    assert(prot == (TEE_MATTR_UR | TEE_MATTR_PR));
    return set_prot_failure;
}
static TEE_Result span_call_tfa_protect(paddr_t pa, size_t size)
{
    (void)pa; (void)size;
    protects++;
    if (protect_failure) return protect_failure;
    assert(!secure);
    secure = true;
    return TEE_SUCCESS;
}
static TEE_Result span_call_tfa_unprotect(paddr_t pa, size_t size)
{
    (void)pa; (void)size;
    unprotects++;
    assert(secure && !controller.mapped && !worker.mapped);
    if (during_unprotect) during_unprotect();
    if (unprotect_failure) return unprotect_failure;
    secure = false;
    return TEE_SUCCESS;
}

#include "span_lifecycle.inc"

static void reset(void)
{
    assert(!secure && !controller.mapped && !worker.mapped);
    span_shared_reset_locked();
    memset(&controller, 0, sizeof(controller));
    memset(&worker, 0, sizeof(worker));
    protects = unprotects = 0;
    map_failure = unmap_failure = set_prot_failure = 0;
    protect_failure = unprotect_failure = 0;
    during_map = during_unprotect = NULL;
}
static uint32_t create(void)
{
    uint32_t handle = 0;
    vaddr_t va = 0;
    assert(span_shared_create(&controller, 0x88000000, 4096, &handle, &va) == 0);
    assert(va == CFG_CONFINFER_SHARED_SPAN_VA && handle);
    return handle;
}
static void attach(uint32_t handle)
{
    vaddr_t va = 0;
    size_t size = 0;
    assert(span_shared_attach(&worker, handle, &va, &size) == 0);
    assert(va == CFG_CONFINFER_SHARED_SPAN_VA && size == 4096);
}
static void destroy(struct user_mode_ctx *ctx)
{
    span_uctx_destroying(ctx);
    ctx->mapped = false; /* 模拟 vm_info_final 已删除页表。 */
    span_uctx_destroyed(ctx);
}
static void destroy_controller(void) { destroy(&controller); }
static void repeated_hook(void) { span_uctx_destroyed(&controller); }

int main(void)
{
    uint32_t h = 0, ignored = 0;
    vaddr_t va = 0;
    size_t size = 0;

    reset();
    assert(span_shared_create(&controller, 0x88000000,
                              CFG_CONFINFER_SHARED_SPAN_SIZE + SMALL_PAGE_SIZE,
                              &ignored, &va) == TEE_ERROR_BAD_PARAMETERS);
    assert(!protects && !secure);
    assert(span_shared_create(&controller, 0x88000000, 0x45f000,
                              &h, &va) == TEE_SUCCESS);
    assert(span_shared_freeze_layout(&controller, h, 0x1000, 0x1000,
                                     0x45e000) == TEE_SUCCESS);
    assert(span_shared_attach(&worker, h, &va, &size) == TEE_SUCCESS);
    assert(va == CFG_CONFINFER_SHARED_SPAN_VA && size == 0x45f000);
    assert(span_shared_detach(&worker, h) == TEE_SUCCESS);
    assert(span_shared_release(&controller, h) == TEE_SUCCESS);
    assert(!secure && protects == 1 && unprotects == 1);
    puts("PASS 0x45f000 region -> freeze and split worker mapping");

    reset();
    h = create(); attach(h);
    assert(span_shared_release(&controller, h) == TEE_ERROR_BUSY);
    destroy(&controller);
    assert(secure && !unprotects);
    assert(span_shared_detach(&worker, h) == 0);
    assert(!secure && unprotects == 1);
    puts("PASS controller destroy -> final explicit detach");

    reset();
    h = create(); attach(h);
    destroy(&worker);
    unmap_failure = TEE_ERROR_GENERIC;
    assert(span_shared_release(&controller, h) == TEE_ERROR_GENERIC);
    assert(secure && controller.mapped && !unprotects);
    unmap_failure = 0;
    unprotect_failure = TEE_ERROR_GENERIC;
    assert(span_shared_release(&controller, h) == TEE_ERROR_GENERIC);
    assert(secure && !controller.mapped &&
           span_shared_region.state == SPAN_SHARED_REGION_RELEASING);
    unprotect_failure = 0;
    assert(span_shared_release(&controller, h) == 0);
    assert(!secure && unprotects == 2);
    puts("PASS unmap/unprotect failure -> retained registry and retry");

    reset();
    map_failure = TEE_ERROR_OUT_OF_MEMORY;
    unprotect_failure = TEE_ERROR_GENERIC;
    assert(span_shared_create(&controller, 0x88000000, 4096, &ignored, &va) != 0);
    assert(secure && span_shared_region.state == SPAN_SHARED_REGION_RELEASING);
    assert(span_shared_create(&worker, 0x88001000, 4096, &ignored, &va) ==
           TEE_ERROR_BUSY);
    unprotect_failure = 0;
    destroy(&controller);
    assert(!secure && unprotects == 2);
    puts("PASS CREATE rollback failure retains ownership");

    reset();
    h = create();
    during_map = destroy_controller;
    assert(span_shared_attach(&worker, h, &va, &size) == TEE_ERROR_ACCESS_CONFLICT);
    assert(!secure && !worker.mapped && unprotects == 1);
    puts("PASS controller destroy during ATTACH -> rollback before unprotect");

    reset();
    h = create(); attach(h);
    destroy(&controller);
    during_unprotect = repeated_hook;
    destroy(&worker);
    span_uctx_destroyed(&worker);
    assert(!secure && protects == 1 && unprotects == 1);
    puts("PASS reentrant/repeated destroyed hook -> single unprotect");

    reset();
    protect_failure = TEE_ERROR_ACCESS_DENIED;
    assert(span_shared_create(&controller, 0x88000000, 4096, &ignored, &va) ==
           TEE_ERROR_ACCESS_DENIED);
    assert(!secure && span_shared_region.state == SPAN_SHARED_REGION_EMPTY &&
           protects == 1 && unprotects == 0);
    puts("PASS protect rejection -> EMPTY without unprotect");
    return 0;
}
