#ifndef __LOG_H__
#define __LOG_H__

#include "CH56x_debug_log.h"

#ifndef DEBUG
#define log_printf(...) ((void)0)
#endif

#ifndef DEBUG
#define cprintf(...) ((void)0)
#endif

#endif
