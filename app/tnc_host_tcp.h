#ifndef ARDOP_APP_TNC_HOST_TCP_H_
#define ARDOP_APP_TNC_HOST_TCP_H_

#include <stdint.h>

#include "app/spine.h"
#include "shell/host_tcp.h"

/**
 * @file tnc_host_tcp.h
 * @brief The TCP TNC interface, presented to the spine as an ::app_tnc_ops.
 *
 * This is the only file under `app/` that knows sockets exist. Everything the
 * spine needs from a TNC transport is four function pointers, so the transport
 * itself stays where it already is -- `shell/host_tcp.c`, unchanged, the same
 * code `ardopb` serves Pat and WoAD with.
 *
 * Keeping it on this side of the seam is what lets `app/spine.c` compile with no
 * feature-test macro, link without `net.o` or `sys.o`, run under
 * ThreadSanitizer with no network, and be tested against a fake. A platform
 * where a listening socket is not available simply never calls this.
 */

/**
 * @brief Fill @p ops so @p h can drive @p sp.
 *
 * @p h is borrowed and must outlive the binding. Both are the caller's to close,
 * in that order: detach from the spine, then close the server.
 */
void app_tnc_host_tcp_bind(app_tnc_ops *ops, ardop_host_tcp *h);

#endif /* ARDOP_APP_TNC_HOST_TCP_H_ */
