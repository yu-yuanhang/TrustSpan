/*
 * Copyright (c) 2016, Linaro Limited
 * All rights reserved.
 */

#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <tee_client_api.h>

#include <hello_world_ta.h>
#include <trustspan_mem_test_uapi.h>

#define TRUSTSPAN_MEM_TEST_DEV "/dev/trustspan_mem_test"

static int trustspan_mem_test_open(void)
{
	int fd;

	fd = open(TRUSTSPAN_MEM_TEST_DEV, O_RDWR);
	if (fd < 0)
		err(1, "open %s failed", TRUSTSPAN_MEM_TEST_DEV);

	return fd;
}

static void trustspan_mem_test_alloc(int fd, struct trustspan_mem_info *info)
{
	info->size = 4 * 1024 * 1024;
	if (ioctl(fd, TRUSTSPAN_MEM_TEST_IOC_ALLOC, info) < 0)
		err(1, "ioctl ALLOC failed");

	printf("TrustSpan alloc success:\n");
	printf("  phys_addr = 0x%016" PRIx64 "\n", info->phys_addr);
	printf("  size      = 0x%016" PRIx64 " (%" PRIu64 ")\n",
	       info->size, info->size);
	printf("  allocated = %" PRIu32 "\n", info->allocated);
	printf("  state     = %" PRIu32 "\n", info->state);

	if (ioctl(fd, TRUSTSPAN_MEM_TEST_IOC_QUERY, info) < 0) {
		close(fd);
		err(1, "ioctl QUERY failed");
	}

	printf("TrustSpan query result:\n");
	printf("  phys_addr = 0x%016" PRIx64 "\n", info->phys_addr);
	printf("  size      = 0x%016" PRIx64 " (%" PRIu64 ")\n",
	       info->size, info->size);
	printf("  allocated = %" PRIu32 "\n", info->allocated);
	printf("  state     = %" PRIu32 "\n", info->state);
}

static void *trustspan_mem_test_map(int fd, const struct trustspan_mem_info *info)
{
	void *user_vaddr;
	uint32_t *words;

	user_vaddr = mmap(NULL, info->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			  fd, 0);
	if (user_vaddr == MAP_FAILED)
		err(1, "mmap failed");

	printf("TrustSpan mmap success:\n");
	printf("  user_vaddr = %p\n", user_vaddr);

	words = user_vaddr;
	words[0] = 0x11223344;
	words[1] = 0x55667788;

	printf("TrustSpan baseline access success:\n");
	printf("  user_vaddr[0] = 0x%08" PRIx32 "\n", words[0]);
	printf("  user_vaddr[1] = 0x%08" PRIx32 "\n", words[1]);
	return user_vaddr;
}

static void trustspan_mem_test_free(int fd)
{
	if (ioctl(fd, TRUSTSPAN_MEM_TEST_IOC_FREE) < 0)
		err(1, "ioctl FREE failed");

	printf("TrustSpan free success\n");
}

static void trustspan_mem_test_prepare_secure(int fd,
					      struct trustspan_mem_info *info)
{
	if (ioctl(fd, TRUSTSPAN_MEM_TEST_IOC_PREPARE_SECURE) < 0)
		err(1, "ioctl PREPARE_SECURE failed");
	if (ioctl(fd, TRUSTSPAN_MEM_TEST_IOC_QUERY, info) < 0)
		err(1, "ioctl QUERY after PREPARE_SECURE failed");

	printf("TrustSpan prepare secure success:\n");
	printf("  state = %" PRIu32 "\n", info->state);
}

static void trustspan_mem_test_mark_protected(int fd,
					      struct trustspan_mem_info *info)
{
	if (ioctl(fd, TRUSTSPAN_MEM_TEST_IOC_MARK_PROTECTED) < 0)
		err(1, "ioctl MARK_PROTECTED failed");

	if (ioctl(fd, TRUSTSPAN_MEM_TEST_IOC_QUERY, info) < 0)
		err(1, "ioctl QUERY after MARK_PROTECTED failed");

	printf("TrustSpan mark protected success:\n");
	printf("  state = %" PRIu32 "\n", info->state);
}

int main(void)
{
	TEEC_Result res;
	TEEC_Context ctx;
	TEEC_Session sess;
	TEEC_Operation op;
	TEEC_UUID uuid = TA_HELLO_TS_UUID;
	uint32_t err_origin;
	struct trustspan_mem_info info = { };
	void *user_vaddr;
	int trustspan_fd;

	res = TEEC_InitializeContext(NULL, &ctx);
	if (res != TEEC_SUCCESS)
		errx(1, "TEEC_InitializeContext failed with code 0x%x", res);

	res = TEEC_OpenSession(&ctx, &sess, &uuid,
			       TEEC_LOGIN_PUBLIC, NULL, NULL, &err_origin);
	if (res != TEEC_SUCCESS)
		errx(1, "TEEC_Opensession failed with code 0x%x origin 0x%x",
			res, err_origin);

	/*
	 * Execute a function in the TA by invoking it, in this case
	 * we're incrementing a number.
	 */

	trustspan_fd = trustspan_mem_test_open();
	trustspan_mem_test_alloc(trustspan_fd, &info);
	user_vaddr = trustspan_mem_test_map(trustspan_fd, &info);

#if 0
	if (munmap(user_vaddr, info.size) < 0)
		err(1, "munmap failed");
	user_vaddr = NULL;

	printf("TrustSpan munmap success before secure handoff\n");
	trustspan_mem_test_prepare_secure(trustspan_fd, &info);
#endif

	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INOUT, TEEC_VALUE_INPUT,
					 TEEC_VALUE_INPUT, TEEC_NONE);
	op.params[0].value.a = 42;
	op.params[1].value.a = (uint32_t)(info.phys_addr >> 32);
	op.params[1].value.b = (uint32_t)(info.phys_addr & 0xffffffff);
	op.params[2].value.a = (uint32_t)info.size;

	printf("Invoking TA to increment %d\n", op.params[0].value.a);
	printf("Passing to TA: phys=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
	       info.phys_addr, info.size);
	res = TEEC_InvokeCommand(&sess, TA_HELLO_TS_CMD_INC_VALUE, &op,
			 &err_origin);
	if (res != TEEC_SUCCESS)
		errx(1, "TEEC_InvokeCommand failed with code 0x%x origin 0x%x",
			res, err_origin);
	printf("TA incremented value to %d\n", op.params[0].value.a);

	if (user_vaddr) {
		uint32_t *words = user_vaddr;

		printf("TrustSpan REE read after TA:\n");
		printf("  user_vaddr[0] = 0x%08" PRIx32 "\n", words[0]);
		printf("  user_vaddr[1] = 0x%08" PRIx32 "\n", words[1]);
		words[0] = 0xdeadbeef;
		words[1] = 0xcafebabe;

		printf("TrustSpan REE write after TA:\n");
		printf("  user_vaddr[0] = 0x%08" PRIx32 "\n", words[0]);
		printf("  user_vaddr[1] = 0x%08" PRIx32 "\n", words[1]);
	}

	// trustspan_mem_test_mark_protected(trustspan_fd, &info);

	close(trustspan_fd);
	TEEC_CloseSession(&sess);
	TEEC_FinalizeContext(&ctx);
	return 0;
}
