/* abi.h — the syscall ABI of our machine.
 *
 * Shared verbatim by the host (which implements these) and by every guest
 * program (which calls them). One file so the two can never drift apart.
 *
 * This list IS the sandbox. A guest program has no other channel to the
 * outside world: no host filesystem, no clock, no entropy, no network except
 * through what is here. Keeping it small is a security property and a
 * determinism property at the same time.
 *
 * Numbers below 512 follow the Linux rv64 convention, so an off-the-shelf
 * toolchain's idea of write/read/exit lines up. Everything from 1024 is ours,
 * because D18 says we define the platform.
 */
#ifndef NOM_ABI_H
#define NOM_ABI_H

/* Freestanding guests have no <stdint.h>. Both sides need the same widths, so
 * they are stated once here rather than assumed twice. */
#ifdef NOM_GUEST
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long               int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;
#else
#include <stdint.h>
#endif

#define SYS_read       63
#define SYS_write      64
#define SYS_close      57
#define SYS_exit       93

#define SYS_open     1024   /* (path, flags) -> fd                          */
#define SYS_readdir  1025   /* (path, index, buf, len) -> name length or -1 */
#define SYS_stat     1026   /* (path, statbuf) -> 0 or -1                   */
#define SYS_spawn    1027   /* (path, argptr) -> exit code, or negative     */
#define SYS_getarg   1028   /* (buf, len) -> length of our argument         */
#define SYS_getpid   1029   /* () -> pid                                    */
#define SYS_bind     1030   /* (target, at) -> 0 or -1  (plan 9 bind)       */
#define SYS_unbind   1031   /* (at) -> 0 or -1                              */
#define SYS_chdir    1032   /* (path) -> 0 or -1                            */
#define SYS_getcwd   1033   /* (buf, len) -> length                         */
#define SYS_repo     1034   /* (pkg, path, buf, len) -> bytes of the file as
                             * the package SHIPPED it. This is the machine
                             * talking to a package repository, which is off
                             * the machine -- that is why reinstall can fix a
                             * disk that has nothing good left on it. */
#define SYS_chmod    1035   /* (path, mode) -> 0 or -1                      */
#define SYS_mount    1036   /* (dev, at, flags) -> 0 or -1                  */
#define SYS_umount   1037   /* (at) -> 0 or -1                              */
#define SYS_chroot   1038   /* (path) -> 0 or -1                            */
#define SYS_mounts   1039   /* (buf, len) -> bytes: the mount table         */
#define SYS_dfused   1052   /* (which) -> bytes used (0) or capacity (1)    */
#define SYS_mkdir    1053   /* (path, parents) -> 0 or -1. `parents` is the
                             * -p of mkdir: make what is missing above it.
                             * Creating a directory is a WRITE to its parent
                             * and costs an inode, and this refuses for both
                             * of those reasons, exactly as open(O_CREAT)
                             * does -- otherwise mkdir would be the one way
                             * to create something on a full filesystem. */
#define SYS_kill     1050   /* (pid, sig) -> 0 or -1. Signals are DELIVERED by
                             * being left pending; a cooperative daemon polls
                             * for them, which is all this machine can offer
                             * without preemption and is enough for HUP.    */
#define SYS_sigpend  1051   /* () -> the pending signal, and clears it      */
#define SIG_HUP  1
#define SIG_TERM 15
#define SYS_pipe     1048   /* (path, arg) -> exit code. Runs the program with
                             * this process's pipe buffer as its stdin and
                             * captures its stdout back into it, which is all
                             * a pipeline is.                              */
#define SYS_pipeout  1049   /* () -> flush the pipe buffer to the console  */
#define SYS_piperead 1054   /* (buf, len) -> bytes taken OUT of the pipe
                             * buffer, oldest first, and dropped from it.
                             * This is what makes `>` work for a real program
                             * rather than only for the echo builtin: the
                             * shell runs the command with its stdout
                             * captured, then pours the capture into a file.
                             * It is also how `$(...)` gets its answer.    */
#define SYS_setvar   1061   /* (name, value) -> 0 or -1. A shell variable.
                             * It lives on the PROCESS, not in the shell,
                             * because /bin/sh here runs once per command
                             * line and exits -- exactly as cd does, and for
                             * exactly the same reason.                    */
#define SYS_getvar   1062   /* (name, buf, len) -> length, or -1 if unset  */
#define SYS_reboot   1060   /* (halt) -> restarts THIS machine, the way the
                             * power button does. Never returns usefully.  */
#define SYS_sp       1059   /* (op, arg, buf) -> the service processor of the
                             * machine this one can reach. How a technician
                             * touches a box that will not boot.          */
