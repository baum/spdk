/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025,2026 IBM, Inc.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_RBD_SPDK_CONTEXT_WQ_H
#define SPDK_BDEV_RBD_SPDK_CONTEXT_WQ_H

// Forward declaration for SPDK thread
struct spdk_thread;

// Forward declaration for rbd_image_t (defined in <rbd/librbd.h>)
// We use void* here to avoid including librbd.h in the header
#ifndef rbd_image_t
typedef void* rbd_image_t;
#endif

// Forward declaration for rados_ioctx_t (defined in <rados/librados.h>)
#ifndef rados_ioctx_t
typedef void* rados_ioctx_t;
#endif

// Opaque type for SpdkContextWQ - provides type safety in C code
// The actual implementation is C++ and is hidden behind this opaque pointer
struct bdev_rbd_spdk_context_wq;

// C API for creating SpdkContextWQ from C code (bdev_rbd.c)
// These declarations are available to both C and C++ code
#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a SpdkContextWQ from rados_ioctx_t and SPDK reactor thread.
 * The returned pointer must be freed by calling bdev_rbd_spdk_context_wq_destroy().
 *
 * @param io_ctx RADOS I/O context (rados_ioctx_t)
 * @param reactor_thread SPDK reactor thread
 * @return Pointer to SpdkContextWQ, or NULL on error
 */
struct bdev_rbd_spdk_context_wq* bdev_rbd_spdk_context_wq_create_from_ioctx(rados_ioctx_t io_ctx, struct spdk_thread* reactor_thread);

/**
 * Destroy a SpdkContextWQ created by bdev_rbd_spdk_context_wq_create_from_ioctx().
 *
 * @param context_wq Pointer to SpdkContextWQ
 */
void bdev_rbd_spdk_context_wq_destroy(struct bdev_rbd_spdk_context_wq* context_wq);

#ifdef __cplusplus
}

// C++ class definition - only available when compiling C++ code
#include <rbd/asio/ContextWQ.hpp>

namespace librbd {
namespace asio {

/**
 * ContextWQ implementation that schedules work on SPDK reactor threads
 */
class SpdkContextWQ : public ContextWQ {
public:
  explicit SpdkContextWQ(void* cct, struct spdk_thread* reactor_thread);
  ~SpdkContextWQ();

  void drain() override;

  /**
   * Queue a context to be executed on the SPDK reactor thread.

   * @param ctx Context to execute
   * @param r Return value to pass to context
   */
  void queue(Context *ctx, int r = 0) override;

private:
  struct spdk_thread* m_reactor_thread;
  std::atomic<bool> m_shutdown{false};  // Flag to prevent new operations during shutdown

  /**
   * Message handler for SPDK thread messages.
   * Executes the context callback on the reactor thread.
   */
  static void spdk_msg_handler(void *arg);

  /**
   * Internal structure to pass context and return value through SPDK message.
   */
  struct SpdkContextMsg {
    Context* ctx;
    int r;
    SpdkContextWQ* wq;  // Pointer to ContextWQ (protected by lifetime guard via m_queued_ops)
  };
};

} // namespace asio
} // namespace librbd

#endif // __cplusplus

#endif /* SPDK_BDEV_RBD_SPDK_CONTEXT_WQ_H */
