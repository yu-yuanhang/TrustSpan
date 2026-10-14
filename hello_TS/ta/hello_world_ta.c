/*
 * Copyright (c) 2016, Linaro Limited
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>
#include <inttypes.h>

#include <hello_world_ta.h>
#include <pta_span.h>

static TEE_Result call_pta_span_protect(uint64_t phys_addr, uint64_t size,
					uint64_t *ta_vaddr, uint64_t *ta_size)
{
	TEE_TASessionHandle sess = TEE_HANDLE_NULL;
	TEE_UUID uuid = PTA_SPAN_UUID;
	TEE_Result res = TEE_ERROR_GENERIC;
	uint32_t origin = 0;
	TEE_Param params[TEE_NUM_PARAMS] = { };
	uint32_t ptypes = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_VALUE_OUTPUT,
					  TEE_PARAM_TYPE_VALUE_OUTPUT);

	res = TEE_OpenTASession(&uuid, TEE_TIMEOUT_INFINITE, 0, NULL,
				&sess, &origin);
	if (res != TEE_SUCCESS) {
		EMSG("Open span PTA failed 0x%x origin 0x%x", res, origin);
		return res;
	}

	params[0].value.a = (uint32_t)(phys_addr >> 32);
	params[0].value.b = (uint32_t)(phys_addr & 0xffffffff);
	params[1].value.a = (uint32_t)(size >> 32);
	params[1].value.b = (uint32_t)(size & 0xffffffff);

	res = TEE_InvokeTACommand(sess, TEE_TIMEOUT_INFINITE,
				  PTA_SPAN_CMD_PROTECT, ptypes,
				  params, &origin);
	EMSG("after TEE_InvokeTACommand : PTA_SPAN_CMD_PROTECT "
	     "res=0x%x origin=0x%x p2=(0x%x,0x%x) p3=(0x%x,0x%x)",
	     res, origin,
	     params[2].value.a, params[2].value.b,
	     params[3].value.a, params[3].value.b);
	if (res != TEE_SUCCESS)
		EMSG("Invoke span PTA failed 0x%x origin 0x%x", res, origin);
	else if (ta_vaddr && ta_size) {
		*ta_vaddr = ((uint64_t)params[2].value.a << 32) |
			    params[2].value.b;
		*ta_size = ((uint64_t)params[3].value.a << 32) |
			   params[3].value.b;
		EMSG("PTA_SPAN_CMD_PROTECT return ta_vaddr=0x%016" PRIx64
		     " ta_size=0x%016" PRIx64,
		     *ta_vaddr, *ta_size);

	}

	TEE_CloseTASession(sess);
	return res;
}

/*
 * Called when the instance of the TA is created. This is the first call in
 * the TA.
 */
TEE_Result TA_CreateEntryPoint(void)
{
	DMSG("has been called");

	return TEE_SUCCESS;
}

/*
 * Called when the instance of the TA is destroyed if the TA has not
 * crashed or panicked. This is the last call in the TA.
 */
void TA_DestroyEntryPoint(void)
{
	DMSG("has been called");
}

/*
 * Called when a new session is opened to the TA. *sess_ctx can be updated
 * with a value to be able to identify this session in subsequent calls to the
 * TA. In this function you will normally do the global initialization for the
 * TA.
 */
TEE_Result TA_OpenSessionEntryPoint(uint32_t param_types,
		TEE_Param __maybe_unused params[4],
		void __maybe_unused **sess_ctx)
{
	uint32_t exp_param_types = TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
						   TEE_PARAM_TYPE_NONE,
						   TEE_PARAM_TYPE_NONE,
						   TEE_PARAM_TYPE_NONE);

	DMSG("has been called");

	if (param_types != exp_param_types)
		return TEE_ERROR_BAD_PARAMETERS;

	/* Unused parameters */
	(void)&params;
	(void)&sess_ctx;

	/*
	 * The DMSG() macro is non-standard, TEE Internal API doesn't
	 * specify any means to logging from a TA.
	 */
	IMSG("hello_TS session opened");

	/* If return value != TEE_SUCCESS the session will not be created. */
	return TEE_SUCCESS;
}

