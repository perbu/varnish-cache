/*-
 * Copyright (c) 2016 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
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
 *
 */

#include "config.h"

#include "cache/cache.h"

#include <stdio.h>
#include <stdlib.h>

#include "cache/cache_transport.h"
#include "cache/cache_filter.h"
#include "http2/cache_http2.h"

#include "vend.h"
#include "vtcp.h"
#include "vtim.h"

#define H2EC1(U,v,d) const struct h2_error_s H2CE_##U[1] = {{#U,d,v,0,1}};
#define H2EC2(U,v,d) const struct h2_error_s H2SE_##U[1] = {{#U,d,v,1,0}};
#define H2EC3(U,v,d) H2EC1(U,v,d) H2EC2(U,v,d)
#define H2_ERROR(NAME, val, sc, desc) H2EC##sc(NAME, val, desc)
#include "tbl/h2_error.h"
#undef H2EC1
#undef H2EC2
#undef H2EC3

static const struct h2_error_s H2NN_ERROR[1] = {{
	"UNKNOWN_ERROR",
	"Unknown error number",
	0xffffffff,
	1,
	1
}};

enum h2frame {
#define H2_FRAME(l,u,t,f,...)	H2F_##u = t,
#include "tbl/h2_frames.h"
};

static const char *
h2_framename(enum h2frame h2f)
{

	switch(h2f) {
#define H2_FRAME(l,u,t,f,...)	case H2F_##u: return #u;
#include "tbl/h2_frames.h"
	default:
		return (NULL);
	}
}

#define H2_FRAME_FLAGS(l,u,v)	const uint8_t H2FF_##u = v;
#include "tbl/h2_frames.h"

/**********************************************************************
 */

static const h2_error stream_errors[] = {
#define H2EC1(U,v,d)
#define H2EC2(U,v,d) [v] = H2SE_##U,
#define H2EC3(U,v,d) H2EC1(U,v,d) H2EC2(U,v,d)
#define H2_ERROR(NAME, val, sc, desc) H2EC##sc(NAME, val, desc)
#include "tbl/h2_error.h"
#undef H2EC1
#undef H2EC2
#undef H2EC3
};

#define NSTREAMERRORS (sizeof(stream_errors)/sizeof(stream_errors[0]))

static h2_error
h2_streamerror(uint32_t u)
{
	if (u < NSTREAMERRORS && stream_errors[u] != NULL)
		return (stream_errors[u]);
	else
		return (H2NN_ERROR);
}

/**********************************************************************
 */

static const h2_error conn_errors[] = {
#define H2EC1(U,v,d) [v] = H2CE_##U,
#define H2EC2(U,v,d)
#define H2EC3(U,v,d) H2EC1(U,v,d) H2EC2(U,v,d)
#define H2_ERROR(NAME, val, sc, desc) H2EC##sc(NAME, val, desc)
#include "tbl/h2_error.h"
#undef H2EC1
#undef H2EC2
#undef H2EC3
};

#define NCONNERRORS (sizeof(conn_errors)/sizeof(conn_errors[0]))

static h2_error
h2_connectionerror(uint32_t u)
{
	if (u < NCONNERRORS && conn_errors[u] != NULL)
		return (conn_errors[u]);
	else
		return (H2NN_ERROR);
}

/**********************************************************************
 */

struct h2_req *
h2_new_req(const struct worker *wrk, struct h2_sess *h2,
    unsigned stream, struct req *req)
{
	struct h2_req *r2;

	if (req == NULL)
		req = Req_New(wrk, h2->sess);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	r2 = WS_Alloc(req->ws, sizeof *r2);
	AN(r2);
	INIT_OBJ(r2, H2_REQ_MAGIC);
	r2->state = H2_S_IDLE;
	r2->h2sess = h2;
	r2->stream = stream;
	r2->req = req;
	r2->r_window = h2->local_settings.initial_window_size;
	r2->t_window = h2->remote_settings.initial_window_size;
	req->transport_priv = r2;
	Lck_Lock(&h2->sess->mtx);
	VTAILQ_INSERT_TAIL(&h2->streams, r2, list);
	Lck_Unlock(&h2->sess->mtx);
	h2->refcnt++;
	return (r2);
}

