/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * TrustSpan private SMC ABI used between OP-TEE and TF-A.
 *
 * This header is the OP-TEE-side mirror of the TF-A definition.
 * The protected range is later returned with the matching unprotect command.
 */

#ifndef TRUSTSPAN_SMC_H
#define TRUSTSPAN_SMC_H

#include <stdint.h>

#define TRUSTSPAN_SMC_PROTECT_MEM		UINT32_C(0x8200ff10)
#define TRUSTSPAN_SMC_UNPROTECT_MEM		UINT32_C(0x8200ff11)

#define TRUSTSPAN_SMC_OK			U(0)
#define TRUSTSPAN_SMC_E_INVALID_PARAM		U(1)
#define TRUSTSPAN_SMC_E_DENIED			U(2)
#define TRUSTSPAN_SMC_E_NOT_SUPPORTED		U(3)
#define TRUSTSPAN_SMC_E_TZC_FAILED		U(4)

#endif /* TRUSTSPAN_SMC_H */
