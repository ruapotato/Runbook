/* machine.h — an installed system, its package database, and its boot chain.
 *
 * D17. The rule this file exists to enforce: the boot is SIMULATED, not
 * narrated. Every stage reads real files out of the machine's own Vfs and
 * fails because of what it finds there. There is no fault id anywhere in this
 * header, and there must never be one, because the moment a stage asks "which
 * fault is active?" instead of "what does this file say?", the game collapses
 * back into a symptom table.
 */
#ifndef NOM_MACHINE_H
#define NOM_MACHINE_H

#include "ns.h"

#define PKG_MAX        64
/* The most files one package may declare. nomsh, the base tools, was
 * sitting exactly on 48 when the network instruments were added to it, and
 * a package that overflows this is caught loudly by the installer rather
 * than silently shipping short -- which is the only reason this number can
 * be moved safely at all. */
/* RUNBOOK: 56 -> 64. Adding /bin/rb to nomsh put it exactly one over, and
 * the installer said so loudly and refused to boot -- which is the check
 * doing its job, and the reason this number can be moved at all. */
/* RUNBOOK: 64 -> 80. The shell package carries /bin/rb, /bin/py, /bin/test,
 * /bin/[ and seven example scripts on top of what NOMINAL shipped, and 64 was
 * exactly one file short. The array truncates silently; what caught it was
 * image.c's own consistency check, which compares the declared count against
 * what actually fits and names the entry that went missing. That check is
 * worth more than the constant. */
#define PKGFILE_MAX    80
#define UNIT_MAX       32
#define CONSOLE_MAX    120
#define PROC_MAX        32

/* A running program. The table lives in the Machine because the machine IS
 * the computer: /proc is a view of this, not of anything on the disk, which
 * is why corrupting the disk cannot fabricate a process. */
/* Shell variables belong to the PROCESS. /bin/sh on this machine runs once
 * per command line and exits, so a variable it kept in its own memory would
 * be gone before the player pressed return again -- which is why `X=5`
 * followed by `echo $X` printed nothing. cwd and the namespace persist here
 * for the identical reason, and now so does this. Sixteen is a shell, not an
 * environment: enough for the handful of paths and uuids someone stashes
 * while working a ticket. */
#define VAR_MAX 16
typedef struct { char name[32], val[192]; } ShVar;

typedef struct {
    int      pid, ppid;
    char     name[64];
    char     arg[128];
    char     cwd[NOM_PATH_MAX];
    char     root[NOM_PATH_MAX];   /* chroot: what "/" means to this process */
    ShVar    var[VAR_MAX];
    int      nvar;
    Ns       ns;
    bool     alive;
    int64_t  exit_code;
    uint64_t icount;
} ProcInfo;

/* A file as its package shipped it. `hash` is of the pristine content, so a
 * corrupted file is detectable without keeping a second copy of the tree —
 * except that we DO keep the content, because `pkg reinstall` has to put the
 * original back and a hash cannot do that. */
typedef struct {
    const char *path;
    const char *content;   /* NULL for a directory */
    unsigned    mode;
    const char *link;      /* non-NULL: this entry is a symlink to `link` */
    /* A DIRECTORY owned by the package. rpm and dpkg both record these, and
     * for the reason we found the hard way: a directory that goes missing, or
     * loses its execute bit, is a real fault with no file to blame, and a
     * package manager that cannot see it also cannot put it back. Appended
     * last so every existing positional initialiser keeps working. */
    bool        isdir;
} PkgFile;

typedef struct {
    const char *name;
    const char *version;
    const char *desc;
    PkgFile     file[PKGFILE_MAX];
    int         nfiles;
} Package;

/* Where a boot got to. The player reads these names constantly, so they are
 * the vocabulary of the whole game. */
