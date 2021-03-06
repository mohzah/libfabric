/*
 * Copyright (c) 2013-2016 Intel Corporation. All rights reserved.
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

#include <stdlib.h>
#include <string.h>

#include <fi_enosys.h>
#include <fi_util.h>

#define UTIL_DEF_CQ_SIZE (1024)

int ofi_cq_write_error(struct util_cq *cq,
		       const struct fi_cq_err_entry *err_entry)
{
	struct util_cq_err_entry *entry;
	struct fi_cq_tagged_entry *comp;

	if (!(entry = calloc(1, sizeof(*entry))))
		return -FI_ENOMEM;

	entry->err_entry = *err_entry;
	fastlock_acquire(&cq->cq_lock);
	slist_insert_tail(&entry->list_entry, &cq->err_list);
	comp = ofi_cirque_tail(cq->cirq);
	comp->flags = UTIL_FLAG_ERROR;
	ofi_cirque_commit(cq->cirq);
	fastlock_release(&cq->cq_lock);
	if (cq->wait)
		cq->wait->signal(cq->wait);
	return 0;
}

int ofi_check_cq_attr(const struct fi_provider *prov,
		      const struct fi_cq_attr *attr)
{
	switch (attr->format) {
	case FI_CQ_FORMAT_UNSPEC:
	case FI_CQ_FORMAT_CONTEXT:
	case FI_CQ_FORMAT_MSG:
	case FI_CQ_FORMAT_DATA:
	case FI_CQ_FORMAT_TAGGED:
		break;
	default:
		FI_WARN(prov, FI_LOG_CQ, "unsupported format\n");
		return -FI_EINVAL;
	}

	switch (attr->wait_obj) {
	case FI_WAIT_NONE:
		break;
	case FI_WAIT_SET:
		if (!attr->wait_set) {
			FI_WARN(prov, FI_LOG_CQ, "invalid wait set\n");
			return -FI_EINVAL;
		}
		/* fall through */
	case FI_WAIT_UNSPEC:
	case FI_WAIT_FD:
		switch (attr->wait_cond) {
		case FI_CQ_COND_NONE:
		case FI_CQ_COND_THRESHOLD:
			break;
		default:
			FI_WARN(prov, FI_LOG_CQ, "unsupported wait cond\n");
			return -FI_EINVAL;
		}
		break;
	default:
		FI_WARN(prov, FI_LOG_CQ, "unsupported wait object\n");
		return -FI_EINVAL;
	}

	if (attr->flags & ~(FI_AFFINITY)) {
		FI_WARN(prov, FI_LOG_CQ, "invalid flags\n");
		return -FI_EINVAL;
	}

	if (attr->flags & FI_AFFINITY) {
		FI_WARN(prov, FI_LOG_CQ, "signaling vector ignored\n");
	}

	return 0;
}

static void util_cq_read_ctx(void **dst, void *src)
{
	*(struct fi_cq_entry *) *dst = *(struct fi_cq_entry *) src;
	*(char**)dst += sizeof(struct fi_cq_entry);
}

static void util_cq_read_msg(void **dst, void *src)
{
	*(struct fi_cq_msg_entry *) *dst = *(struct fi_cq_msg_entry *) src;
	*(char**)dst += sizeof(struct fi_cq_msg_entry);
}

static void util_cq_read_data(void **dst, void *src)
{
	*(struct fi_cq_data_entry *) *dst = *(struct fi_cq_data_entry *) src;
	*(char**)dst += sizeof(struct fi_cq_data_entry);
}

static void util_cq_read_tagged(void **dst, void *src)
{
	*(struct fi_cq_tagged_entry *) *dst = *(struct fi_cq_tagged_entry *) src;
	*(char **)dst += sizeof(struct fi_cq_tagged_entry);
}

