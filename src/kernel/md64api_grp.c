#include "moduos/kernel/md64api_grp.h"

/*
 * Currently this module is header-only from the public API perspective.
 * Kernel/userland interact with graphics devices via $/dev/graphics/*.
 *
 * This translation unit exists to provide a stable place for future GRP
 * helpers without needing to touch the main md64api.c.
 */

