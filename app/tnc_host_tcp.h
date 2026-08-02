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

/**
 * @brief Storage for the observer binding. Caller-owned; outlive the spine.
 *
 * Two pointers, because the transport's observer is told what a guest did and
 * the spine is what turns that into an event a user interface can see, and
 * neither knows about the other.
 */
typedef struct {
	app_spine *spine;
	ardop_host_tcp *host;
} app_tnc_watch;

/**
 * @brief Report guest activity into @p sp as ::APP_EV_GUEST events.
 *
 * Optional: ardopb does not call it, because a daemon's operator reads the same
 * information on stderr. A windowed application has no stderr anybody is
 * looking at, which is the whole reason the hook exists.
 */
void app_tnc_host_tcp_watch(app_tnc_watch *w, ardop_host_tcp *h, app_spine *sp);

#endif /* ARDOP_APP_TNC_HOST_TCP_H_ */