typedef enum {
    BOOT_FIRMWARE,   /* find something to boot                     */
    BOOT_LOADER,     /* read the bootloader config                 */
    BOOT_KERNEL,     /* load the kernel image                      */
    BOOT_INITRD,     /* load initrd, find and mount the root fs    */
    BOOT_INIT,       /* hand over to /sbin/init                    */
    BOOT_SERVICES,   /* bring up units in dependency order         */
    BOOT_LOGIN,      /* getty: is there an account to hand it to?  */
    BOOT_TARGET,     /* login prompt: the machine is up            */
    BOOT_STAGE_COUNT
} BootStage;

const char *boot_stage_name(BootStage s);

typedef struct {
    bool       running;      /* did the machine reach BOOT_TARGET */
    BootStage  reached;      /* furthest stage entered */
    BootStage  failed_at;    /* stage that stopped it (== reached on failure) */
    char       reason[160];  /* the machine's own words, never a diagnosis */
    Buf        console;      /* everything the boot printed, in order */
    /* Stopped inside the initrd, before there was a userland. NOT "dropped to
     * an emergency shell": this initrd is built out of driver modules and has
     * no shell in it, which is what the console now says. See initrd_no_shell
     * in boot.c. */
    int        emergency;
} BootResult;

#define MOUNT_MAX 12

/* A mounted filesystem. `dev` is what you named when you mounted it, so
 * `mount` can print the table the way mount(8) does. */
typedef struct {
    char  at[NOM_PATH_MAX];
    char  dev[40];
    Vfs  *fs;
    char  sub[NOM_PATH_MAX];   /* a bind mount names a subtree, not a device */
    bool  used;
} Mount;

