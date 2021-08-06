/*-
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Tollef Fog Heen <tfheen@redpill-linpro.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <ctype.h>
#include <string.h>
#include <unistd.h>

#include "vdef.h"

#include "vas.h"	// XXX Flexelint "not used" - but req'ed for assert()
#include "vsb.h"
#include "miniobj.h"

#include "vre.h"
#include "vre_pcre2.h"

#if !HAVE_PCRE2_SET_DEPTH_LIMIT
#  define pcre2_set_depth_limit(r, d) pcre2_set_recursion_limit(r, d)
#endif

#define VRE_PACKED_RE		(pcre2_code *)(-1)

struct vre {
	unsigned		magic;
#define VRE_MAGIC		0xe83097dc
	pcre2_code		*re;
	pcre2_match_context	*re_ctx;
};

/*
 * We don't want to spread or even expose the majority of PCRE2 options
 * and errors so we establish our own symbols and implement hard linkage
 * to PCRE2 here.
 */
const int VRE_ERROR_NOMATCH = PCRE2_ERROR_NOMATCH;

const unsigned VRE_CASELESS = PCRE2_CASELESS;

/*
 * Even though we only have one for each case so far, keep track of masks
 * to differentiate between compile and match options and enfore the hard
 * VRE linkage.
 */
#define VRE_MASK_COMPILE	PCRE2_CASELESS
#define VRE_MASK_MATCH		0

vre_t *
VRE_compile(const char *pattern, unsigned options,
    int *errptr, int *erroffset, unsigned jit)
{
	PCRE2_SIZE erroff;
	vre_t *v;

	AN(pattern);
	AZ(options & (~VRE_MASK_COMPILE));
	AN(errptr);
	AN(erroffset);

	*errptr = 0;
	*erroffset = -1;

	ALLOC_OBJ(v, VRE_MAGIC);
	if (v == NULL) {
		*errptr = PCRE2_ERROR_NOMEMORY;
		return (NULL);
	}
	v->re = pcre2_compile((PCRE2_SPTR8)pattern, PCRE2_ZERO_TERMINATED,
	    options, errptr, &erroff, NULL);
	*erroffset = erroff;
	if (v->re == NULL) {
		VRE_free(&v);
		return (NULL);
	}
	v->re_ctx = pcre2_match_context_create(NULL);
	if (v->re_ctx == NULL) {
		*errptr = PCRE2_ERROR_NOMEMORY;
		VRE_free(&v);
		return (NULL);
	}
#if USE_PCRE2_JIT
	if (jit)
		(void)pcre2_jit_compile(v->re, 0);
#else
	(void)jit;
#endif
	return (v);
}

int
VRE_error(struct vsb *vsb, int err)
{
	char buf[VRE_ERROR_LEN];
	int i;

	CHECK_OBJ_NOTNULL(vsb, VSB_MAGIC);
	i = pcre2_get_error_message(err, (PCRE2_UCHAR *)buf, VRE_ERROR_LEN);
	if (i == PCRE2_ERROR_BADDATA) {
		VSB_printf(vsb, "unknown pcre2 error code (%d)", err);
		return (-1);
	}
	VSB_cat(vsb, buf);
	return (0);
}

pcre2_code *
VRE_unpack(const vre_t *code)
{

	CHECK_OBJ_NOTNULL(code, VRE_MAGIC);
	if (code->re == VRE_PACKED_RE) {
		AZ(code->re_ctx);
		return (TRUST_ME(code + 1));
	}
	return (code->re);
}

static void
vre_limit(const vre_t *code, const volatile struct vre_limits *lim)
{

	CHECK_OBJ_NOTNULL(code, VRE_MAGIC);

	if (lim == NULL)
		return;

	assert(code->re != VRE_PACKED_RE);

	/* XXX: not reentrant */
	AN(code->re_ctx);
	AZ(pcre2_set_match_limit(code->re_ctx, lim->match));
	AZ(pcre2_set_depth_limit(code->re_ctx, lim->depth));
}

vre_t *
VRE_export(const vre_t *code, size_t *sz)
{
	pcre2_code *re;
	vre_t *exp;

	CHECK_OBJ_NOTNULL(code, VRE_MAGIC);
	re = VRE_unpack(code);
	AZ(pcre2_pattern_info(re, PCRE2_INFO_SIZE, sz));

	exp = malloc(sizeof(*exp) + *sz);
	if (exp == NULL)
		return (NULL);

	INIT_OBJ(exp, VRE_MAGIC);
	exp->re = VRE_PACKED_RE;
	memcpy(exp + 1, re, *sz);
	*sz += sizeof(*exp);
	return (exp);
}

