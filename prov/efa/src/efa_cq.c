/*
 * Copyright (c) 2013-2015 Intel Corporation, Inc.  All rights reserved.
 * Copyright (c) 2017-2019 Amazon.com, Inc. or its affiliates. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"
#include <ofi_mem.h>
#include "efa.h"
#include <infiniband/verbs.h>

static inline uint64_t efa_cq_opcode_to_fi_flags(enum ibv_wc_opcode	opcode) {
	switch (opcode) {
	case IBV_WC_SEND:
		return FI_SEND | FI_MSG;
	case IBV_WC_RECV:
		return FI_RECV | FI_MSG;
	default:
		assert(0);
		return 0;
	}
}

static uint64_t efa_cq_wc_to_fi_flags(struct efa_wc *wc)
{
	return efa_cq_opcode_to_fi_flags(wc->ibv_wc.opcode);
}

/**
 * @brief Unlike ibv_poll_cq, wide completion APIs do not write to ibv_wc struct. We need to do this manually.
 * This is inspired by rdma-core, i.e. `efa_process_cqe` and `efa_process_ex_cqe`.
 * @param[in]		cq	the current extended CQ
 * @param[in,out]	wc	WC struct to write result to
 */
static inline void efa_cq_wc_from_ibv_cq_ex_unsafe(struct ibv_cq_ex *cq, struct ibv_wc *wc) {
	wc->status = cq->status;
	wc->vendor_err = ibv_wc_read_vendor_err(cq);
	wc->wc_flags = ibv_wc_read_wc_flags(cq);
	wc->qp_num = ibv_wc_read_qp_num(cq);
	wc->opcode = ibv_wc_read_opcode(cq);
	wc->byte_len = ibv_wc_read_byte_len(cq);
	wc->src_qp = ibv_wc_read_src_qp(cq);
	wc->sl = ibv_wc_read_sl(cq);
	wc->slid = ibv_wc_read_slid(cq);
	wc->imm_data = ibv_wc_read_imm_data(cq);
	wc->wr_id = cq->wr_id;
}

static inline uint32_t efa_cq_api_version(struct efa_cq *cq) {
	return cq->domain->fabric->util_fabric.fabric_fid.api_version;
}

ssize_t efa_cq_readerr(struct fid_cq *cq_fid, struct fi_cq_err_entry *entry,
		       uint64_t flags)
{
	struct efa_cq *cq;
	uint32_t api_version;

	cq = container_of(cq_fid, struct efa_cq, util_cq.cq_fid);

	ofi_spin_lock(&cq->lock);

	if (!cq->ibv_cq_ex->status)
		goto err;

	api_version = efa_cq_api_version(cq);

	entry->op_context = (void *)(uintptr_t)cq->ibv_cq_ex->wr_id;
	entry->flags = efa_cq_opcode_to_fi_flags(ibv_wc_read_opcode(cq->ibv_cq_ex));
	entry->err = EIO;
	entry->prov_errno = cq->ibv_cq_ex->status;
	EFA_WARN(FI_LOG_CQ, "Work completion status: %s\n", ibv_wc_status_str(cq->ibv_cq_ex->status));

	ofi_spin_unlock(&cq->lock);

	/* We currently don't have err_data to give back to the user. */
	if (FI_VERSION_GE(api_version, FI_VERSION(1, 5)))
		entry->err_data_size = 0;

	return sizeof(*entry);
err:
	ofi_spin_unlock(&cq->lock);
	return -FI_EAGAIN;
}

static void efa_cq_read_context_entry(struct efa_wc *wc, int i, void *buf)
{
	struct fi_cq_entry *entry = buf;

	entry[i].op_context = (void *)(uintptr_t)wc->ibv_wc.wr_id;
}

static void efa_cq_read_msg_entry(struct efa_wc *wc, int i, void *buf)
{
	struct fi_cq_msg_entry *entry = buf;

	entry[i].op_context = (void *)(uintptr_t)wc->ibv_wc.wr_id;
	entry[i].flags = efa_cq_wc_to_fi_flags(wc);
	entry[i].len = (uint64_t)wc->ibv_wc.byte_len;
}