typedef struct Machine_ {
    char  id[16];            /* "4823" — the seed, and the machine's name  */
    Vfs   disk;              /* the customer's installed system, /dev/sda1  */
    /* The rescue medium: a complete, separate, working system that is never
     * corrupted. Booting it is how you get a shell on a machine whose own
     * disk cannot produce one -- which is the whole point of a live image. */
    Vfs   rescue;
    bool  on_rescue;         /* which medium did we boot                    */
    /* WHAT VERSION ACTUALLY LOADED, as opposed to what is installed.
     *
     * `uname` had 6.4.11 compiled into it and never read anything, so on a
     * machine booted from a valid image of the WRONG version the loader said
     * "kernel 6.3.12 booting" and uname cheerfully said 6.4.11. The fault
     * catalogue even suggested comparing uname against /lib/modules as the
     * diagnosis -- against a tool that could not participate. A running system
     * knows which kernel is running; this is where it knows it. */
    char  booted_kver[32];

    /* THE MACHINE THIS ONE CAN REACH.
     *
     * A support engineer does not sit at the broken box. They sit at their
     * own workstation -- a healthy install of the same system, which is what
     * makes "compare it with mine" possible -- and reach the customer's
     * machine through its service processor, the way iDRAC or iLO works: a
     * little computer on the motherboard that is up even when the host is
     * not, and can power cycle it, attach media, choose a boot device and
     * show you the console.
     *
     * `peer` is that link. The workstation points at the customer's machine;
     * the customer's machine points at nothing. */
    struct Machine_ *peer;
    char  peer_addr[32];     /* what the customer reads off the sticker     */
    /* NO SERVICE PROCESSOR, NO NETWORK. Some machines are air-gapped: a
     * secure site, a factory floor, a box that was never on the network. You
     * cannot reach it at all, and the only terminal you have is the person
     * standing in front of it. You dictate; they type; they read back what
     * they see, badly. It is a real support scenario and a genuinely
     * different puzzle -- every command costs a round trip through somebody
     * who does not know what any of it means. */
    /* IS THERE ELECTRICITY IN IT.
     *
     * Not the same question as "did it boot", and conflating the two made the
     * service processor lie: a machine that powered on and died at initrd
     * reported "power off", which is what you would see if it were unplugged.
     * A real BMC tells you those apart, and telling them apart is the whole
     * reason you trust it. */
    bool  powered;
    bool  airgapped;
    bool  sp_connected;      /* has the technician attached to the console  */
    /* THE DRIVE AND THE FIRMWARE BELONG TO THE MACHINE THEY ARE IN.
     *
     * These two lived on the WORKSTATION -- the machine doing the reaching --
     * so the customer's box had no idea whether there was a disc in its own
     * drive. `rcon media eject` printed "virtual drive emptied" and then
     * `blkid` on that machine still reported /dev/sr0 TYPE="iso9660", because
     * the device table answered from a constant and the status bit answered
     * from the workstation. Two pieces of state for one fact, and they could
     * disagree; a playtester found them disagreeing.
     *
     * They are now the target's own, so there is one answer: what is in the
     * drive is what blkid sees, what mount can mount, and what the firmware
     * boots. sp_connected stays on the workstation, because attachment really
     * is a property of the technician's end. */
    bool  sp_media;          /* is the rescue medium in THIS machine's drive */
    int   sp_bootdev;        /* 0 disk, 1 the medium: what IT boots next     */
    /* An unclean shutdown leaves the filesystem marked dirty. Nothing will
     * mount it until fsck has been run, which is the point: the repair has to
     * happen BEFORE you can even look at the disk. */
    /* The disk has a size. Everything up to now could write forever, so a log
     * that grows was not a fault and could not become one -- and "the disk
     * filled up" is one of the commonest real causes there is. */
    uint64_t fs_capacity;
    /* The other thing a filesystem runs out of. Space and inodes are
     * independent, and exhausting the second is much harder to see. */
    uint64_t fs_inodes_max;
    bool  fs_dirty;
    int   fs_lost;           /* files fsck could not save                   */
    /* Which repository channel `pkg` pulls from. Read off the disk at
     * the .repo files under /etc/pkg/repos.d, so pointing it at the wrong
     * one is a configuration fault and the packages that arrive are
     * genuinely different. */
    char  channel[24];
    Mount mount[MOUNT_MAX];
    int   nmount;
    /* The root filesystem is not in the mount table -- it is the thing the
     * table is relative to -- so "mounted read-only" has to live here. Set by
     * /sbin/mountall when the fstab entry for / carries the ro option, which
     * is exactly the moment a real init decides whether to remount rw. */
    bool  root_ro;
    char  root_uuid[40];     /* what the root partition actually IS        */
    bool  bootsector;        /* firmware can find something to chain to    */
    BootResult boot;
    /* The package database is a pointer into static image data plus a
     * per-machine record of what has been reinstalled. Packages never change,
     * so they do not need copying per machine. */
    const Package *pkg[PKG_MAX];
    int   npkg;

    /* Legitimate local edits this machine's admin made. They show up in
     * `pkg verify` as CHANGED and they are NOT the fault -- reinstalling the
     * package destroys real work and usually creates a second problem. This
     * is what stops verify from being an oracle. */
    char  local[8][NOM_PATH_MAX];
    int   nlocal;
    /* What each local edit looked like when the machine arrived, so the bench
     * can tell the player afterwards which of the administrator's decisions
     * they reverted. Fixing the machine and quietly undoing somebody's work
     * is not the same as fixing the machine. */
    Buf   local_orig[8];

    /* The person whose machine it is. Briefed with ground truth from the
     * breaker, and unwilling to volunteer it. See customer.c. */
    struct {
        char truth[256];      /* exactly what the breaker did              */
        int  cause;           /* their version of it                       */
        int  mood;
        int  asked;
        char told[32];        /* questions already answered, by option id  */
        bool deflected;       /* denied it once, as people do              */
        bool confessed;
        bool gave_password;
        /* HAS THE CALL ACTUALLY STARTED.
         *
         * Set once she has answered any of the open questions -- what were you
         * doing, when did it last work, what do you use it for. The four
         * pointed questions (deleted anything / installed updates / anybody
         * else / lost power) are the differential diagnosis in list form, so
         * they are not offered until then: a technician earns the right to ask
         * about Friday's updater by hearing her mention Friday. */
        bool opened;
        /* The customer is also the pair of hands in the room. The technician
         * cannot press the power button; they have to ask. */
        bool at_machine;      /* are they sitting in front of it right now  */
        bool disc_inserted;   /* have they put the rescue medium in         */
        int  power_cycles;

        /* WHO they are, on top of what they know. Drawn per ticket, so the
         * same fault twice is not the same phone call twice. */
        int  persona;

        /* WHAT IS IN FRONT OF HER, and how far up it she has read.
         *
         * A terminal is a fixed number of lines high and she can only see the
         * bottom of it. This is the copy she is reading from: the last thing
         * the machine printed, held so that "it has scrolled off -- do you
         * want me to do it again?" can be answered yes. `scroll` is the index
         * of the topmost line she has read back so far, so each further
         * request pages UP through material that really was printed. */
        Buf  screen;
        int  nlines;
        int  scroll;
        /* Unprompted observations she has already made. True things about her
         * machine, offered for free, and rationed: a person who volunteers
         * something every single time is a hint system. */
        int  remarks;
    } cust;

    /* Daemons. A service that starts does not run to completion: it runs
     * until it blocks or its startup budget is spent, and then it STAYS
     * RUNNING, with its cpu and memory intact, for the rest of the boot.
     * That is what makes `ps` a picture of a live system rather than a
     * history, and it is what lets a service crash at 11am. */
    struct Daemon *daemon;
    int    ndaemon;

    ProcInfo proc[PROC_MAX];
    int      nproc;        /* high-water mark, so exited pids stay visible */
    int      next_pid;

    /* WHERE THIS BOX IS ON THE WIRE.
     *
     * The network is not per machine -- it is one world that every machine
     * is plugged into, because that is what a network is and because a
     * megabyte of frame queue per machine would price a tower out. All the
     * machine keeps is which node it is, which configuration that node was
     * built from, and which generation of the world it belongs to. Zero
     * means "not plugged in yet", which is what a zeroed Machine is.
     *
     * NOTHING HERE IS A NETWORK FACT. There is no address, no route and no
     * reachability in this struct, deliberately: the moment a machine
     * cached its own address, the address on the wire and the address in
     * the struct could disagree, and every tool would have to pick one. See
     * core/netsite.c. */
    int      net_node;
    int      net_port;     /* which port of the switch the cable is in */
    uint32_t net_cfg;
    uint32_t net_gen;
    /* PINNED TO A NETWORK SOMEBODY ELSE BUILT.
     *
     * A break-fix machine is plugged into the one network this process keeps
     * for it, on a port netsite.c chose. A machine standing in the player's
     * own tower is not: it is a box in a rack with the player's cable in it,
     * on a switch the player bought, and the node already exists before the
     * operating system does. When `net_home` is set, netsite.c stops
     * allocating and cabling and only does the other half of its job --
     * reading the config off this disk and applying it to that node -- so
     * `ping` inside the shell goes over the copper the player paid for.
     *
     * NULL on every machine that is not in a tower, which is every machine
     * the break-fix game has ever made. See core/session.c. */
    struct Net *net_home;
} Machine;

