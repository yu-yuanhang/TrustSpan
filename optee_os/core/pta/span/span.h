#ifndef SPAN_MANAGER_H
#define SPAN_MANAGER_H

#include <kernel/user_mode_ctx.h>
#include <tee_api_types.h>

struct span_request {
	paddr_t phys_addr;
	size_t size;
	vaddr_t ta_vaddr;
	bool mapped;
};

TEE_Result span_handle_protect(struct user_mode_ctx *uctx, paddr_t phys_addr,
			       size_t size, struct span_request *req);
TEE_Result span_handle_release(struct user_mode_ctx *uctx, vaddr_t ta_vaddr,
			       size_t size);

#endif /* SPAN_MANAGER_H */