static void efa_cq_read_data_entry(struct efa_wc *wc, int i, void *buf)
{
	struct fi_cq_data_entry *entry = buf;

	entry[i].op_context = (void *)(uintptr_t)wc->ibv_wc.wr_id;
	entry[i].flags = efa_cq_wc_to_fi_flags(wc);
	entry[i].data = 0;
	entry[i].len = (uint64_t)wc->ibv_wc.byte_len;
}

/**
 * @brief Convert an error code from CQ poll API, e.g. `ibv_start_poll`, `ibv_end_poll`.
 * The returned error code must be 0 (success) or negative (error).
 * As a special case, if input error code is ENOENT (there was no item on CQ), we should return -FI_EAGAIN.
 * @param[in] err	Return value from `ibv_start_poll` or `ibv_end_poll`
 * @returns	Converted error code
 */
static inline ssize_t efa_cq_ibv_poll_error_to_fi_error(ssize_t err) {
	if (err == ENOENT) {
		return -FI_EAGAIN;
	}

	if (err > 0) {
		return -err;
	}

	return err;
}

ssize_t efa_cq_readfrom(struct fid_cq *cq_fid, void *buf, size_t count,
			fi_addr_t *src_addr)
{
	bool should_end_poll = false;
	struct efa_cq *cq;
	struct efa_av *av;
	struct efa_wc wc = {0};
	ssize_t err = 0;
	size_t num_cqe = 0; /* Count of read entries */

	/* Initialize an empty ibv_poll_cq_attr struct for ibv_start_poll.
	 * EFA expects .comp_mask = 0, or otherwise returns EINVAL.
	 */
	struct ibv_poll_cq_attr poll_cq_attr = {.comp_mask = 0};

	cq = container_of(cq_fid, struct efa_cq, util_cq.cq_fid);

	ofi_spin_lock(&cq->lock);

	/* Call ibv_start_poll only once regardless of count == 0 */
	err = ibv_start_poll(cq->ibv_cq_ex, &poll_cq_attr);
	should_end_poll = !err;

	while (!err && num_cqe < count) {
		efa_cq_wc_from_ibv_cq_ex_unsafe(cq->ibv_cq_ex, &wc.ibv_wc);

		if (cq->ibv_cq_ex->status) {
			err = -FI_EAVAIL;
			break;
		}

		if (src_addr) {
			av = cq->domain->qp_table[wc.ibv_wc.qp_num &
				cq->domain->qp_table_sz_m1]->ep->av;

			src_addr[num_cqe] = efa_av_reverse_lookup_dgram(av,
				wc.ibv_wc.slid,
				wc.ibv_wc.src_qp);
		}

		cq->read_entry(&wc, num_cqe, buf);
		num_cqe++;

		err = ibv_next_poll(cq->ibv_cq_ex);
	}

	err = efa_cq_ibv_poll_error_to_fi_error(err);

	if (should_end_poll)
		ibv_end_poll(cq->ibv_cq_ex);

	ofi_spin_unlock(&cq->lock);

	return num_cqe ? num_cqe : err;
}

static const char *efa_cq_strerror(struct fid_cq *cq_fid,
				   int prov_errno,
				   const void *err_data,
				   char *buf, size_t len)
{
	/* XXX use vendor_error */
	return "unknown error";
}

static struct fi_ops_cq efa_cq_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = ofi_cq_read,
	.readfrom = ofi_cq_readfrom,
	.readerr = ofi_cq_readerr,
	.sread = fi_no_cq_sread,
	.sreadfrom = fi_no_cq_sreadfrom,
	.signal = fi_no_cq_signal,
	.strerror = efa_cq_strerror
};

static int efa_cq_control(fid_t fid, int command, void *arg)
{
	int ret = 0;

	switch (command) {
	default:
		ret = -FI_ENOSYS;
		break;
	}

	return ret;
}