/* Build a pristine installation. Deterministic: same seed, same machine. */
void machine_install(Machine *m, uint64_t seed);
void machine_free(Machine *m);

/* Run the boot chain against whatever is on the disk right now. Pure function
 * of disk state — call it as often as you like. */
void machine_boot(Machine *m);

/* Boot the rescue medium instead of the customer's disk. The rescue system is
 * a complete, separate installation that is never corrupted, so this always
 * gets you a shell -- which is the entire reason a live image exists. */
void machine_boot_rescue(Machine *m);

/* The customer. customer_brief is given the breaker's own description of what
 * it did. That string is ground truth and the customer NEVER repeats it: she
 * is told what a person would have noticed and nothing else, so there is no
 * secret in her mouth to leak. See the head of customer.c. */
void customer_brief(Machine *m, const char *what);
/* The customer's name. Bound to the persona, so a name is a person. */
const char *customer_name(const Machine *m);

/* WHAT YOU CAN SAY TO HER RIGHT NOW, as a numbered list.
 *
 * The numbers are stable ids, not positions: what she can do changes every
 * time the machine does, and a list that renumbers under the player's fingers
 * produces the one mistake a menu exists to prevent. The list is filtered, so
 * it never offers something that cannot work, and it always leaves a way
 * forward. */
