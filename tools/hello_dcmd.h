/* Commands understood by hellodev, the test device. */
#ifndef HELLO_DCMD_H
#define HELLO_DCMD_H

#include <devctl.h>

#define DCMD_HELLO_GET   __DIOF(_DCMD_MISC, 0x51, int)   /* read the value */
#define DCMD_HELLO_SET   __DIOT(_DCMD_MISC, 0x52, int)   /* write the value */
#define DCMD_HELLO_ADD   __DIOTF(_DCMD_MISC, 0x53, int)  /* add to the value, get the sum back */
#define HELLO_INFO_MARK  7                               /* GET returns this as its info value */

#endif
