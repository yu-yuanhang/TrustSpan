#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define U(value) value##U
#define NOTICE(...) ((void)0)
#define ERROR(...) ((void)0)
#define IS_PAGE_ALIGNED(value) (((value) & 0xfffU) == 0U)

#define TRUSTSPAN_POOL_BASE UINT64_C(0xe0000000)
#define TRUSTSPAN_POOL_SIZE UINT64_C(0x08000000)
#define TRUSTSPAN_POOL_END (TRUSTSPAN_POOL_BASE + TRUSTSPAN_POOL_SIZE - 1U)
#define TRUSTSPAN_TZC_REGION2_NS_DRAM1 U(2)
#define TRUSTSPAN_TZC_DYNAMIC_REGION4 U(4)
#define TRUSTSPAN_TZC_DYNAMIC_REGION5 U(5)

#define ARM_NS_DRAM1_BASE UINT64_C(0x80000000)
#define ARM_NS_DRAM1_END UINT64_C(0xffffffff)
#define PLAT_ARM_TZC_FILTERS U(1)
#define ARM_TZC_NS_DRAM_S_ACCESS U(0)
#define PLAT_ARM_TZC_NS_DEV_ACCESS U(0x83038303)
#define TZC_REGION_S_RDWR U(3)
#define TZC_REGION_ATTR_SEC_SHIFT U(30)
#define TZC_REGION_ATTR_F_EN_SHIFT U(0)
#define TRUSTSPAN_TZC_REGION_ATTR_OFFSET U(0x110)
#define TRUSTSPAN_TZC_REGION_ID_ACCESS_OFFSET U(0x114)
#define PLAT_ARM_TZC_BASE ((uintptr_t)0x10000000U)

#define ARM_SIP_TRUSTSPAN_PROTECT_MEM U(0x82002010)
#define ARM_SIP_TRUSTSPAN_UNPROTECT_MEM U(0x82002011)
#define TRUSTSPAN_SMC_OK U(0)
#define TRUSTSPAN_SMC_E_DENIED U(1)
#define TRUSTSPAN_SMC_E_TZC_FAILED U(2)
#define SMC_UNK UINT64_C(0xffffffffffffffff)

typedef uint64_t u_register_t;
typedef unsigned int spinlock_t;

struct region_write {
	unsigned int filters;
	unsigned int region;
	uint64_t base;
	uint64_t top;
	unsigned int secure;
	unsigned int permissions;
};

struct mmio_write {
	uintptr_t address;
	uint32_t value;
};

static struct region_write region_writes[8];
static struct mmio_write mmio_writes[8];
static size_t region_write_count;
static size_t mmio_write_count;
static unsigned int access_switch_count;

static void reset_trace(void)
{
	region_write_count = 0;
	mmio_write_count = 0;
	access_switch_count = 0;
}

static uintptr_t span_region_reg(unsigned int offset, unsigned int region_no)
{
	return PLAT_ARM_TZC_BASE + offset + (uintptr_t)region_no * 0x20U;
}

static void tzc400_configure_region(unsigned int filters, unsigned int region,
				    uint64_t base, uint64_t top,
				    unsigned int secure,
				    unsigned int permissions)
{
	assert(region_write_count < sizeof(region_writes) / sizeof(region_writes[0]));
	region_writes[region_write_count++] = (struct region_write) {
		filters, region, base, top, secure, permissions
	};
}

static void mmio_write_32(uintptr_t address, uint32_t value)
{
	assert(mmio_write_count < sizeof(mmio_writes) / sizeof(mmio_writes[0]));
	mmio_writes[mmio_write_count++] = (struct mmio_write) { address, value };
}

static uint32_t mmio_read_32(uintptr_t address)
{
	(void)address;
	return 0;
}

static void dsbsy(void) {}

#include "tzc_boot_setup.inc"
#include "tzc_access.inc"

static void span_configure_region_access_counted(unsigned int region_no,
						 bool secure)
{
	access_switch_count++;
	span_configure_region_access(region_no, secure);
}

#define span_configure_region_access span_configure_region_access_counted
#include "tzc_transition.inc"
#undef span_configure_region_access

static void spin_lock(spinlock_t *lock) { (void)lock; }
static void spin_unlock(spinlock_t *lock) { (void)lock; }
static bool is_caller_non_secure(u_register_t flags) { return flags != 0; }
#define SMC_RET1(handle, value) return (uintptr_t)(value)
#include "span_sip.inc"