ssize_t ofi_cq_read(struct fid_cq *cq_fid, void *buf, size_t count)
{
	struct util_cq *cq;
	struct fi_cq_tagged_entry *entry;
	size_t i;

	cq = container_of(cq_fid, struct util_cq, cq_fid);
	fastlock_acquire(&cq->cq_lock);
	if (ofi_cirque_isempty(cq->cirq)) {
		fastlock_release(&cq->cq_lock);
		cq->progress(cq);
		fastlock_acquire(&cq->cq_lock);
		if (ofi_cirque_isempty(cq->cirq)) {
			i = -FI_EAGAIN;
			goto out;
		}
	}

	if (count > ofi_cirque_usedcnt(cq->cirq))
		count = ofi_cirque_usedcnt(cq->cirq);

	for (i = 0; i < count; i++) {
		entry = ofi_cirque_head(cq->cirq);
		if (entry->flags & UTIL_FLAG_ERROR) {
			if (!i)
				i = -FI_EAVAIL;
			break;
		}
		cq->read_entry(&buf, entry);
		ofi_cirque_discard(cq->cirq);
	}
out:
	fastlock_release(&cq->cq_lock);
	return i;
}

ssize_t ofi_cq_readfrom(struct fid_cq *cq_fid, void *buf, size_t count,
		fi_addr_t *src_addr)
{
	struct util_cq *cq;
	struct fi_cq_tagged_entry *entry;
	ssize_t i;

	cq = container_of(cq_fid, struct util_cq, cq_fid);
	if (!cq->src) {
		i = ofi_cq_read(cq_fid, buf, count);
		if (i > 0) {
			for (count = 0; count < (size_t)i; count++)
				src_addr[i] = FI_ADDR_NOTAVAIL;
		}
		return i;
	}

	fastlock_acquire(&cq->cq_lock);
	if (ofi_cirque_isempty(cq->cirq)) {
		fastlock_release(&cq->cq_lock);
		cq->progress(cq);
		fastlock_acquire(&cq->cq_lock);
		if (ofi_cirque_isempty(cq->cirq)) {
			i = -FI_EAGAIN;
			goto out;
		}
	}

	if (count > ofi_cirque_usedcnt(cq->cirq))
		count = ofi_cirque_usedcnt(cq->cirq);

	for (i = 0; i < (ssize_t)count; i++) {
		entry = ofi_cirque_head(cq->cirq);
		if (entry->flags & UTIL_FLAG_ERROR) {
			if (!i)
				i = -FI_EAVAIL;
			break;
		}
		src_addr[i] = cq->src[ofi_cirque_rindex(cq->cirq)];
		cq->read_entry(&buf, entry);
		ofi_cirque_discard(cq->cirq);
	}
out:
	fastlock_release(&cq->cq_lock);
	return i;
}

ssize_t ofi_cq_readerr(struct fid_cq *cq_fid, struct fi_cq_err_entry *buf,
		uint64_t flags)
{
	struct util_cq *cq;
	struct util_cq_err_entry *err;
	struct slist_entry *entry;
	char *err_buf_save;
	size_t err_data_size;
	uint32_t api_version;
	ssize_t ret;

	cq = container_of(cq_fid, struct util_cq, cq_fid);
	api_version = cq->domain->fabric->fabric_fid.api_version;

	fastlock_acquire(&cq->cq_lock);
	if (ofi_cirque_isempty(cq->cirq) ||
	    !(ofi_cirque_head(cq->cirq)->flags & UTIL_FLAG_ERROR)) {
		ret = -FI_EAGAIN;
		goto unlock;
	}

	ofi_cirque_discard(cq->cirq);
	entry = slist_remove_head(&cq->err_list);
	err = container_of(entry, struct util_cq_err_entry, list_entry);
	if ((FI_VERSION_GE(api_version, FI_VERSION(1, 5))) && buf->err_data_size) {
		err_data_size = MIN(buf->err_data_size, err->err_entry.err_data_size);
		memcpy(buf->err_data, err->err_entry.err_data, err_data_size);
		err_buf_save = buf->err_data;
		*buf = err->err_entry;
		buf->err_data = err_buf_save;
		buf->err_data_size = err_data_size;
	} else {
		*buf = err->err_entry;
	}
	ret = 1;
	free(err);
unlock:
	fastlock_release(&cq->cq_lock);
	return ret;
}