void
h2_del_req(struct worker *wrk, struct h2_req *r2)
{
	struct h2_sess *h2;
	struct sess *sp;
	struct req *req;
	int r;

	CHECK_OBJ_NOTNULL(r2, H2_REQ_MAGIC);
	h2 = r2->h2sess;
	CHECK_OBJ_NOTNULL(h2, H2_SESS_MAGIC);
	sp = h2->sess;
	Lck_Lock(&sp->mtx);
	assert(h2->refcnt > 0);
	r = --h2->refcnt;
	/* XXX: PRIORITY reshuffle */
	VTAILQ_REMOVE(&h2->streams, r2, list);
	Lck_Unlock(&sp->mtx);
	Req_Cleanup(sp, wrk, r2->req);
	Req_Release(r2->req);
	if (r)
		return;

	/* All streams gone, including stream #0, clean up */
	req = h2->srq;
	Req_Cleanup(sp, wrk, req);
	Req_Release(req);
	SES_Delete(sp, SC_RX_JUNK, NAN);
}

/**********************************************************************/

static void
h2_vsl_frame(const struct h2_sess *h2, const void *ptr, size_t len)
{
	const uint8_t *b;
	struct vsb *vsb;
	const char *p;
	unsigned u;

	AN(ptr);
	assert(len >= 9);
	b = ptr;

	VSLb_bin(h2->vsl, SLT_H2RxHdr, 9, b);
	if (len > 9)
		VSLb_bin(h2->vsl, SLT_H2RxBody, len - 9, b + 9);

	vsb = VSB_new_auto();
	AN(vsb);
	p = h2_framename((enum h2frame)b[3]);
	if (p != NULL)
		VSB_cat(vsb, p);
	else
		VSB_quote(vsb, b + 3, 1, VSB_QUOTE_HEX);

	u = vbe32dec(b) >> 8;
	VSB_printf(vsb, "[%u] ", u);
	VSB_quote(vsb, b + 4, 1, VSB_QUOTE_HEX);
	VSB_putc(vsb, ' ');
	VSB_quote(vsb, b + 5, 4, VSB_QUOTE_HEX);
	if (u > 0) {
		VSB_putc(vsb, ' ');
		VSB_quote(vsb, b + 9, len - 9, VSB_QUOTE_HEX);
	}
	AZ(VSB_finish(vsb));
	VSLb(h2->vsl, SLT_Debug, "H2RXF %s", VSB_data(vsb));
	VSB_destroy(&vsb);
}


/**********************************************************************
 */

static h2_error __match_proto__(h2_frame_f)
h2_rx_ping(struct worker *wrk, struct h2_sess *h2, struct h2_req *r2)
{

	(void)r2;
	if (h2->rxf_len != 8)				// rfc7540,l,2364,2366
		return (H2CE_FRAME_SIZE_ERROR);
	if (h2->rxf_stream != 0)			// rfc7540,l,2359,2362
		return (H2CE_PROTOCOL_ERROR);
	if (h2->rxf_flags != 0)				// We never send pings
		return (H2SE_PROTOCOL_ERROR);
	H2_Send_Get(wrk, h2, r2);
	H2_Send_Frame(wrk, h2,
	    H2_F_PING, H2FF_PING_ACK, 8, 0, h2->rxf_data);
	H2_Send_Rel(h2, r2);
	return (0);
}

/**********************************************************************
 */

static h2_error __match_proto__(h2_frame_f)
h2_rx_push_promise(struct worker *wrk, struct h2_sess *h2, struct h2_req *r2)
{
	// rfc7540,l,2262,2267
	(void)wrk;
	(void)h2;
	(void)r2;
	return (H2CE_PROTOCOL_ERROR);
}

/**********************************************************************
 */

static h2_error __match_proto__(h2_frame_f)
h2_rx_rst_stream(struct worker *wrk, struct h2_sess *h2, struct h2_req *r2)
{
	(void)wrk;

	if (h2->rxf_len != 4)			// rfc7540,l,2003,2004
		return (H2CE_FRAME_SIZE_ERROR);
	if (r2 == NULL)
		return (0);
	Lck_Lock(&h2->sess->mtx);
	r2->error = h2_streamerror(vbe32dec(h2->rxf_data));
	if (r2->wrk != NULL)
		AZ(pthread_cond_signal(&r2->wrk->cond));
	Lck_Unlock(&h2->sess->mtx);
	return (0);
}

