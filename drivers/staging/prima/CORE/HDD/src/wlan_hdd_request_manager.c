/*
 * Copyright (c) 2017-2018 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/kernel.h>
#include "wlan_hdd_request_manager.h"
#include "wlan_hdd_main.h"
#include "wlan_hdd_dp_utils.h"
#include "vos_event.h"
#include "vos_memory.h"

/* arbitrary value */
#define MAX_NUM_REQUESTS 20

static bool is_initialized;
static hdd_list_t requests;
static spinlock_t spinlock;
static void *cookie;

struct hdd_request {
	hdd_list_node_t node;
	void *cookie;
	uint32_t reference_count;
	struct hdd_request_params params;
	vos_event_t completed;
};

/* must be called with spinlock held */
static void hdd_request_unlink(struct hdd_request *request)
{
	hdd_list_remove_node(&requests, &request->node);
}

static void hdd_request_destroy(struct hdd_request *request)
{
	struct hdd_request_params *params;

	params = &request->params;
	if (params->dealloc) {
		void *priv = hdd_request_priv(request);

		params->dealloc(priv);
	}
	vos_event_destroy(&request->completed);
	vos_mem_free(request);
}

/* must be called with spinlock held */
static struct hdd_request *hdd_request_find(void *cookie)
{
	VOS_STATUS status;
	struct hdd_request *request;
	hdd_list_node_t *node;

	status = hdd_list_peek_front(&requests, &node);
	while (VOS_IS_STATUS_SUCCESS(status)) {
		request = container_of(node, struct hdd_request, node);
		if (request->cookie == cookie)
			return request;
		status = hdd_list_peek_next(&requests, node, &node);
	}

	return NULL;
}

struct hdd_request *hdd_request_alloc(const struct hdd_request_params *params)
{
	size_t length;
	struct hdd_request *request;

	if (!is_initialized) {
		hddLog(VOS_TRACE_LEVEL_ERROR,
		       FL("invoked when not initialized"));
		return NULL;
	}

	length = sizeof(*request) + params->priv_size;
	request = vos_mem_malloc(length);
	if (!request) {
		hddLog(VOS_TRACE_LEVEL_ERROR, FL("allocation failed"));
		return NULL;
	}
	request->reference_count = 1;
	request->params = *params;
	vos_event_init(&request->completed);
	spin_lock_bh(&spinlock);
	request->cookie = cookie++;
	hdd_list_insert_back(&requests, &request->node);
	spin_unlock_bh(&spinlock);
	hddLog(VOS_TRACE_LEVEL_INFO, FL("request %pK, cookie %pK"),
	       request, request->cookie);

	return request;
}

void *hdd_request_priv(struct hdd_request *request)
{
	/* private data area immediately follows the struct hdd_request */
	return request + 1;
}

void *hdd_request_cookie(struct hdd_request *request)
{
	return request->cookie;
}

struct hdd_request *hdd_request_get(void *cookie)
{
	struct hdd_request *request;

	if (!is_initialized) {
		hddLog(VOS_TRACE_LEVEL_ERROR,
		       FL("invoked when not initialized"));
		return NULL;
	}
	spin_lock_bh(&spinlock);
	request = hdd_request_find(cookie);
	if (request)
		request->reference_count++;
	spin_unlock_bh(&spinlock);
	hddLog(VOS_TRACE_LEVEL_INFO, FL("cookie %pK, request %pK"),
	       cookie, request);

	return request;
}

void hdd_request_put(struct hdd_request *request)
{
	bool unlinked = false;

	hddLog(VOS_TRACE_LEVEL_INFO, FL("request %pK, cookie %pK"),
	       request, request->cookie);
	spin_lock_bh(&spinlock);
	request->reference_count--;
	if (0 == request->reference_count) {
		hdd_request_unlink(request);
		unlinked = true;
	}
	spin_unlock_bh(&spinlock);
	if (unlinked)
		hdd_request_destroy(request);
}

int hdd_request_wait_for_response(struct hdd_request *request)
{
	return vos_wait_single_event(&request->completed,
				     request->params.timeout_ms);
}

void hdd_request_complete(struct hdd_request *request)
{
	(void)vos_event_set(&request->completed);
}

void hdd_request_manager_init(void)
{
	if (is_initialized)
		return;

	hdd_list_init(&requests, MAX_NUM_REQUESTS);
	spin_lock_init(&spinlock);
	is_initialized = true;
}

/*
 * hdd_request_manager_deinit implementation note:
 * It is intentional that we do not destroy the list or the spinlock.
 * This allows threads to still access the infrastructure even when it
 * has been deinitialized. Since neither lists nor spinlocks consume
 * resources this does not result in a resource leak.
 */
void hdd_request_manager_deinit(void)
{
	is_initialized = false;
}