ssize_t ofi_cq_sread(struct fid_cq *cq_fid, void *buf, size_t count,
		const void *cond, int timeout)
{
	struct util_cq *cq;

	cq = container_of(cq_fid, struct util_cq, cq_fid);
	assert(cq->wait && cq->internal_wait);
	fi_wait(&cq->wait->wait_fid, timeout);
	return ofi_cq_read(cq_fid, buf, count);
}

ssize_t ofi_cq_sreadfrom(struct fid_cq *cq_fid, void *buf, size_t count,
		fi_addr_t *src_addr, const void *cond, int timeout)
{
	struct util_cq *cq;

	cq = container_of(cq_fid, struct util_cq, cq_fid);
	assert(cq->wait && cq->internal_wait);
	fi_wait(&cq->wait->wait_fid, timeout);
	return ofi_cq_readfrom(cq_fid, buf, count, src_addr);
}

int ofi_cq_signal(struct fid_cq *cq_fid)
{
	struct util_cq *cq;

	cq = container_of(cq_fid, struct util_cq, cq_fid);
	assert(cq->wait);
	cq->wait->signal(cq->wait);
	return 0;
}

static const char *util_cq_strerror(struct fid_cq *cq, int prov_errno,
				    const void *err_data, char *buf, size_t len)
{
	return fi_strerror(prov_errno);
}

static struct fi_ops_cq util_cq_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = ofi_cq_read,
	.readfrom = ofi_cq_readfrom,
	.readerr = ofi_cq_readerr,
	.sread = ofi_cq_sread,
	.sreadfrom = ofi_cq_sreadfrom,
	.signal = ofi_cq_signal,
	.strerror = util_cq_strerror,
};

int ofi_cq_cleanup(struct util_cq *cq)
{
	struct util_cq_err_entry *err;
	struct slist_entry *entry;

	if (ofi_atomic_get32(&cq->ref))
		return -FI_EBUSY;

	fastlock_destroy(&cq->cq_lock);
	fastlock_destroy(&cq->ep_list_lock);

	while (!slist_empty(&cq->err_list)) {
		entry = slist_remove_head(&cq->err_list);
		err = container_of(entry, struct util_cq_err_entry, list_entry);
		free(err);
	}

	if (cq->wait) {
		fi_poll_del(&cq->wait->pollset->poll_fid,
			    &cq->cq_fid.fid, 0);
		if (cq->internal_wait)
			fi_close(&cq->wait->wait_fid.fid);
	}

	ofi_atomic_dec32(&cq->domain->ref);
	util_comp_cirq_free(cq->cirq);
	free(cq->src);
	return 0;
}

static int util_cq_close(struct fid *fid)
{
	struct util_cq *cq;
	int ret;

	cq = container_of(fid, struct util_cq, cq_fid.fid);
	ret = ofi_cq_cleanup(cq);
	if (ret)
		return ret;
	return 0;
}

static struct fi_ops util_cq_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = util_cq_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

static int fi_cq_init(struct fid_domain *domain, struct fi_cq_attr *attr,
		      fi_cq_read_func read_entry, struct util_cq *cq,
		      void *context)
{
	struct fi_wait_attr wait_attr;
	struct fid_wait *wait;
	int ret;

	cq->domain = container_of(domain, struct util_domain, domain_fid);
	ofi_atomic_initialize32(&cq->ref, 0);
	dlist_init(&cq->ep_list);
	fastlock_init(&cq->ep_list_lock);
	fastlock_init(&cq->cq_lock);
	slist_init(&cq->err_list);
	cq->read_entry = read_entry;

	cq->cq_fid.fid.fclass = FI_CLASS_CQ;
	cq->cq_fid.fid.context = context;

	switch (attr->wait_obj) {
	case FI_WAIT_NONE:
		wait = NULL;
		break;
	case FI_WAIT_UNSPEC:
	case FI_WAIT_FD:
	case FI_WAIT_MUTEX_COND:
		memset(&wait_attr, 0, sizeof wait_attr);
		wait_attr.wait_obj = attr->wait_obj;
		cq->internal_wait = 1;
		ret = fi_wait_open(&cq->domain->fabric->fabric_fid,
				   &wait_attr, &wait);
		if (ret)
			return ret;
		break;
	case FI_WAIT_SET:
		wait = attr->wait_set;
		break;
	default:
		assert(0);
		return -FI_EINVAL;
	}

	if (wait)
		cq->wait = container_of(wait, struct util_wait, wait_fid);

	ofi_atomic_inc32(&cq->domain->ref);
	return 0;
}

