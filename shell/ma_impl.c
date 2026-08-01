/*
 * The miniaudio implementation, and nothing else.
 *
 * This is the ONE translation unit in the tree not held to
 * -Wall -Wextra -Werror -Wconversion. It exists so that ~93,000 lines of
 * vendored third-party code have exactly one place to be compiled, isolated
 * from everything we wrote. See third_party/README.md for the containment
 * argument and shell/backend_ma.c for the code that actually uses it.
 *
 * Nothing may be added to this file. Anything that needs writing goes in
 * backend_ma.c, at the normal bar.
 *
 * The feature cuts below remove most of the file and all of its file I/O: we
 * need raw device capture and playback and nothing above it. MA_NO_NULL is
 * deliberately NOT defined -- miniaudio's null backend runs a device on a
 * synthesised clock, which is what lets test/core/test_backend_ma exercise the
 * ring, both resamplers and the PTT drain path in CI with no sound card.
 */

#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE

#define MINIAUDIO_IMPLEMENTATION
#include "third_party/miniaudio.h"
