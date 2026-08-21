/* /usr/bin/sl — the train you get for typing sl instead of ls.
 *
 * The original animates; this terminal does not, so the whole train is drawn
 * at once with its smoke trailing away above it. That is the honest version of
 * the joke here: nothing on this machine pretends to redraw the screen, and a
 * program that claimed to would be the only liar in /usr/bin.
 *
 * It is a real program in a real package. `pkg owns /usr/bin/sl` answers, and
 * `ldd /usr/bin/sl` says it needs libc like everything else -- so when a bad
 * libc upgrade has killed the machine, even the train stops running, which is
 * a diagnostic in its own small way.
 */
#include "gsys.h"

void _start(void)
{
    g_putln("                          (  ) (@@) ( )  (@)  ()    @@    O     @     O     @      O");
    g_putln("                      (@@@)");
    g_putln("                  (    )");
    g_putln("               (@@@@)");
    g_putln("             (   )");
    g_putln("      ====        ________                ___________");
    g_putln("  _D _|  |_______/        \\__I_I_____===__|_________|");
    g_putln("   |(_)---  |   H\\________/ |   |        =|___ ___|      _________________");
    g_putln("   /     |  |   H  |  |     |   |         ||_| |_||     _|                \\_____A");
    g_putln("  |      |  |   H  |__--------------------| [___] |   =|                        |");
    g_putln("  | ________|___H__/__|_____/[][]~\\_______|       |   -|                        |");
    g_putln("  |/ |   |-----------I_____I [][] []  D   |=======|____|________________________|_");
    g_putln("__/ =| o |=-~~\\  /~~\\  /~~\\  /~~\\ ____Y___________|__|__________________________|_");
    g_putln(" |/-=|___|=    ||    ||    ||    |_____/~\\___/          |_D__D__D_|  |_D__D__D_|");
    g_putln("  \\_/      \\__/  \\__/  \\__/  \\__/      \\_/               \\_/   \\_/    \\_/   \\_/");
    g_putln("");
    g_putln("     you typed sl. ls is one key to the left, and it is in a hurry too.");
    g_exit(0);
}