#define SYS_svcinfo  1058   /* (name, buf, len) -> bytes describing ONE service:
                             * whether it is running, how many times it has
                             * been restarted, and what it said when it died.
                             * The kernel has always known this; nothing could
                             * ask.                                          */
#define SYS_restore  1057   /* (pkg, path) -> 0 or -1. Puts one path back the
                             * way the package ships it. Separate from
                             * SYS_repo because a FETCH must never write.  */
#define SYS_fstype   1056   /* (dev, buf, len) -> the type the DEVICE actually
                             * carries, which is what mount probes for and
                             * what fstab is only claiming.               */
#define SYS_needs    1055   /* (path, buf, len) -> bytes of the .nomneed list,
                             * one "soname version" per line, or -1. This is
                             * what ldd reads.                               */
#define SYS_svcstart 1047   /* (path, name, restart) -> 0 running, negative if
                             * it would not start. Unlike spawn, the program
                             * STAYS running afterwards. `restart` is 1 for
                             * on-failure, 2 for always, 0 for never.      */
#define SYS_svcctl   1063   /* (op, name) -> 0, or a negative SVCCTL_*. The
                             * other half of svcstart: a service you can start
                             * and never stop is a machine whose only repair
                             * is the power switch, and rebooting is what
                             * DESTROYS the evidence for half the faults on
                             * it. Starting a service is svcstart with a name
                             * already in the table, so it is not here.     */
#define SVCCTL_STOP    0
#define SVCCTL_RELOAD  1      /* leave a HUP on it and see whether it looks */
#define SVCCTL_ENOSVC  (-1)   /* nothing by that name started this boot     */
#define SVCCTL_ENOTRUN (-2)   /* it is in the table and it is not running   */
#define SVCCTL_ENOSIG  (-3)   /* it does not read signals: it never picked
                               * the HUP up, which is the only honest way to
                               * tell a daemon that reloads from one that
                               * would simply ignore you                    */
#define SYS_fsck     1046   /* (dev, buf, len) -> 0 clean, 1 repaired, -1 no  */
#define SYS_bootsec  1044   /* (write?) -> 1 if a boot sector is present.
                             * Not a file, so no package owns it and no
                             * amount of reinstalling will bring it back.  */
#define SYS_rootuuid 1045   /* (buf, len) -> the uuid the disk ACTUALLY has,
                             * which is how a config can be regenerated to
                             * match reality rather than to match itself.  */
#define SYS_unlink   1043   /* (path) -> 0 or -1                            */
#define SYS_readlink 1040   /* (path, buf, len) -> length of the target     */
#define SYS_dns      1041   /* (name, buf, len) -> length of the address, or
                             * -1 if the nameserver does not know it        */
#define SYS_http     1042   /* (ip, path, buf) -> bytes, or -1 if nothing is
                             * listening at that address. Takes an ADDRESS,
                             * never a name: resolution is the browser's job,
                             * which is what makes "works by ip, not by name"
                             * a diagnosis you can actually reach.          */

/* THE NETWORK, AS SEEN FROM INSIDE THE MACHINE.
 *
 * If a player cannot see a frame they cannot diagnose a network, so these
 * three are not conveniences -- they are the whole reason the stack under
 * them is worth having. Everything they return is read out of the running
 * network: an address that was really configured, a neighbour that really
 * answered, a socket that is really in that state. Nothing is derived from
 * what a config file INTENDED. */
/* ------------------------------------------------------------ RUNBOOK ---
 * THE ONE SYSCALL THIS GAME ADDED, and the only edit made to NOMINAL's
 * machine layer.
 *
 * Handoff decision 13: the player's scripts run on the emulated machine, and
 * that is the moat -- no other game in this space has a real interpreter on a
 * real machine. Decision 7: the UI is a client of the API. Put those together
 * and the machine needs exactly one way to reach the world outside it, which
 * is this: a line of the RUNBOOK protocol in, the response out.
 *
 * It is deliberately the SAME protocol the socket speaks, the desktop's forms
 * send and the reference agent plays through. A script on this machine and a
 * telnet session are the same client, so nothing the player can automate is
 * something the game cannot test -- and nothing the game can do is something
 * the player cannot automate.
 *
 * §14's protocol note applies here too: this LOOKS like an HTTP-shaped call
 * because that is a familiar shape, and it is not HTTP. Request in, response
 * out. There is no wire.
 */
#define SYS_rbapi    1080   /* (line, buf, len) -> bytes of response, or -1 */

#define SYS_netinfo  1064   /* (op, buf, len) -> bytes of text, or -1       */
#define SYS_netctl   1065   /* (op, a, b) -> 0 or -1: change the running
                             * filter, or turn the capture on              */