static int
vre_match(const vre_t *code, const char *subject, size_t length, size_t offset,
    int options, pcre2_match_data **datap)
{
	pcre2_match_data *data;
	pcre2_code *re;
	int matches;

	re = VRE_unpack(code);

	if (datap != NULL && *datap != NULL) {
		data = *datap;
		*datap = NULL;
	} else {
		data = pcre2_match_data_create_from_pattern(re, NULL);
		AN(data);
	}

	matches =  pcre2_match(re, (PCRE2_SPTR)subject, length, offset,
	    options, data, code->re_ctx);

	if (datap != NULL && matches > VRE_ERROR_NOMATCH)
		*datap = data;
	else
		pcre2_match_data_free(data);
	return (matches);
}

int
VRE_match(const vre_t *code, const char *subject, size_t length,
    int options, const volatile struct vre_limits *lim)
{

	CHECK_OBJ_NOTNULL(code, VRE_MAGIC);
	AN(subject);
	AZ(options & (~VRE_MASK_MATCH));

	if (length == 0)
		length = PCRE2_ZERO_TERMINATED;
	vre_limit(code, lim);
	return (vre_match(code, subject, length, 0, options, NULL));
}

int
VRE_sub(const vre_t *code, const char *subject, const char *replacement,
    struct vsb *vsb, const volatile struct vre_limits *lim, int all)
{
	pcre2_match_data *data = NULL;
	PCRE2_SIZE *ovector;
	uint32_t nov;
	int i, l;
	const char *s;
	unsigned x;
	int offset = 0;

	CHECK_OBJ_NOTNULL(code, VRE_MAGIC);
	CHECK_OBJ_NOTNULL(vsb, VSB_MAGIC);
	AN(subject);
	AN(replacement);

	vre_limit(code, lim);
	i = vre_match(code, subject, PCRE2_ZERO_TERMINATED, offset, 0, &data);

	if (i <= VRE_ERROR_NOMATCH)
		return (i);

	do {
		AN(data);
		ovector = pcre2_get_ovector_pointer(data);
		nov = pcre2_get_ovector_count(data);
		AN(ovector);

		/* Copy prefix to match */
		VSB_bcat(vsb, subject + offset, ovector[0] - offset);
		for (s = replacement; *s != '\0'; s++ ) {
			if (*s != '\\' || s[1] == '\0') {
				VSB_putc(vsb, *s);
				continue;
			}
			s++;
			if (isdigit(*s)) {
				x = *s - '0';
				if (x >= nov)
					continue;
				l = ovector[2*x+1] - ovector[2*x];
				VSB_bcat(vsb, subject + ovector[2*x], l);
				continue;
			}
			VSB_putc(vsb, *s);
		}
		offset = ovector[1];
		if (!all)
			break;
		i = vre_match(code, subject, PCRE2_ZERO_TERMINATED, offset,
		    PCRE2_NOTEMPTY, &data);
		if (i < VRE_ERROR_NOMATCH)
			return (i);
	} while (i != VRE_ERROR_NOMATCH);

	if (data != NULL) {
		assert(i > VRE_ERROR_NOMATCH);
		AZ(all);
		pcre2_match_data_free(data);
	}

	/* Copy suffix to match */
	VSB_cat(vsb, subject + offset);
	return (1);
}

void
VRE_free(vre_t **vv)
{
	vre_t *v = *vv;

	*vv = NULL;
	CHECK_OBJ(v, VRE_MAGIC);

	if (v->re == VRE_PACKED_RE) {
		v->re = NULL;
		AZ(v->re_ctx);
	}

	if (v->re_ctx != NULL)
		pcre2_match_context_free(v->re_ctx);
	if (v->re != NULL)
		pcre2_code_free(v->re);
	FREE_OBJ(v);
}

void
VRE_quote(struct vsb *vsb, const char *src)
{
	const char *b, *e;

	CHECK_OBJ_NOTNULL(vsb, VSB_MAGIC);
	if (src == NULL)
		return;
	for (b = src; (e = strstr(b, "\\E")) != NULL; b = e + 2)
		VSB_printf(vsb, "\\Q%.*s\\\\EE", (int)(e - b), b);
	if (*b != '\0')
		VSB_printf(vsb, "\\Q%s\\E", b);
}