int ofi_check_bind_cq_flags(struct util_ep *ep, struct util_cq *cq,
			    uint64_t flags)
{
	const struct fi_provider *prov = ep->domain->fabric->prov;

	if (flags & ~(FI_TRANSMIT | FI_RECV | FI_SELECTIVE_COMPLETION)) {
		FI_WARN(prov, FI_LOG_EP_CTRL,
			"Unsupported flags\n");
		return -FI_EBADFLAGS;
	}

	if (((flags & FI_TRANSMIT) && ep->tx_cq) ||
	    ((flags & FI_RECV) && ep->rx_cq)) {
		FI_WARN(prov, FI_LOG_EP_CTRL,
			"Duplicate CQ binding\n");
		return -FI_EINVAL;
	}

	return FI_SUCCESS;
}

void ofi_cq_progress(struct util_cq *cq)
{
	struct util_ep *ep;
	struct fid_list_entry *fid_entry;
	struct dlist_entry *item;

	fastlock_acquire(&cq->ep_list_lock);
	dlist_foreach(&cq->ep_list, item) {
		fid_entry = container_of(item, struct fid_list_entry, entry);
		ep = container_of(fid_entry->fid, struct util_ep, ep_fid.fid);
		ep->progress(ep);

	}
	fastlock_release(&cq->ep_list_lock);
}

int ofi_cq_init(const struct fi_provider *prov, struct fid_domain *domain,
		 struct fi_cq_attr *attr, struct util_cq *cq,
		 ofi_cq_progress_func progress, void *context)
{
	fi_cq_read_func read_func;
	int ret;

	assert(progress);
	ret = ofi_check_cq_attr(prov, attr);
	if (ret)
		return ret;

	cq->cq_fid.fid.ops = &util_cq_fi_ops;
	cq->cq_fid.ops = &util_cq_ops;
	cq->progress = progress;

	switch (attr->format) {
	case FI_CQ_FORMAT_UNSPEC:
	case FI_CQ_FORMAT_CONTEXT:
		read_func = util_cq_read_ctx;
		break;
	case FI_CQ_FORMAT_MSG:
		read_func = util_cq_read_msg;
		break;
	case FI_CQ_FORMAT_DATA:
		read_func = util_cq_read_data;
		break;
	case FI_CQ_FORMAT_TAGGED:
		read_func = util_cq_read_tagged;
		break;
	default:
		assert(0);
		return -FI_EINVAL;
	}

	ret = fi_cq_init(domain, attr, read_func, cq, context);
	if (ret)
		return ret;

	/* CQ must be fully operational before adding to wait set */
	if (cq->wait) {
		ret = fi_poll_add(&cq->wait->pollset->poll_fid,
				  &cq->cq_fid.fid, 0);
		if (ret) {
			ofi_cq_cleanup(cq);
			return ret;
		}
	}

	cq->cirq = util_comp_cirq_create(attr->size == 0 ? UTIL_DEF_CQ_SIZE : attr->size);
	if (!cq->cirq) {
		ret = -FI_ENOMEM;
		goto err1;
	}

	if (cq->domain->info_domain_caps & FI_SOURCE) {
		cq->src = calloc(cq->cirq->size, sizeof *cq->src);
		if (!cq->src) {
			ret = -FI_ENOMEM;
			goto err2;
		}
	}
	return 0;

err2:
	util_comp_cirq_free(cq->cirq);
err1:
	ofi_cq_cleanup(cq);
	return ret;
}