static void test_boot_layout(void)
{
	reset_trace();
	span_tzc_boot_setup();
	assert(region_write_count == 3);
	assert(region_writes[0].region == 2);
	assert(region_writes[0].base == ARM_NS_DRAM1_BASE);
	assert(region_writes[0].top == TRUSTSPAN_POOL_BASE - 1U);
	assert(region_writes[1].region == 4);
	assert(region_writes[1].base == TRUSTSPAN_POOL_BASE);
	assert(region_writes[1].top == TRUSTSPAN_POOL_END);
	assert(region_writes[2].region == 5);
	assert(region_writes[2].base == TRUSTSPAN_POOL_END + 1U);
	assert(region_writes[2].top == ARM_NS_DRAM1_END);
}

static void test_access_write_order(void)
{
	uintptr_t attr = span_region_reg(TRUSTSPAN_TZC_REGION_ATTR_OFFSET, 4);
	uintptr_t id = span_region_reg(TRUSTSPAN_TZC_REGION_ID_ACCESS_OFFSET, 4);

	reset_trace();
	assert(span_tzc_protect_region(TRUSTSPAN_POOL_BASE, 0x1000) == 0);
	assert(access_switch_count == 1 && mmio_write_count == 2);
	assert(mmio_writes[0].address == id && mmio_writes[0].value == 0);
	assert(mmio_writes[1].address == attr);

	reset_trace();
	assert(span_tzc_unprotect_region(TRUSTSPAN_POOL_BASE, 0x1000) == 0);
	assert(access_switch_count == 1 && mmio_write_count == 2);
	assert(mmio_writes[0].address == attr);
	assert(mmio_writes[1].address == id);
	assert(mmio_writes[1].value == PLAT_ARM_TZC_NS_DEV_ACCESS);
}

static void test_invalid_ranges_do_not_mutate(void)
{
	reset_trace();
	assert(span_tzc_protect_region(TRUSTSPAN_POOL_BASE - 0x1000, 0x1000) != 0);
	assert(span_tzc_protect_region(TRUSTSPAN_POOL_BASE + 1, 0x1000) != 0);
	assert(span_tzc_protect_region(TRUSTSPAN_POOL_END - 0xfff, 0x2000) != 0);
	assert(access_switch_count == 0 && mmio_write_count == 0);
}

static uintptr_t call_smc(uint32_t fid, uint64_t pa, uint64_t size,
			  u_register_t flags)
{
	return span_sip_smc_handler(fid, pa >> 32, (uint32_t)pa,
				    size >> 32, (uint32_t)size,
				    NULL, NULL, flags);
}

static void test_smc_state_machine(void)
{
	const uint64_t pa = TRUSTSPAN_POOL_BASE;
	const uint64_t size = 0x2000;

	reset_trace();
	assert(call_smc(ARM_SIP_TRUSTSPAN_PROTECT_MEM, pa, size, 1) ==
	       TRUSTSPAN_SMC_E_DENIED);
	assert(access_switch_count == 0);
	assert(call_smc(ARM_SIP_TRUSTSPAN_PROTECT_MEM, pa, size, 0) ==
	       TRUSTSPAN_SMC_OK);
	assert(call_smc(ARM_SIP_TRUSTSPAN_PROTECT_MEM, pa, size, 0) ==
	       TRUSTSPAN_SMC_E_DENIED);
	assert(call_smc(ARM_SIP_TRUSTSPAN_UNPROTECT_MEM, pa, size + 0x1000, 0) ==
	       TRUSTSPAN_SMC_E_DENIED);
	assert(call_smc(ARM_SIP_TRUSTSPAN_UNPROTECT_MEM, pa, size, 0) ==
	       TRUSTSPAN_SMC_OK);
	assert(call_smc(ARM_SIP_TRUSTSPAN_UNPROTECT_MEM, pa, size, 0) ==
	       TRUSTSPAN_SMC_E_DENIED);
	assert(access_switch_count == 2);
}

int main(void)
{
	test_boot_layout();
	test_access_write_order();
	test_invalid_ranges_do_not_mutate();
	test_smc_state_machine();
	puts("TrustSpan fixed-pool TZC transition tests passed");
	return 0;
}