/*
 * Called when a session is closed, sess_ctx hold the value that was
 * assigned by TA_OpenSessionEntryPoint().
 */
void TA_CloseSessionEntryPoint(void __maybe_unused *sess_ctx)
{
	(void)&sess_ctx; /* Unused parameter */
	IMSG("hello_TS session closed");
}

static TEE_Result inc_value(uint32_t param_types,
	TEE_Param params[4])
{
	uint32_t exp_param_types = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INOUT,
					    TEE_PARAM_TYPE_VALUE_INPUT,
					    TEE_PARAM_TYPE_VALUE_INPUT,
					    TEE_PARAM_TYPE_NONE);
	uint64_t phys_addr;
	uint64_t size;
	uint64_t ta_vaddr = 0;
	uint64_t ta_size = 0;
	uint32_t *buf = NULL;

	DMSG("has been called");

	if (param_types != exp_param_types)
		return TEE_ERROR_BAD_PARAMETERS;

	phys_addr = ((uint64_t)params[1].value.a << 32) | params[1].value.b;
	size = params[2].value.a;

	IMSG("Got value: %u from NW", params[0].value.a);
	IMSG("Got protect request: phys=0x%016" PRIx64 " size=0x%016" PRIx64,
	     phys_addr, size);
	if (call_pta_span_protect(phys_addr, size, &ta_vaddr, &ta_size) !=
	    TEE_SUCCESS)
		return TEE_ERROR_GENERIC;
	IMSG("PTA returned ta_vaddr=0x%016" PRIx64 " size=0x%016" PRIx64,
	     ta_vaddr, ta_size);
	if (!ta_vaddr || ta_size < (2 * sizeof(uint32_t)))
		return TEE_ERROR_GENERIC;

	buf = (uint32_t *)(uintptr_t)ta_vaddr;
	IMSG("TA read mapped memory before write: [0]=0x%08" PRIx32
	     " [1]=0x%08" PRIx32, buf[0], buf[1]);
	buf[0] = 0xaabbccdd;
	buf[1] = 0x12345678;
	IMSG("TA read mapped memory after write: [0]=0x%08" PRIx32
	     " [1]=0x%08" PRIx32, buf[0], buf[1]);
	params[0].value.a++;
	IMSG("Increase value to: %u", params[0].value.a);

	return TEE_SUCCESS;
}

static TEE_Result dec_value(uint32_t param_types,
	TEE_Param params[4])
{
    uint32_t exp_param_types = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INOUT,
                                            TEE_PARAM_TYPE_NONE,
                                            TEE_PARAM_TYPE_NONE,
                                            TEE_PARAM_TYPE_NONE);

	DMSG("has been called");

	if (param_types != exp_param_types)
		return TEE_ERROR_BAD_PARAMETERS;

	IMSG("Got value: %u from NW", params[0].value.a);
	params[0].value.a--;
	IMSG("Decrease value to: %u", params[0].value.a);

	return TEE_SUCCESS;
}

/*
 * Called when a TA is invoked. sess_ctx hold that value that was
 * assigned by TA_OpenSessionEntryPoint(). The rest of the paramters
 * comes from normal world.
 */
TEE_Result TA_InvokeCommandEntryPoint(void __maybe_unused *sess_ctx,
			uint32_t cmd_id,
			uint32_t param_types, TEE_Param params[4])
{
	(void)&sess_ctx; /* Unused parameter */

	switch (cmd_id) {
	case TA_HELLO_TS_CMD_INC_VALUE:
		return inc_value(param_types, params);
	case TA_HELLO_TS_CMD_DEC_VALUE:
		return dec_value(param_types, params);
	default:
		return TEE_ERROR_BAD_PARAMETERS;
	}
}
