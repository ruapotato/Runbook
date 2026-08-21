/* /bin/whoami — there is one account and it is root. Saying so plainly is
 * better than pretending there is a user model there is not. */
#include "gsys.h"
void _start(void){ g_putln("root"); g_exit(0); }