void customer_options(Machine *m, Buf *out);
/* Say option `idx`. `arg` carries the command for the dictate option and is
 * NULL otherwise. Writes what she does and what she says. */
void customer_choose(Machine *m, int idx, const char *arg, Buf *out);
void customer_intro(Machine *m, Buf *out);
/* Which local configuration decisions no longer survive. Returns how many. */
int machine_collateral(Machine *m, Buf *out);
/* Damage still on the disk, whether or not the machine boots. A ticket is
 * "prove it is healthy", not "prove it starts today". */
int machine_outstanding(Machine *m, Buf *out);

/* Hand the machine back. Writes what the customer says and, if the claim does
 * not hold, what is still wrong -- at the customer's level of knowledge, so
 * naming the fault stays the player's job. Returns true if the ticket closed.
 *
 * ONE FUNCTION BECAUSE THERE ARE THREE FRONT ENDS. The socket server, the
 * --desk loop and the desktop must not be able to disagree about whether a
 * job is finished; that is the same rule that keeps the desktop a view of the
 * machine rather than a second opinion about it. */
bool machine_handback(Machine *m, Buf *out);

bool machine_mount(Machine *m, const char *dev, const char *at, int flags);
/* Check and repair the filesystem. Clears the dirty flag; reports what it
 * could not save. Metadata is repairable, contents are not -- which is why a
 * dirty filesystem is usually two repairs, not one. */
int  machine_fsck(Machine *m, const char *dev, Buf *out);
void machine_read_channel(Machine *m);
/* Bytes in use on the customer's disk, counted from the tree. */
uint64_t machine_disk_used(const Machine *m);
uint64_t machine_inodes_used(const Machine *m);

/* Start a program as a long-lived service. Returns 0 if it is now running,
 * or a negative SPAWN_* if it could not be started at all. */
/* `inherit` is the namespace the caller was standing in: a service inherits
 * its parent's view of the filesystem like any other child, so a bind made
 * before the services start applies to them too. NULL means an empty one. */
int64_t kernel_start_daemon(Machine *m, const char *path, const char *arg,
                            const char *name, int restart, Buf *console,
                            const Ns *inherit);
/* Let every running daemon have another slice of cpu. A daemon that exits or
 * faults during its slice has crashed, and says so. */
void kernel_tick(Machine *m, int slices, Buf *console);
void kernel_stop_daemons(Machine *m);
/* Stop ONE service, and ask one to re-read its configuration. The other half
 * of starting them: a service that can only be started is a machine whose
 * only repair is the power switch. Both return 0 or a negative SVCCTL_*. */
int  kernel_svc_stop(Machine *m, const char *name);
int  kernel_svc_reload(Machine *m, const char *name, Buf *console);
/* How many services that should be running are not, and which. A machine can
 * boot perfectly and still be broken; this is the difference. */
int  kernel_health(Machine *m, Buf *out);
bool kernel_console_dead(Machine *m, const char *cmd, Buf *out);
bool machine_umount(Machine *m, const char *at);

/* --- the package database, which is the fix verb ---------------------- */
const Package *pkg_find(const Machine *m, const char *name);
const Package *pkg_owns(const Machine *m, const char *path);
/* Files that differ from what their package shipped. One `path status` line
 * each: missing | changed | mode. */
void pkg_verify(Machine *m, const char *name_or_null, Buf *out);
/* Put a package's files back exactly as shipped. Returns files restored. */
int  pkg_reinstall(Machine *m, const char *name, Buf *out);
/* Put ONE path back. Mutating; kept apart from pkg_file_content so that a
 * fetch can never change the machine. */