static int efa_cq_close(fid_t fid)
{
	struct efa_cq *cq;
	int ret;

	cq = container_of(fid, struct efa_cq, util_cq.cq_fid.fid);

	ofi_bufpool_destroy(cq->wce_pool);

	ofi_spin_destroy(&cq->lock);

	ret = -ibv_destroy_cq(ibv_cq_ex_to_cq(cq->ibv_cq_ex));
	if (ret)
		return ret;

	ret = ofi_cq_cleanup(&cq->util_cq);
	if (ret)
		return ret;

	free(cq);

	return 0;
}

static struct fi_ops efa_cq_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = efa_cq_close,
	.bind = fi_no_bind,
	.control = efa_cq_control,
	.ops_open = fi_no_ops_open,
};

int efa_cq_open(struct fid_domain *domain_fid, struct fi_cq_attr *attr,
		struct fid_cq **cq_fid, void *context)
{
	struct efa_cq *cq;
	int ret;
	struct ibv_cq_init_attr_ex init_attr_ex = {
		.cqe = 0,
		.cq_context = NULL,
		.channel = NULL,
		.comp_vector = 0,
		/* EFA requires these values for wc_flags and comp_mask.
		 * See `efa_create_cq_ex` in rdma-core.
		 */
		.wc_flags = IBV_WC_STANDARD_FLAGS,
		.comp_mask = 0,
	};

	if (attr->wait_obj != FI_WAIT_NONE)
		return -FI_ENOSYS;

	cq = calloc(1, sizeof(*cq));
	if (!cq)
		return -FI_ENOMEM;

	ret = ofi_cq_init(&efa_prov, domain_fid, attr, &cq->util_cq,
			  &ofi_cq_progress, context);
	if (ret) {
		EFA_WARN(FI_LOG_CQ, "Unable to create UTIL_CQ\n");
		goto err_free_cq;
	}

	cq->domain = container_of(domain_fid, struct efa_domain,
				  util_domain.domain_fid);

	init_attr_ex.cqe = attr->size ? attr->size : EFA_DEF_CQ_SIZE;

	cq->ibv_cq_ex = ibv_create_cq_ex(cq->domain->device->ibv_ctx, &init_attr_ex);
	if (!cq->ibv_cq_ex) {
		EFA_WARN(FI_LOG_CQ, "Unable to create extended CQ\n");
		ret = -FI_EINVAL;
		goto err_free_util_cq;
	}

	ret = ofi_bufpool_create(&cq->wce_pool, sizeof(struct efa_wce), 16, 0,
				 EFA_WCE_CNT, 0);
	if (ret) {
		EFA_WARN(FI_LOG_CQ, "Failed to create wce_pool\n");
		goto err_destroy_cq;
	}

	switch (attr->format) {
	case FI_CQ_FORMAT_UNSPEC:
	case FI_CQ_FORMAT_CONTEXT:
		cq->read_entry = efa_cq_read_context_entry;
		cq->entry_size = sizeof(struct fi_cq_entry);
		break;
	case FI_CQ_FORMAT_MSG:
		cq->read_entry = efa_cq_read_msg_entry;
		cq->entry_size = sizeof(struct fi_cq_msg_entry);
		break;
	case FI_CQ_FORMAT_DATA:
		cq->read_entry = efa_cq_read_data_entry;
		cq->entry_size = sizeof(struct fi_cq_data_entry);
		break;
	case FI_CQ_FORMAT_TAGGED:
	default:
		ret = -FI_ENOSYS;
		goto err_destroy_pool;
	}

	ofi_spin_init(&cq->lock);

	*cq_fid = &cq->util_cq.cq_fid;
	(*cq_fid)->fid.fclass = FI_CLASS_CQ;
	(*cq_fid)->fid.context = context;
	(*cq_fid)->fid.ops = &efa_cq_fi_ops;
	(*cq_fid)->ops = &efa_cq_ops;

	return 0;

err_destroy_pool:
	ofi_bufpool_destroy(cq->wce_pool);
err_destroy_cq:
	ibv_destroy_cq(ibv_cq_ex_to_cq(cq->ibv_cq_ex));
err_free_util_cq:
	ofi_cq_cleanup(&cq->util_cq);
err_free_cq:
	free(cq);
	return ret;
}
