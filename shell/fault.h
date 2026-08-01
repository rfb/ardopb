#ifndef ARDOP_SHELL_FAULT_H_
#define ARDOP_SHELL_FAULT_H_

/**
 * @file fault.h
 * @brief Device faults that the application, not the loop, must react to.
 *
 * ::ardop_platform_ops has no error channel: `read_audio` returns a count and
 * `set_ptt` returns `void`. That is the right shape for the driver loop, which
 * should contain no policy. But a capture device that has been unplugged, or a
 * CAT link that has dropped while the rig is keyed, has to reach *somebody* --
 * a modem that believes it is transmitting and is not must not keep feeding the
 * link.
 *
 * So faults are latched on the concrete backend and polled by `shell/main.c`,
 * which already holds the backend pointer. `shell/loop.c` never learns about
 * them, and `shell/platform.h` stays frozen. The correct reaction -- abort the
 * ARQ session, unkey, tell the operator, offer reselection -- is application
 * policy and belongs where the application is.
 */
typedef enum {
	ARDOP_FAULT_NONE = 0,
	ARDOP_FAULT_CAPTURE_LOST,      /**< No callback within the timeout. */
	ARDOP_FAULT_PLAYBACK_LOST,     /**< Playback stalled; the ring never drained. */
	ARDOP_FAULT_PLAYBACK_UNDERRUN, /**< The device wanted samples we did not have. */
	ARDOP_FAULT_PTT_LOST           /**< Keying failed, e.g. the rigctld socket dropped. */
} ardop_fault;

/** @brief A short human-readable name for @p f. Never NULL. */
const char *ardop_fault_str(ardop_fault f);

#endif /* ARDOP_SHELL_FAULT_H_ */
