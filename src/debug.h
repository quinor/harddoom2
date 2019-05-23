#ifndef _JN_HARDDOOM2_DEBUG_H
#define _JN_HARDDOOM2_DEBUG_H

#include "doomdriver.h"

#undef PDBG

#ifdef __KERNEL__
#  include <linux/kernel.h>
#  define PDBG(msg, args...) printk(KERN_DEBUG DOOMHDR "<%s:%d (%s)>: " msg "\n", \
      __FILE__, __LINE__, __func__, ## args)
#else
#  include <stdio.h>
#  define PDBG(msg, args...) printf("[DEBUG] <%s:%d (%s)>: " msg "\n", \
      __FILE__, __LINE__, __func__, ## args)
#endif

#endif // _JN_DOOMDEV_DEBUG_H