bool pkg_restore_path(Machine *m, const char *pkgname, const char *path);
/* Baseline the local edits against the disk as the PLAYER receives it, so the
 * collateral report blames only the player. */
void machine_rebaseline_local(Machine *m);
/* The pristine bytes of one file, as its package shipped it. This is the
 * repository, and it deliberately lives OFF the machine: it is how a disk
 * with nothing good left on it can still be repaired. */
bool pkg_file_content(const Machine *m, const char *pkg, const char *path, Buf *out);

/* --- the breaker is gone -----------------------------------------------
 * machine_corrupt, machine_break, machine_airgapped, breaker_syslog,
 * breaker_powerfail and breaker_bad_sector took a real disk and put real
 * wrong bytes on it, which is what `pkg verify` and `fsck` were built to
 * find. David: "we are not going to corrupt the computer OS anymore."
 *
 * THE TOOLS STAY AND STILL WORK. fsck, pkg verify, pkg diff, the service
 * processor and the rescue medium are real programs on a real machine; what
 * has gone is the thing that gave them something to find. The maintenance
 * half of this game is the SHIP -- cut cable, burnt segments, damaged
 * systems, fires -- and that half is played and gated.
 */

/* WHAT ONE MACHINE LOST WHEN THE PLUG CAME OUT, and it is deliberately not
 * the same thing on every box in the building.
 *
 * D28. A playtester met a mains failure that took three servers down and
 * said: *"Three servers down from one cause was three instances of the same
 * puzzle."* They were right -- breaker_powerfail() dealt every downed box the
 * same fault_unclean_shutdown. These are the casualties an unclean stop can
 * really leave, one per machine, and core/siteday.c deals them round-robin so
 * that a blackout across a floor of servers is a floor of different mornings.
 * PF_CLEAN is the box that was idle: dirty, and nothing lost. */
typedef enum {
    PF_CLEAN = 0,  /* the journal replays and nothing is missing            */
    PF_TRUNC,      /* the file it was writing is half there                 */
    PF_CONF,       /* a daemon's config stops mid-file                      */
    PF_SVC,        /* a file created just before the stop is gone entirely  */
    PF_KIND_COUNT
} PowerfailKind;
int  breaker_powerfail_kinds(void);
const char *breaker_powerfail_kind_name(int k);
void breaker_powerfail_as(Machine *m, Rng *r, int kind, char *d, size_t ds);

/* A SECTOR THAT WILL NOT READ BACK, with the fairness rule made an argument.
 * breaker_bad_sector() keeps the boot chain's own files out of reach so the
 * box still comes up and can be worked on. A disk that has already lost one
 * sector and was not replaced has no such courtesy left, and `boot_too`
 * says so. */
bool breaker_bad_sector_any(Machine *m, Rng *r, bool boot_too, char *d, size_t ds);

/* WHETHER THIS PATH IS BETWEEN THE PLAYER AND A SHELL. The first sector a disk
 * loses is kept off these so the box comes up and can be worked on; the second
 * one, on a disk nobody replaced, is aimed AT them.
 *
 * It is exported because --eventcheck kept its own copy of the list and the
 * two drifted apart the moment breaker.c's grew. One fact, one place: the gate
 * now asks the predicate the damage was dealt by, and proves the consequence
 * separately by watching the box fail to reach target. */
bool breaker_boot_critical(const char *path);

/* Every file a lost sector could land on, for `boot_too` false (the first one,
 * which leaves the box workable) or true (the second, aimed at the boot). Fills
 * `out` up to `max` and returns the true count. */
int breaker_sector_targets(const Machine *m, bool boot_too,
                           const char **out, int max);

const char *breaker_dealt(void);
int         breaker_fault_count(void);
const char *breaker_fault_name(int i);

#endif /* NOM_MACHINE_H */