/**********************************************************************
 */

static h2_error __match_proto__(h2_frame_f)
h2_rx_goaway(struct worker *wrk, struct h2_sess *h2, struct h2_req *r2)
{

	(void)wrk;
	(void)r2;
	h2->goaway_last_stream = vbe32dec(h2->rxf_data);
	h2->error = h2_connectionerror(vbe32dec(h2->rxf_data + 4));
	VSLb(h2->vsl, SLT_Debug, "GOAWAY %s", h2->error->name);
	return (h2->error);
}

/**********************************************************************
 */

static h2_error __match_proto__(h2_frame_f)
h2_rx_window_update(struct worker *wrk, struct h2_sess *h2, struct h2_req *r2)
{
	uint32_t wu;

	(void)wrk;
	if (h2->rxf_len != 4)
		return (H2CE_FRAME_SIZE_ERROR);
	wu = vbe32dec(h2->rxf_data) & ~(1LU<<31);
	if (wu == 0)
		return (H2SE_PROTOCOL_ERROR);
	if (r2 == NULL)
		return (0);
	Lck_Lock(&h2->sess->mtx);
	r2->t_window += wu;
	Lck_Unlock(&h2->sess->mtx);
	if (r2->t_window >= (1LLU << 31))
		return (H2SE_FLOW_CONTROL_ERROR);
	return (0);
}

/**********************************************************************
 * Incoming PRIORITY, possibly an ACK of one we sent.
 */

static h2_error __match_proto__(h2_frame_f)
h2_rx_priority(struct worker *wrk, struct h2_sess *h2, struct h2_req *r2)
{

	(void)wrk;
	(void)h2;
	xxxassert(r2->stream & 1);
	return (0);
}

/**********************************************************************
 * Incoming SETTINGS, possibly an ACK of one we sent.
 */

#define H2_SETTING(U,l, ...)					\
static void __match_proto__(h2_setsetting_f)			\
h2_setting_##l(struct h2_settings* s, uint32_t v)		\
{								\
	s -> l = v;						\
}
#include <tbl/h2_settings.h>

#define H2_SETTING(U, l, ...)					\
const struct h2_setting_s H2_SET_##U[1] = {{			\
	#l,							\
	h2_setting_##l,						\
	__VA_ARGS__						\
}};
#include <tbl/h2_settings.h>

static const struct h2_setting_s * const h2_setting_tbl[] = {
#define H2_SETTING(U,l,v, ...) [v] = H2_SET_##U,
#include <tbl/h2_settings.h>
};

#define H2_SETTING_TBL_LEN (sizeof(h2_setting_tbl)/sizeof(h2_setting_tbl[0]))

h2_error
h2_set_setting(struct h2_sess *h2, const uint8_t *d)
{
	const struct h2_setting_s *s;
	uint16_t x;
	uint32_t y;

	x = vbe16dec(d);
	y = vbe32dec(d + 2);
	if (x >= H2_SETTING_TBL_LEN || h2_setting_tbl[x] == NULL) {
		// rfc7540,l,2181,2182
		VSLb(h2->vsl, SLT_Debug,
		    "H2SETTING unknown setting 0x%04x=%08x (ignored)", x, y);
		return (0);
	}
	s = h2_setting_tbl[x];
	AN(s);
	if (y < s->minval || y > s->maxval) {
		VSLb(h2->vsl, SLT_Debug, "H2SETTING invalid %s=0x%08x",
		    s->name, y);
		AN(s->range_error);
		if (!DO_DEBUG(DBG_H2_NOCHECK))
			return (s->range_error);
	}
	VSLb(h2->vsl, SLT_Debug, "H2SETTING %s=0x%08x", s->name, y);
	AN(s->setfunc);
	s->setfunc(&h2->remote_settings, y);
	return (0);
}