#define SYS_ping     1066   /* (addr, rttbuf, 0) -> a PING_* code. A real
                             * echo request down a real wire; the codes are
                             * distinct because "no route", "host
                             * unreachable" and "no answer" are three
                             * different repairs.                          */
#define SYS_traceroute 1067 /* (addr, buf, len) -> bytes of text, one hop
                             * per line: the TTL, and the address of the
                             * router that sent the time-exceeded back, or
                             * `*` for a hop that answered nothing. Real
                             * probes, real ICMP, counted by the stack.    */

#define NETINFO_IFACE  0    /* addresses, masks, carrier, counters          */
#define NETINFO_ROUTE  1    /* the routing table, connected routes included */
#define NETINFO_ARP    2    /* who answered, and how long ago               */
#define NETINFO_SOCK   3    /* real sockets in real TCP states              */
#define NETINFO_TRACE  4    /* the packet capture, one frame per line       */
#define NETINFO_PORT   5    /* the physical port: link, speed, duplex       */
#define NETINFO_FW     6    /* the running ruleset, and what each rule has
                             * really dropped. A filter nobody can read is a
                             * fault with no evidence at all: the packet does
                             * not arrive and nothing says why.             */
#define NETINFO_PCAP   7    /* THIS MACHINE'S frames, one line per frame,
                             * in fields taken out of the headers. Not the
                             * same ring as NETINFO_TRACE: that one is the
                             * whole world's events, this one is one card's
                             * traffic, which is what tcpdump(8) is.        */
#define NETINFO_VOICE  8    /* THE CALLS THIS MACHINE HAS FINISHED, which is
                             * the only network state on this list that
                             * outlives the thing it measured. Everything
                             * else here is a reading taken now; a call is
                             * over by the time anybody sits down to ask
                             * about it, so the counters are kept on the
                             * node. Concealed audio, both directions, and
                             * the verdict on the worst one.               */
#define NETINFO_VOICENOW 9  /* calls in progress at this instant, one line
                             * each. Usually none, and that is the point of
                             * the one above.                              */

#define NETCTL_FWCLEAR 0    /* forget the running ruleset                   */
#define NETCTL_FWADD   1    /* a = chain<<24 | proto<<16 | dport, b = drop  */
#define NETCTL_TRACE   2    /* a = 1 to start capturing, 0 to stop          */
#define NETCTL_PCAP    3    /* a = 1 to start the FRAME capture, 0 to stop  */
#define NETCTL_ARPDEL  4    /* a = an address: forget that neighbour. -1
                             * when there was no such entry, so `arp -d`
                             * can say it deleted nothing.                  */

/* What came back from a ping, in the order a person eliminates them. */
#define NPING_OK           0
#define NPING_TIMEOUT      1
#define NPING_NET_UNREACH  2
#define NPING_HOST_UNREACH 3
#define NPING_TTL          4
#define NPING_NO_ROUTE     5
#define NPING_IF_DOWN      6

#define MNT_BIND  1         /* `at` is another path, not a device           */
#define MNT_RO    2         /* mount read-only; on / it is a remount        */

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000

/* The layout SYS_stat fills in. Fixed-width and explicitly padded, because
 * host and guest are compiled by different compilers for different machines
 * and a layout disagreement here is a silent memory bug. */
#define NOM_KIND_FILE  1
#define NOM_KIND_DIR   2
#define NOM_KIND_LINK  3
#define NOM_KIND_DEV   4

typedef struct {
    int64_t  size;
    int32_t  mode;      /* low 9 bits, as usual */
    int32_t  kind;      /* NOM_KIND_*           */
} NomStat;

/* Service processor operations. */
#define SP_STATUS     0
#define SP_CONNECT    1
#define SP_POWER      2      /* arg: 0 off, 1 on, 2 cycle                  */
#define SP_MEDIA      3      /* arg: 0 eject, 1 insert the rescue medium   */
#define SP_BOOTDEV    4      /* arg: 0 disk, 1 the attached medium         */
#define SP_CONSOLE    5      /* read the console back                      */

/* Spawn failures are distinguishable, because "the binary is corrupt" and
 * "the binary is missing" are different tickets. */
#define SPAWN_ENOENT   (-1)   /* no such file                        */
#define SPAWN_EPERM    (-2)   /* not executable                      */
#define SPAWN_ENOEXEC  (-3)   /* not a program this machine can run  */
#define SPAWN_EFAULT   (-4)   /* it ran and trapped                  */
#define SPAWN_EDEPTH   (-5)   /* nested too deep                     */
#define SPAWN_EBUSY    (-6)   /* a service of that name is already
                               * running: starting it twice would
                               * leave two records for one name and
                               * `svc status` reading whichever it
                               * found first                        */

#endif /* NOM_ABI_H */
