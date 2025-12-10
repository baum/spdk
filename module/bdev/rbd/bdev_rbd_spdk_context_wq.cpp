/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025,2026 IBM, Inc.
 *   All rights reserved.
 */

#include <rbd/asio/ContextWQ.hpp>
#include <rbd/librbd.h>
#include <rados/librados.hpp>
#include <atomic>

#include "bdev_rbd_spdk_context_wq.h"

extern "C" {
#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/env.h"
}

namespace librbd {
namespace asio {

SpdkContextWQ::SpdkContextWQ(void* cct, struct spdk_thread* reactor_thread)
  : ContextWQ(cct), m_reactor_thread(reactor_thread) {
  assert(reactor_thread != nullptr);
}

SpdkContextWQ::~SpdkContextWQ() {
  // Set shutdown flag to reject new operations
  m_shutdown.store(true, std::memory_order_release);

  // Wait for all pending messages to complete
  drain();

  // Verify all messages are processed
  uint64_t queued = m_queued_ops.load(std::memory_order_acquire);
  if (queued > 0) {
    SPDK_ERRLOG("SpdkContextWQ::~SpdkContextWQ: Warning: %lu operations still pending during destruction\n", queued);
  }
}

void SpdkContextWQ::queue(Context *ctx, int r) {
  // Check if shutdown is in progress
  if (m_shutdown.load(std::memory_order_acquire)) {
    // ContextWQ is shutting down, complete with error
    SPDK_ERRLOG("SpdkContextWQ::queue: ContextWQ is shutting down, rejecting new operation\n");
    rbd_context_complete(ctx, -ESHUTDOWN);
    return;
  }

  // Increment queued operations counter
  m_queued_ops.fetch_add(1, std::memory_order_acq_rel);

  // Allocate message structure to pass context and return value
  auto msg = new SpdkContextMsg{ctx, r, this};

  // Schedule work on the SPDK reactor thread
  int rc = spdk_thread_send_msg(m_reactor_thread, spdk_msg_handler, msg);
  if (rc != 0) {
    // If message send failed, we need to clean up and complete with error
    m_queued_ops.fetch_sub(1, std::memory_order_acq_rel);
    delete msg;
    // Complete context with error using public API
    SPDK_ERRLOG("SpdkContextWQ::queue: calling rbd_context_complete(ctx=%p, r=%d) on error path\n", ctx, rc);
    rbd_context_complete(ctx, rc);
  }
}

void SpdkContextWQ::spdk_msg_handler(void *arg) {
  auto msg = static_cast<SpdkContextMsg*>(arg);

  if (msg == nullptr) {
    SPDK_ERRLOG("SpdkContextWQ::spdk_msg_handler: FATAL: msg is nullptr\n");
    return;
  }

  if (msg->wq == nullptr) {
    SPDK_ERRLOG("SpdkContextWQ::spdk_msg_handler: FATAL: msg->wq is nullptr\n");
    delete msg;
    return;
  }

  if (msg->ctx == nullptr) {
    SPDK_ERRLOG("SpdkContextWQ::spdk_msg_handler: FATAL: msg->ctx is nullptr\n");
    delete msg;
    return;
  }

  // Execute the context callback on the reactor thread
  rbd_context_complete(msg->ctx, msg->r);

  uint64_t queued_ops_before = msg->wq->m_queued_ops.load(std::memory_order_acquire);
  if (queued_ops_before == 0) {
    SPDK_ERRLOG("SpdkContextWQ::spdk_msg_handler: WARNING: m_queued_ops is 0, expected > 0\n");
  }

  // Update queued ops counter
  msg->wq->m_queued_ops.fetch_sub(1, std::memory_order_acq_rel);

  // Free the message structure
  delete msg;
}

void SpdkContextWQ::drain() {
  // Wait for all pending messages to be processed.
  // Note: This relies on the SPDK reactor thread to be actively polling.
  // TODO: conf parameter, non busy wait implementation
  const int max_iterations = 100000;  // 10 seconds at 100us per iteration
  int iterations = 0;

  // Wait for all queued operations to complete
  while (m_queued_ops.load(std::memory_order_acquire) > 0 &&
         iterations < max_iterations) {
    // Yield to allow SPDK reactor thread to process messages
    spdk_delay_us(100);
    ++iterations;
  }

  uint64_t queued = m_queued_ops.load(std::memory_order_acquire);
  if (queued > 0) {
    SPDK_ERRLOG("SpdkContextWQ::drain: Incomplete drain - queued_ops=%lu after %d iterations\n",
                queued, iterations);
  }
}

} // namespace asio
} // namespace librbd

// C API implementation
extern "C" {

struct bdev_rbd_spdk_context_wq* bdev_rbd_spdk_context_wq_create_from_ioctx(rados_ioctx_t io_ctx, struct spdk_thread* reactor_thread)
{
  if (io_ctx == NULL || reactor_thread == NULL) {
    return NULL;
  }

  // Convert rados_ioctx_t to librados::IoCtx to get CephContext
  librados::IoCtx ioctx;
  librados::IoCtx::from_rados_ioctx_t(io_ctx, ioctx);
  void* cct_ptr = ioctx.cct();

  if (cct_ptr == NULL) {
    SPDK_ERRLOG("Failed to get CephContext from rados_ioctx_t\n");
    return NULL;
  }

  // Create SpdkContextWQ
  uint64_t thread_id = spdk_thread_get_id(reactor_thread);
  const char *thread_name = spdk_thread_get_name(reactor_thread);
  try {
    auto wq = new librbd::asio::SpdkContextWQ(cct_ptr, reactor_thread);
    // Cast to opaque struct pointer for type safety
    struct bdev_rbd_spdk_context_wq* result = reinterpret_cast<struct bdev_rbd_spdk_context_wq*>(wq);
    SPDK_NOTICELOG("bdev_rbd_spdk_context_wq_create_from_ioctx: Successfully created SpdkContextWQ=%p with reactor thread=%p (id=%lu, name=%s)\n",
                   result, reactor_thread, thread_id, thread_name ? thread_name : "NULL");
    return result;
  } catch (...) {
    SPDK_ERRLOG("bdev_rbd_spdk_context_wq_create_from_ioctx: Failed to create SpdkContextWQ with reactor thread=%p (id=%lu, name=%s)\n",
                reactor_thread, thread_id, thread_name ? thread_name : "NULL");
    return NULL;
  }
}

void bdev_rbd_spdk_context_wq_destroy(struct bdev_rbd_spdk_context_wq* context_wq)
{
  if (context_wq == NULL) {
    return;
  }

  // Cast back to SpdkContextWQ and delete
  auto wq = reinterpret_cast<librbd::asio::SpdkContextWQ*>(context_wq);
  delete wq;
}

} // extern "C"