static h2_error __match_proto__(h2_frame_f)
h2_rx_settings(struct worker *wrk, struct h2_sess *h2, struct h2_req *r2)
{
	const uint8_t *p;
	unsigned l;
	h2_error retval = 0;

	AN(wrk);
	AN(r2);
	AZ(h2->rxf_stream);
	if (h2->rxf_flags == H2FF_SETTINGS_ACK) {
		if (h2->rxf_len > 0)			// rfc7540,l,2047,2049
			return (H2CE_FRAME_SIZE_ERROR);
		return (0);
	} else {
		if (h2->rxf_len % 6)			// rfc7540,l,2062,2064
			return (H2CE_PROTOCOL_ERROR);
		p = h2->rxf_data;
		for (l = h2->rxf_len; l >= 6; l -= 6, p += 6) {
			retval = h2_set_setting(h2, p);
			if (retval)
				return (retval);
		}
		H2_Send_Get(wrk, h2, r2);
		H2_Send_Frame(wrk, h2,
		    H2_F_SETTINGS, H2FF_SETTINGS_ACK, 0, 0, NULL);
		H2_Send_Rel(h2, r2);
	}
	return (0);
}

/**********************************************************************
 * Incoming HEADERS, this is where the partys at...
 */

void __match_proto__(task_func_t)
h2_do_req(struct worker *wrk, void *priv)
{
	struct req *req;
	struct h2_req *r2;

	CAST_OBJ_NOTNULL(req, priv, REQ_MAGIC);
	CAST_OBJ_NOTNULL(r2, req->transport_priv, H2_REQ_MAGIC);
	THR_SetRequest(req);
	req->http->conds = 1;
	if (CNT_Request(wrk, req) != REQ_FSM_DISEMBARK) {
		VSL(SLT_Debug, 0, "H2REQ CNT done");
		r2->state = H2_S_CLOSED;
		h2_del_req(wrk, r2);
	}
	THR_SetRequest(NULL);
}

static h2_error
h2_end_headers(const struct worker *wrk, const struct h2_sess *h2,
    struct req *req, struct h2_req *r2)
{
	h2_error h2e;

	assert(r2->state == H2_S_OPEN);
	r2->state = H2_S_CLOS_REM;
	h2e = h2h_decode_fini(h2, r2->decode);
	FREE_OBJ(r2->decode);
	if (h2e != NULL) {
		VSL(SLT_Debug, 0, "H2H_DECODE_FINI %s", h2e->name);
		return (h2e);
	}
	VSLb_ts_req(req, "Req", req->t_req);

	if (h2->rxf_flags & H2FF_HEADERS_END_STREAM)
		req->req_body_status = REQ_BODY_NONE;
	else
		req->req_body_status = REQ_BODY_WITHOUT_LEN;

	req->req_step = R_STP_TRANSPORT;
	req->task.func = h2_do_req;
	req->task.priv = req;
	XXXAZ(Pool_Task(wrk->pool, &req->task, TASK_QUEUE_REQ));
	return (0);
}

static h2_error __match_proto__(h2_frame_f)
h2_rx_headers(struct worker *wrk, struct h2_sess *h2, struct h2_req *r2)
{
	struct req *req;
	h2_error h2e;
	const uint8_t *p;
	size_t l;

	if (r2->state != H2_S_IDLE)
		return (H2CE_PROTOCOL_ERROR);	// XXX spec ?
	r2->state = H2_S_OPEN;

	req = r2->req;
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	req->vsl->wid = VXID_Get(wrk, VSL_CLIENTMARKER);
	VSLb(req->vsl, SLT_Begin, "req %u rxreq", VXID(req->sp->vxid));
	VSL(SLT_Link, req->sp->vxid, "req %u rxreq", VXID(req->vsl->wid));

	h2->new_req = req;
	req->sp = h2->sess;
	req->transport = &H2_transport;

	req->t_first = VTIM_real();
	req->t_req = VTIM_real();
	req->t_prev = req->t_first;
	VSLb_ts_req(req, "Start", req->t_first);
	VCL_Refresh(&wrk->vcl);
	req->vcl = wrk->vcl;
	wrk->vcl = NULL;

	HTTP_Setup(req->http, req->ws, req->vsl, SLT_ReqMethod);
	http_SetH(req->http, HTTP_HDR_PROTO, "HTTP/2.0");

	ALLOC_OBJ(r2->decode, H2H_DECODE_MAGIC);
	AN(r2->decode);
	h2h_decode_init(h2, r2->decode);

	/* XXX: Error handling */
	p = h2->rxf_data;
	l = h2->rxf_len;
	if (h2->rxf_flags & H2FF_HEADERS_PADDED) {
		l -= 1 + *p;
		p += 1;
	}
	if (h2->rxf_flags & H2FF_HEADERS_PRIORITY) {
		p += 5;
		l -= 5;
	}
	h2e = h2h_decode_bytes(h2, r2->decode, p, l);
	if (h2e != NULL) {
		VSL(SLT_Debug, 0, "H2H_DECODE_BYTES %s", h2e->name);
		return (h2e);
	}
	if (h2->rxf_flags & H2FF_HEADERS_END_HEADERS)
		return (h2_end_headers(wrk, h2, req, r2));
	return (0);
}

