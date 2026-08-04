#include "shell/build.h"

#include "shell/host.h"

#include <stdio.h>

/**
 * @file build.c
 * @brief The build identifier (see build.h).
 *
 * The values arrive as macros from the build system. This file is the only one
 * that receives them, so a change of commit rebuilds one object and not the
 * whole tree.
 */

#ifndef ARDOP_BUILD_ID
/* No build system value. A person compiling one file by hand gets this. */
#define ARDOP_BUILD_ID "unknown"
#endif

#ifndef ARDOP_BUILD_DATE
#define ARDOP_BUILD_DATE "unknown"
#endif

const char *ardop_build_id(void)
{
	/* Never empty: an empty string in a fault report looks like a missing
	 * field, and a person cannot tell it from a field nobody filled in. */
	const char *id = ARDOP_BUILD_ID;
	return id[0] ? id : "unknown";
}

const char *ardop_build_date(void)
{
	const char *date = ARDOP_BUILD_DATE;
	return date[0] ? date : "unknown";
}

const char *ardop_build_line(const char *program, char *out, unsigned long cap)
{
	snprintf(out, (size_t)cap, "%s %s (%s), protocol %s_%s",
		 program ? program : "ardop", ardop_build_id(),
		 ardop_build_date(), ARDOP_HOST_PRODUCT, ARDOP_HOST_VERSION);
	return out;
}