/**********************************************************************
 * XXX: Check hard sequence req. for Cont.
 */

static h2_error __match_proto__(h2_frame_f)
h2_rx_continuation(struct worker *wrk, struct h2_sess *h2, struct h2_req *r2)
{
	struct req *req;
	h2_error h2e;

	if (r2->state != H2_S_OPEN)
		return (H2CE_PROTOCOL_ERROR);	// XXX spec ?
	req = r2->req;
	h2e = h2h_decode_bytes(h2, r2->decode, h2->rxf_data, h2->rxf_len);
	if (h2e != NULL) {
		VSL(SLT_Debug, 0, "H2H_DECODE_BYTES %s", h2e->name);
		return (h2e);
	}
	if (h2->rxf_flags & H2FF_HEADERS_END_HEADERS)
		return (h2_end_headers(wrk, h2, req, r2));
	return (0);
}

/**********************************************************************/

static h2_error __match_proto__(h2_frame_f)
h2_rx_data(struct worker *wrk, struct h2_sess *h2, struct h2_req *r2)
{
	int w1 = 0, w2 = 0;
	char buf[4];
	unsigned wi;

	(void)wrk;
	AZ(h2->mailcall);
	h2->mailcall = r2;
	Lck_Lock(&h2->sess->mtx);
	h2->r_window -= h2->rxf_len;
	r2->r_window -= h2->rxf_len;
	AZ(pthread_cond_broadcast(h2->cond));
	while (h2->mailcall != NULL && h2->error == 0 && r2->error == 0)
		AZ(Lck_CondWait(h2->cond, &h2->sess->mtx, 0));
	wi = cache_param->h2_rx_window_increment;
	if (h2->r_window < cache_param->h2_rx_window_low_water) {
		h2->r_window += wi;
		w1 = 1;
	}
	if (r2->r_window < cache_param->h2_rx_window_low_water) {
		r2->r_window += wi;
		w2 = 1;
	}
	Lck_Unlock(&h2->sess->mtx);
	if (w1 || w2) {
		vbe32enc(buf, wi);
		H2_Send_Get(wrk, h2, h2->req0);
		if (w1)
			H2_Send_Frame(wrk, h2, H2_F_WINDOW_UPDATE, 0,
			    4, 0, buf);
		if (w2)
			H2_Send_Frame(wrk, h2, H2_F_WINDOW_UPDATE, 0,
			    4, r2->stream, buf);
		H2_Send_Rel(h2, h2->req0);
	}
	return (0);
}

static enum vfp_status __match_proto__(vfp_pull_f)
h2_vfp_body(struct vfp_ctx *vc, struct vfp_entry *vfe, void *ptr, ssize_t *lp)
{
	struct h2_req *r2;
	struct h2_sess *h2;
	unsigned l;
	enum vfp_status retval = VFP_OK;

	CHECK_OBJ_NOTNULL(vc, VFP_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(vfe, VFP_ENTRY_MAGIC);
	CAST_OBJ_NOTNULL(r2, vfe->priv1, H2_REQ_MAGIC);
	h2 = r2->h2sess;

	AN(ptr);
	AN(lp);
	l = *lp;
	*lp = 0;

	Lck_Lock(&h2->sess->mtx);
	while (h2->mailcall != r2 && h2->error == 0 && r2->error == 0)
		AZ(Lck_CondWait(h2->cond, &h2->sess->mtx, 0));
	if (h2->mailcall == r2) {
		assert(h2->mailcall == r2);
		if (l > h2->rxf_len)
			l = h2->rxf_len;
		if (l > 0) {
			memcpy(ptr, h2->rxf_data, l);
			h2->rxf_data += l;
			h2->rxf_len -= l;
		}
		*lp = l;
		if (h2->rxf_len == 0) {
			if (h2->rxf_flags & H2FF_DATA_END_STREAM)
				retval = VFP_END;
		}
		h2->mailcall = NULL;
		AZ(pthread_cond_broadcast(h2->cond));
	} else {
		retval = VFP_ERROR;
	}
	Lck_Unlock(&h2->sess->mtx);
	return (retval);
}

static const struct vfp h2_body = {
	.name = "H2_BODY",
	.pull = h2_vfp_body,
};

void __match_proto__(vtr_req_body_t)
h2_req_body(struct req *req)
{
	struct h2_req *r2;
	struct vfp_entry *vfe;

	CHECK_OBJ(req, REQ_MAGIC);
	CAST_OBJ_NOTNULL(r2, req->transport_priv, H2_REQ_MAGIC);
	vfe = VFP_Push(req->vfc, &h2_body, 0);
	AN(vfe);
	vfe->priv1 = r2;
}

/**********************************************************************/

void __match_proto__(vtr_req_fail_f)
h2_req_fail(struct req *req, enum sess_close reason)
{
	assert(reason > 0);
	assert(req->sp->fd != 0);
}

/**********************************************************************/

static enum htc_status_e __match_proto__(htc_complete_f)
h2_frame_complete(struct http_conn *htc)
{
	int l;
	unsigned u;

	CHECK_OBJ_NOTNULL(htc, HTTP_CONN_MAGIC);
	l = htc->rxbuf_e - htc->rxbuf_b;
	if (l < 9)
		return (HTC_S_MORE);
	u = vbe32dec(htc->rxbuf_b) >> 8;
	VSL(SLT_Debug, 0, "RX %p %d %u", htc->rxbuf_b, l, u);
	if (l < u + 9)	// XXX: Only for !DATA frames
		return (HTC_S_MORE);
	return (HTC_S_COMPLETE);
}

/**********************************************************************/

static h2_error
h2_procframe(struct worker *wrk, struct h2_sess *h2,
    h2_frame h2f)
{
	struct h2_req *r2 = NULL;
	h2_error h2e;
	char b[4];

	if (h2->rxf_stream == 0 && h2f->act_szero != 0)
		return (h2f->act_szero);

	if (h2->rxf_stream != 0 && h2f->act_snonzero != 0)
		return (h2f->act_snonzero);

	if (h2->rxf_stream > h2->highest_stream && h2f->act_sidle != 0)
		return (h2f->act_sidle);

	if (h2->rxf_stream != 0 && !(h2->rxf_stream & 1)) {
		// rfc7540,l,1140,1145
		// rfc7540,l,1153,1158
		/* No even streams, we don't do PUSH_PROMISE */
		VSLb(h2->vsl, SLT_Debug, "H2: illegal stream (=%u)",
		    h2->rxf_stream);
		return (H2CE_PROTOCOL_ERROR);
	}

	VTAILQ_FOREACH(r2, &h2->streams, list)
		if (r2->stream == h2->rxf_stream)
			break;

	if (r2 == NULL && h2f->act_sidle == 0) {
		if (h2->rxf_stream <= h2->highest_stream)
			return (H2CE_PROTOCOL_ERROR);	// rfc7540,l,1153,1158
		h2->highest_stream = h2->rxf_stream;
		r2 = h2_new_req(wrk, h2, h2->rxf_stream, NULL);
		AN(r2);
	}

	h2e = h2f->rxfunc(wrk, h2, r2);
	if (h2e == 0)
		return (0);
	if (h2->rxf_stream == 0 || h2e->connection)
		return (h2e);	// Connection errors one level up

	VSLb(h2->vsl, SLT_Debug, "H2: stream %u: %s", h2->rxf_stream, h2e->txt);
	vbe32enc(b, h2e->val);

	H2_Send_Get(wrk, h2, r2);
	(void)H2_Send_Frame(wrk, h2, H2_F_RST_STREAM,
	    0, sizeof b, h2->rxf_stream, b);
	H2_Send_Rel(h2, r2);

	h2_del_req(wrk, r2);
	return (0);
}

/***********************************************************************
 * Called in loop from h2_new_session()
 */

#define H2_FRAME(l,U,...) const struct h2_frame_s H2_F_##U[1] = \
    {{ #U, h2_rx_##l, __VA_ARGS__ }};
#include "tbl/h2_frames.h"

static const h2_frame h2flist[] = {
#define H2_FRAME(l,U,t,...) [t] = H2_F_##U,
#include "tbl/h2_frames.h"
};

#define H2FMAX (sizeof(h2flist) / sizeof(h2flist[0]))

int
h2_rxframe(struct worker *wrk, struct h2_sess *h2)
{
	enum htc_status_e hs;
	h2_frame h2f;
	h2_error h2e;
	char b[8];

	(void)VTCP_blocking(*h2->htc->rfd);
	h2->sess->t_idle = VTIM_real();
	hs = HTC_RxStuff(h2->htc, h2_frame_complete,
	    NULL, NULL, NAN,
	    h2->sess->t_idle + cache_param->timeout_idle + 100,
	    1024);
	if (hs != HTC_S_COMPLETE) {
		Lck_Lock(&h2->sess->mtx);
		VSLb(h2->vsl, SLT_Debug, "H2: No frame (hs=%d)", hs);
		h2->error = H2CE_NO_ERROR;
		Lck_Unlock(&h2->sess->mtx);
		return (0);
	}

	h2->rxf_len =  vbe32dec(h2->htc->rxbuf_b) >> 8;
	h2->rxf_type =  h2->htc->rxbuf_b[3];
	h2->rxf_flags = h2->htc->rxbuf_b[4];
	h2->rxf_stream = vbe32dec(h2->htc->rxbuf_b + 5);
	h2->rxf_stream &= ~(1LU<<31);			// rfc7540,l,690,692
	h2->rxf_data = (void*)(h2->htc->rxbuf_b + 9);
	/* XXX: later full DATA will not be rx'ed yet. */
	HTC_RxPipeline(h2->htc, h2->htc->rxbuf_b + h2->rxf_len + 9);

	Lck_Lock(&h2->sess->mtx);
	h2_vsl_frame(h2, h2->htc->rxbuf_b, 9L + h2->rxf_len);
	Lck_Unlock(&h2->sess->mtx);

	if (h2->rxf_type >= H2FMAX) {
		// rfc7540,l,679,681
		// XXX: later, drain rest of frame
		h2->bogosity++;
		VSLb(h2->vsl, SLT_Debug,
		    "H2: Unknown frame type 0x%02x (ignored)",
		    (uint8_t)h2->rxf_type);
		return (1);
	}
	h2f = h2flist[h2->rxf_type];
#if 1
	AN(h2f->name);
	AN(h2f->rxfunc);
#else
	/* If we ever get holes in the frame table... */
	if (h2f->name == NULL || h2f->func == NULL) {
		// rfc7540,l,679,681
		// XXX: later, drain rest of frame
		h2->bogosity++;
		VSLb(h2->vsl, SLT_Debug,
		    "H2: Unimplemented frame type 0x%02x (ignored)",
		    h2->rxf_type);
		return (0);
	}
#endif
	if (h2->rxf_flags & ~h2f->flags) {
		// rfc7540,l,687,688
		h2->bogosity++;
		VSLb(h2->vsl, SLT_Debug,
		    "H2: Unknown flags 0x%02x on %s (ignored)",
		    (uint8_t)h2->rxf_flags, h2f->name);
		h2->rxf_flags &= h2f->flags;
	}

	h2e = h2_procframe(wrk, h2, h2f);
	if (h2->error == 0 && h2e) {
		h2->error = h2e;
		vbe32enc(b, h2->highest_stream);
		vbe32enc(b + 4, h2e->val);
		H2_Send_Get(wrk, h2, h2->req0);
		(void)H2_Send_Frame(wrk, h2, H2_F_GOAWAY, 0, 8, 0, b);
		H2_Send_Rel(h2, h2->req0);
	}
	return (h2e ? 0 : 1);
}
