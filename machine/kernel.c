/* kernel.c — the host side of the machine: syscalls backed by the real
 * filesystem, and process spawn.
 *
 * This is what a guest binary is actually talking to when it executes `ecall`.
 * Everything here is a deterministic function of the machine's disk and the
 * program's own behaviour. There is no clock, no host filesystem, no entropy.
 *
 * SPAWN, and why it exists in this shape: a real boot is a chain of programs,
 * each of which can be corrupted independently. Rather than build an MMU and a
 * scheduler to get that, spawn runs the child on its own fresh CPU to
 * completion and returns its exit code. It is exactly the "run this and wait"
 * that an rc script does, and it gives every stage of the boot its own
 * separately-breakable binary.
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "nom.h"
#include "cpu.h"
#include "abi.h"
#include "machine.h"
#include "ns.h"
#include "kernel.h"

/* Set by the host so a guest program can reach the RUNBOOK API. NULL in a
 * bare machine, which is a state the syscall handles rather than assumes
 * away. */
void (*rb_api_hook)(Machine *m, const char *line, Buf *out) = NULL;

/* THE NETWORK, WHICH IS NOW A NETWORK. These used to be net_dns() and
 * net_fetch(): two table lookups in net_sites.c that could not fail for any
 * reason a player could find. They go through core/netsite.c now, which puts
 * a query on a wire and waits for a packet -- so a machine with a dead
 * resolver, a stopped netd or an address it never got from DHCP fails here,
 * and fails differently in each case. */
bool netsite_dns(Machine *m, const char *name, char *out, size_t cap);
bool netsite_http(Machine *m, const char *ip, const char *path, Buf *out);
void netsite_info(Machine *m, int op, Buf *out);
int  netsite_ping(Machine *m, const char *dst, int *rtt);
void netsite_fw_clear(Machine *m);
void netsite_fw_add(Machine *m, int chain, int proto, int dport, int drop);
void netsite_trace(Machine *m, int on);
void netsite_pcap(Machine *m, int on);
void netsite_traceroute(Machine *m, const char *dst, Buf *out);
int  netsite_arp_del(Machine *m, const char *addr);

#define FD_MAX      16
#define SPAWN_DEPTH  8
/* A boot that never finishes is a real failure, and a real one to diagnose.
 * The budget is per program and generous: a correct guest uses a tiny
 * fraction of it. */
#define PROC_BUDGET  40000000ull

typedef struct {
    bool  used;
    Vfs  *fs;                 /* which filesystem this path is really on */
    char  path[NOM_PATH_MAX];
    Buf   data;        /* the whole file, read at open */
    size_t pos;
    bool  writable;
} Fd;

struct Proc {
    Machine *m;
    Buf     *console;
    Fd       fd[FD_MAX];
    char     arg[NOM_ARG_MAX];
    int      depth;
    int      pid;            /* index into m->proc                       */
    ProcInfo *info;          /* our own row: cwd and namespace live here */

    /* PIPES. A process holds one buffer that is both the output of the last
     * stage it ran and the input of the next. `a | b | c` is three children
     * in a row, each reading what the previous one wrote. There is no
     * concurrency and none is needed: these are filters, not conversations. */
    Buf      pipe;
    Buf     *stdin_from;     /* fd 0 reads from here, if set              */
    size_t   stdin_pos;
    bool     capture;        /* fd 1 goes to the parent's pipe, not the console */
    Buf     *capture_into;
};

/* A long-lived service. Declared up here rather than beside its own code
 * because the syscall handler needs to see it: kill() has to find the
 * target, and the target is a daemon. */
struct Daemon {
    Cpu      cpu;
    Proc     proc;
    char     name[40];
    char     path[NOM_PATH_MAX];
    bool     running;
    int      restart_policy;   /* 0 never, 1 on-failure, 2 always */
    int      restarts;
    bool     gave_up;
    int      pending_sig;      /* delivered when the daemon next asks */
    /* THE NAMESPACE IT WAS STARTED IN. A child inherits its parent's view of
     * the filesystem -- that is the whole of the Plan 9 model and the reason
     * `bind` is worth having -- and daemons were the one kind of process that
     * did not: every service started with an empty namespace, so a bind made
     * before the services came up applied to everything except the services.
     * Kept here as well as in the process record because a restart has to
     * come back into the same view it died in. */
    Ns       ns;
    int64_t  exit_code;
    char     died[NOM_ERR_MAX];
};

/* Which filesystem is a path actually on, and where on it?
 *
 * Four transforms, in the order a real kernel applies them:
 *   1. relative -> absolute, against the process's cwd
 *   2. chroot   -> the process's root is prepended, so "/" means /mnt
 *   3. namespace-> plan 9 bindings, longest prefix (ns.c)
 *   4. mounts   -> longest mountpoint wins; the remainder is the path on
 *                  THAT filesystem
 *
 * Step 4 is what makes `mount /dev/sda1 /mnt` real: below /mnt, lookups stop
 * happening on the rescue medium and start happening on the customer's disk.
 */
static const char *device_type(const Machine *m, const char *dev);

static Vfs *resolve_fs(Proc *p, const char *in, char *out, size_t outsz)
{
    Machine *m = p->m;
    char abs[NOM_PATH_MAX * 2], rooted[NOM_PATH_MAX * 2], nsr[NOM_PATH_MAX * 2];

    vfs_normalize(p->info ? p->info->cwd : "/", in, abs, sizeof abs);

    const char *root = (p->info && p->info->root[0]) ? p->info->root : "/";
    if (strcmp(root, "/") != 0)
        snprintf(rooted, sizeof rooted, "%s%s", root, abs);
    else
        snprintf(rooted, sizeof rooted, "%s", abs);

    if (p->info) ns_resolve(&p->info->ns, rooted, nsr, sizeof nsr);
    else         snprintf(nsr, sizeof nsr, "%s", rooted);

    Vfs *fs = m->on_rescue ? &m->rescue : &m->disk;
    int best = -1;
    size_t bestlen = 0;
    for (int i = 0; i < m->nmount; i++) {
        if (!m->mount[i].used) continue;
        if (!m->mount[i].fs) continue;      /* virtual: nothing is layered */
        size_t al = strlen(m->mount[i].at);
        if (strncmp(nsr, m->mount[i].at, al) != 0) continue;
        if (!(al == 1 || nsr[al] == 0 || nsr[al] == '/')) continue;
        if (best < 0 || al > bestlen) { best = i; bestlen = al; }
    }
    if (best >= 0) {
        const char *rest = (bestlen == 1) ? nsr : nsr + bestlen;
        if (!*rest) rest = "/";
        if (m->mount[best].sub[0]) {
            char j[NOM_PATH_MAX * 2];
            snprintf(j, sizeof j, "%s%s", m->mount[best].sub, rest);
            vfs_normalize("/", j, out, outsz);
        } else {
            snprintf(out, outsz, "%s", rest);
        }
        return m->mount[best].fs;
    }
    snprintf(out, outsz, "%s", nsr);
    return fs;
}

/* Resolve for MOUNT and friends: cwd, chroot and namespace, but deliberately
 * NOT the mount table. A mountpoint is a name in the process's view of the
 * world; running it through the mount table would rewrite /mnt/dev back into
 * /dev on the underlying disk and mount it over the wrong place. */
static void resolve_ns(Proc *p, const char *in, char *out, size_t outsz)
{
    char abs[NOM_PATH_MAX * 2], rooted[NOM_PATH_MAX * 2];
    vfs_normalize(p->info ? p->info->cwd : "/", in, abs, sizeof abs);
    const char *root = (p->info && p->info->root[0]) ? p->info->root : "/";
    if (strcmp(root, "/") != 0) snprintf(rooted, sizeof rooted, "%s%s", root, abs);
    else                        snprintf(rooted, sizeof rooted, "%s", abs);
    if (p->info) ns_resolve(&p->info->ns, rooted, out, outsz);
    else         snprintf(out, outsz, "%s", rooted);
}

/* The common case: callers that only want the path, on the machine's own
 * current root filesystem. */
static void resolve(Proc *p, const char *in, char *out, size_t outsz)
{
    resolve_fs(p, in, out, outsz);
}

/* ------------------------------------------------------------- /proc ----
 * Synthesised from the process table, never read off the disk -- exactly as
 * on a real system. Corrupting the customer's filesystem therefore cannot
 * forge a process, and /proc stays trustworthy when everything else is not.
 * That is a property worth having in a game about deciding what to believe.
 */

static bool proc_split(const char *path, int *pid, char *leaf, size_t leafsz,
                       Proc *self)
{
    if (strncmp(path, "/proc", 5) != 0) return false;
    if (path[5] != '/' ) { *pid = -1; leaf[0] = 0; return path[5] == 0; }
    const char *p = path + 6;
    if (strncmp(p, "self", 4) == 0 && (p[4] == 0 || p[4] == '/')) {
        *pid = self ? self->pid : 1;
        p += 4;
    } else {
        int v = 0; bool any = false;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); any = true; }
        if (!any) return false;
        *pid = v;
    }
    if (*p == '/') p++;
    snprintf(leaf, leafsz, "%s", p);
    return true;
}

static ProcInfo *proc_by_pid(Machine *m, int pid)
{
    for (int i = 0; i < m->nproc; i++)
        if (m->proc[i].pid == pid) return &m->proc[i];
    return NULL;
}

/* Fill `out` with the contents of a /proc file. Returns false if there is no
 * such file. */
static bool proc_read(Machine *m, Proc *self, const char *path, Buf *out)
{
    int pid; char leaf[64];
    /* /proc/version -- WHAT IS RUNNING, not what is installed.
     *
     * `uname` had the version compiled into it, so a machine booted from a
     * valid image of the wrong version reported the right one and the fault
     * became undiagnosable with the tool the catalogue recommended for it.
     * The loader writes what it loaded; this is where a program reads it. */
    if (strcmp(path, "/proc/version") == 0) {
        buf_printf(out, "NomnixOS kernel %s rv64\n",
                   m->booted_kver[0] ? m->booted_kver : "(unknown)");
        return true;
    }
    if (!proc_split(path, &pid, leaf, sizeof leaf, self)) return false;
    if (pid < 0) return false;                    /* /proc itself is a dir */
    ProcInfo *pi = proc_by_pid(m, pid);
    if (!pi) return false;

    if (strcmp(leaf, "status") == 0) {
        buf_printf(out, "name %s\n", pi->name);
        buf_printf(out, "pid %d\n", pi->pid);
        buf_printf(out, "ppid %d\n", pi->ppid);
        buf_printf(out, "state %s\n", pi->alive ? "running" : "exited");
        buf_printf(out, "exit %lld\n", (long long)pi->exit_code);
        buf_printf(out, "instructions %llu\n", (unsigned long long)pi->icount);
        return true;
    }
    if (strcmp(leaf, "cmdline") == 0) {
        buf_printf(out, "%s%s%s\n", pi->name, pi->arg[0] ? " " : "", pi->arg);
        return true;
    }
    if (strcmp(leaf, "cwd") == 0) { buf_printf(out, "%s\n", pi->cwd); return true; }
    if (strcmp(leaf, "ns") == 0)  { ns_print(&pi->ns, out); return true; }
    return false;
}

static const char *PROC_FILES[] = { "status", "cmdline", "cwd", "ns", NULL };

/* A shared library satisfies a requirement when it is AT LEAST the version
 * asked for. This was string equality, which meant a binary needing 2.38
 * failed against an installed 2.41 -- backwards from every real dynamic
 * linker, where a newer libc is a superset of the older one and the whole
 * point of symbol versioning is that upgrading does not break what is already
 * built. A playtester who administers Linux for a living spotted it in one
 * line of output and said so. The fault it belongs to is a DOWNGRADE. */
static bool version_older(const char *have, const char *want)
{
    int hmaj = 0, hmin = 0, wmaj = 0, wmin = 0;
    if (sscanf(have, "%d.%d", &hmaj, &hmin) < 1) return false;
    if (sscanf(want, "%d.%d", &wmaj, &wmin) < 1) return false;
    if (hmaj != wmaj) return hmaj < wmaj;
    return hmin < wmin;
}

static bool link_check(Machine *m, Vfs *fs, const char *needs,
                       char *err, size_t errsz);
static int64_t spawn_fail(Buf *console, char *err, size_t errsz, int64_t code,
                          const char *fmt, ...);
static int64_t kernel_syscall(Cpu *c, int64_t n, int64_t a0, int64_t a1,
                              int64_t a2, void *ctx);

/* Read a NUL-terminated string out of guest memory, bounded. */
static bool guest_str(Cpu *c, uint64_t addr, char *out, size_t outsz)
{
    for (size_t i = 0; i < outsz; i++) {
        uint8_t ch;
        if (!cpu_read(c, addr + i, &ch, 1)) return false;
        out[i] = (char)ch;
        if (!ch) return true;
    }
    return false;              /* unterminated: the guest is corrupt */
}

static int alloc_fd(Proc *p)
{
    for (int i = 3; i < FD_MAX; i++) if (!p->fd[i].used) return i;
    return -1;
}

/* A directory you cannot enter hides everything under it, however healthy
 * those files are. This is the one fault class `pkg verify` structurally
 * cannot see: no manifest lists a directory, so every file inside reports as
 * pristine while nothing can read them. A playtester called the game
 * recipe-following because verify names the broken file every time; a fault
 * verify is blind to is the answer to that, and this one is a mistake real
 * administrators make constantly with a careless recursive chmod.
 *
 * Returns the first ancestor that bars the way, or NULL. */
static VNode *dir_barred(Vfs *fs, const char *path)
{
    char acc[NOM_PATH_MAX * 2];
    size_t al = 0;
    acc[al++] = '/';
    acc[al] = 0;
    /* Every component except the last: the leaf's own mode is the caller's
     * business, the path TO it is ours. */
    const char *q = path;
    while (*q == '/') q++;
    while (*q) {
        const char *e = q;
        while (*e && *e != '/') e++;
        if (!*e) break;                      /* last component: not a parent */
        size_t seg = (size_t)(e - q);
        if (al + seg + 2 >= sizeof acc) return NULL;
        if (al > 1) acc[al++] = '/';
        memcpy(acc + al, q, seg);
        al += seg;
        acc[al] = 0;
        VNode *d = vfs_lookup(fs, acc);
        if (d && d->kind == VN_DIR && !(d->mode & 0111)) return d;
        q = e + 1;
    }
    return NULL;
}

static int64_t sys_open(Proc *p, Cpu *c, uint64_t pathp, int64_t flags)
{
    char raw[NOM_PATH_MAX], path[NOM_PATH_MAX];
    if (!guest_str(c, pathp, raw, sizeof raw)) return -1;
    Vfs *fs = resolve_fs(p, raw, path, sizeof path);

    /* /proc is generated, read-only, and not on any disk. */
    {
        Buf pb = {0};
        if (proc_read(p->m, p, path, &pb)) {
            int pfd = alloc_fd(p);
            if (pfd < 0) { buf_free(&pb); return -1; }
            Fd *pf = &p->fd[pfd];
            memset(pf, 0, sizeof *pf);
            pf->used = true;
            snprintf(pf->path, sizeof pf->path, "%s", path);
            buf_put(&pf->data, pb.p, pb.len);
            buf_free(&pb);
            return pfd;
        }
    }

    if (dir_barred(fs, path)) return -1;

    bool dangling = false;
    VNode *n = vfs_resolve(fs, path, &dangling);
    if (!n && (flags & O_CREAT)) {
        /* O_CREAT creates the FILE, never the directories above it. The vfs
         * walk has mkdir -p semantics because the installer needs them, and
         * open inherited that by accident: deleting /var/log did nothing at
         * all, because syslogd's first O_CREAT quietly put it back. A real
         * open returns ENOENT and the daemon dies, which is the whole reason
         * a missing directory is a fault worth having. */
        const char *slash = strrchr(path, '/');
        if (slash && slash != path) {
            char parent[NOM_PATH_MAX * 2];
            size_t pl = (size_t)(slash - path);
            if (pl >= sizeof parent) return -1;
            memcpy(parent, path, pl);
            parent[pl] = 0;
            VNode *pd = vfs_lookup(fs, parent);
            if (!pd || pd->kind != VN_DIR) return -1;
            /* AND THE DIRECTORY HAS TO BE WRITABLE. Creating a file is a
             * write to the directory, not to the file, which is why a
             * security sweep that takes the write bit off /run stops every
             * daemon on the machine while every file in it still reads
             * perfectly. Without this the mode on a directory meant nothing
             * except for traversal, and "permissions on the directory rather
             * than on the file" is one of the classic afternoons. */
            if (!(pd->mode & 0222)) return -1;
        }
        /* AND ARE THERE ANY INODES LEFT. A filesystem with free space and no
         * free inodes refuses to create anything at all, which is the whole
         * point of the fault: df says there is room. */
        if (fs == &p->m->disk && p->m->fs_inodes_max &&
            machine_inodes_used(p->m) >= p->m->fs_inodes_max)
            return -1;
        n = vfs_mkfile(fs, path, "");
        if (!n) return -1;
    }
    if (!n || dangling) return -1;
    if (n->kind == VN_DIR) return -1;
    /* Mode is enforced for read and write, not just execute. There is no uid
     * model yet, so there is no root override -- which means `chmod 000` on a
     * config file really does break the thing that reads it, and that is a
     * fault worth being able to have. */
    bool want_write = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0;
    /* A read-only root refuses every write, which is the point: nothing is
     * corrupt, every hash matches, and the machine still cannot run. The
     * failure is a cascade -- each daemon fails at the moment it first tries
     * to write its state -- and the cause is one word in one line of fstab. */
    if (want_write && !p->m->on_rescue && p->m->root_ro && fs == &p->m->disk) return -1;
    /* AND THE RESCUE MEDIUM REALLY IS READ-ONLY, which its own banner has
     * been claiming while accepting writes.
     *
     *   rescue: live system, read-only medium
     *   rescue# ed /etc/hostname 1c "WROTE-TO-READONLY" . w
     *   ed: /etc/hostname: 1 line(s), 18 bytes written.
     *
     * That is the founding rule broken at the worst moment: a player
     * repairing a machine edits what they think is the customer's file,
     * is told it was written, and has changed a live image that vanishes on
     * the next boot. The medium is a stamped disc; it does not take writes,
     * and being refused is what sends somebody to `mount /dev/sda1 /mnt` --
     * the procedure the banner already prints and which really works, since
     * a mounted path resolves to the customer's filesystem and not to this
     * one.
     *
     * /tmp and /run stay writable because a live system carries a tmpfs for
     * exactly them, and the rescue boot's own services write their state
     * there. */
    if (want_write && p->m->on_rescue && fs == &p->m->rescue &&
        strncmp(path, "/tmp", 4) != 0 && strncmp(path, "/run", 4) != 0)
        return -1;
    if (!want_write && !(n->mode & 0444)) return -1;
    if (want_write  && !(n->mode & 0222) && !(flags & O_CREAT)) return -1;

    int fd = alloc_fd(p);
    if (fd < 0) return -1;
    Fd *f = &p->fd[fd];
    memset(f, 0, sizeof *f);
    f->used = true;
    f->writable = (flags & (O_WRONLY | O_RDWR)) != 0;
    f->fs = fs;
    snprintf(f->path, sizeof f->path, "%s", path);
    /* RUNBOOK: A DEVICE IS ITS CALLBACK, not its stored bytes.
     *
     * vfs_mkdev and the DevRead/DevWrite hooks were already here -- nom.h
     * even documents the Plan 9 shape they are for -- but nothing in the
     * guest kernel ever called them. sys_open copied n->data, which for a
     * device is permanently empty, so `cat /dev/ship/hull` opened
     * successfully and printed nothing at all. Devices existed for host code
     * and were invisible from the machine itself.
     *
     * Reading one snapshots it here, at open, which is what a synthetic file
     * means: the value is whatever it was when you opened it, and reading it
     * again means opening it again. That is how /proc behaves and it is what
     * a shell loop does anyway. */
    if (n->kind == VN_DEV && !want_write) {
        if (vfs_read(fs, path, &f->data) != IO_OK) {
            buf_free(&f->data);
            memset(f, 0, sizeof *f);
            return -1;
        }
    } else if (!(flags & O_TRUNC))
        buf_put(&f->data, n->data.p, n->data.len);
    if (flags & O_APPEND) f->pos = f->data.len;
    return fd;
}

static int64_t sys_read(Proc *p, Cpu *c, int64_t fd, uint64_t buf, int64_t len)
{
    if (fd == 0) {                                 /* stdin, from a pipe */
        if (!p->stdin_from || len < 0) return 0;
        size_t left = p->stdin_from->len > p->stdin_pos
                    ? p->stdin_from->len - p->stdin_pos : 0;
        size_t n = (size_t)len < left ? (size_t)len : left;
        if (n && !cpu_write(c, buf, p->stdin_from->p + p->stdin_pos, n)) return -1;
        p->stdin_pos += n;
        return (int64_t)n;
    }
    if (fd < 3 || fd >= FD_MAX || !p->fd[fd].used || len < 0) return -1;
    Fd *f = &p->fd[fd];
    size_t left = f->data.len > f->pos ? f->data.len - f->pos : 0;
    size_t n = (size_t)len < left ? (size_t)len : left;
    if (n && !cpu_write(c, buf, f->data.p + f->pos, n)) return -1;
    f->pos += n;
    return (int64_t)n;
}

static int64_t sys_write(Proc *p, Cpu *c, int64_t fd, uint64_t buf, int64_t len)
{
    if (len < 0 || len > (1 << 20)) return -1;
    char *tmp = nom_alloc((size_t)len + 1);
    if (!cpu_read(c, buf, tmp, (size_t)len)) { nom_free(tmp); return -1; }

    if (fd == 1 || fd == 2) {
        /* A captured stdout goes to the pipe. stderr always goes to the
         * console, because an error message that vanishes into a pipe is how
         * you lose an afternoon. */
        if (fd == 1 && p->capture && p->capture_into)
            buf_put(p->capture_into, tmp, (size_t)len);
        else if (p->console)
            buf_put(p->console, tmp, (size_t)len);
        nom_free(tmp);
        return len;
    }
    if (fd < 3 || fd >= FD_MAX || !p->fd[fd].used || !p->fd[fd].writable) {
        nom_free(tmp);
        return -1;
    }
    Fd *f = &p->fd[fd];
    if (strncmp(f->path, "/proc", 5) == 0) { nom_free(tmp); return -1; }
    /* ENOSPC. A full disk is not a corruption and no amount of verifying will
     * find it: every file is exactly what it should be, there is simply
     * nowhere to put the next one. */
    if (p->m->fs_capacity && f->fs == &p->m->disk) {
        uint64_t used = machine_disk_used(p->m);
        if (used + (uint64_t)len > p->m->fs_capacity) {
            nom_free(tmp);
            return -1;
        }
    }
    if (f->pos != f->data.len) buf_clear(&f->data);   /* no seeking yet */
    buf_put(&f->data, tmp, (size_t)len);
    f->pos = f->data.len;
    nom_free(tmp);
    return len;
}

static int64_t sys_close(Proc *p, int64_t fd)
{
    if (fd < 3 || fd >= FD_MAX || !p->fd[fd].used) return -1;
    Fd *f = &p->fd[fd];
    if (f->writable) {
        Vfs *wfs = f->fs ? f->fs : &p->m->disk;
        VNode *n = vfs_lookup(wfs, f->path);
        if (n && n->kind == VN_FILE) {
            buf_clear(&n->data);
            buf_put(&n->data, f->data.p, f->data.len);
        } else if (n && n->kind == VN_DEV) {
            /* RUNBOOK: and a write to a device is delivered on close, so
             * `echo 3 > /dev/ship/rooms/shields/power` arrives once, whole,
             * rather than a byte at a time. See the note in sys_open. */
            int rc = vfs_write(wfs, f->path, f->data.p ? f->data.p : "", f->data.len);
            buf_free(&f->data);
            memset(f, 0, sizeof *f);
            return rc == IO_OK ? 0 : -1;
        }
    }
    buf_free(&f->data);
    memset(f, 0, sizeof *f);
    return 0;
}

static int64_t sys_readdir(Proc *p, Cpu *c, uint64_t pathp, int64_t idx,
                           uint64_t buf, int64_t len)
{
    char raw[NOM_PATH_MAX], path[NOM_PATH_MAX], name[NOM_NAME_MAX];
    if (!guest_str(c, pathp, raw, sizeof raw)) return -1;
    Vfs *fs = resolve_fs(p, raw, path, sizeof path);

    /* /proc listings come from the process table */
    {
        int pid; char leaf[64];
        if (proc_split(path, &pid, leaf, sizeof leaf, p)) {
            if (pid < 0) {                        /* ls /proc -> the pids */
                if (idx < 0 || idx >= p->m->nproc) return -1;
                snprintf(name, sizeof name, "%d", p->m->proc[idx].pid);
            } else {                              /* ls /proc/N -> its files */
                if (!proc_by_pid(p->m, pid)) return -1;
                int n = 0;
                while (PROC_FILES[n]) n++;
                if (idx < 0 || idx >= n) return -1;
                snprintf(name, sizeof name, "%s", PROC_FILES[idx]);
            }
            size_t nl = strlen(name);
            if ((int64_t)nl + 1 > len) return -1;
            return cpu_write(c, buf, name, nl + 1) ? (int64_t)nl : -1;
        }
    }

    VNode *d = vfs_resolve(fs, path, NULL);
    if (!d || d->kind != VN_DIR) return -1;
    int64_t i = 0;
    for (VNode *k = d->child; k; k = k->next, i++) {
        if (i != idx) continue;
        size_t nl = strlen(k->name);
        if ((int64_t)nl + 1 > len) return -1;
        if (!cpu_write(c, buf, k->name, nl + 1)) return -1;
        return (int64_t)nl;
    }
    return -1;                                     /* past the end */
}

static int64_t sys_stat(Proc *p, Cpu *c, uint64_t pathp, uint64_t sbuf)
{
    char raw[NOM_PATH_MAX], path[NOM_PATH_MAX];
    if (!guest_str(c, pathp, raw, sizeof raw)) return -1;
    Vfs *fs = resolve_fs(p, raw, path, sizeof path);

    {
        int pid; char leaf[64];
        if (proc_split(path, &pid, leaf, sizeof leaf, p)) {
            NomStat st; memset(&st, 0, sizeof st);
            st.mode = 0555;
            if (pid < 0 || !leaf[0]) { st.kind = NOM_KIND_DIR; }
            else {
                Buf pb = {0};
                if (!proc_read(p->m, p, path, &pb)) { buf_free(&pb); return -1; }
                st.kind = NOM_KIND_FILE;
                st.size = (int64_t)pb.len;
                buf_free(&pb);
            }
            return cpu_write(c, sbuf, &st, sizeof st) ? 0 : -1;
        }
    }

    VNode *ln = vfs_lookup(fs, path);
    if (!ln) return -1;
    NomStat st;
    memset(&st, 0, sizeof st);
    if (ln->kind == VN_LINK) {
        /* stat follows the link; a dangling one is a genuine failure and the
         * guest is entitled to see it as one. */
        bool dangling = false;
        VNode *t = vfs_resolve(fs, path, &dangling);
        if (!t || dangling) return -1;
        ln = t;
    }
    st.mode = (int32_t)ln->mode;
    st.size = (int64_t)ln->data.len;
    st.kind = ln->kind == VN_DIR  ? NOM_KIND_DIR
            : ln->kind == VN_DEV  ? NOM_KIND_DEV
            : ln->kind == VN_LINK ? NOM_KIND_LINK : NOM_KIND_FILE;
    if (!cpu_write(c, sbuf, &st, sizeof st)) return -1;
    return 0;
}

static int64_t sys_getarg(Proc *p, Cpu *c, uint64_t buf, int64_t len)
{
    size_t n = strlen(p->arg);
    if ((int64_t)n + 1 > len) return -1;
    if (!cpu_write(c, buf, p->arg, n + 1)) return -1;
    return (int64_t)n;
}

/* ------------------------------------------------------------- syscall -- */

static int64_t kernel_syscall(Cpu *c, int64_t n, int64_t a0, int64_t a1,
                              int64_t a2, void *ctx)
{
    Proc *p = (Proc *)ctx;
    switch (n) {
    case SYS_write:   return sys_write(p, c, a0, (uint64_t)a1, a2);
    case SYS_read:    return sys_read (p, c, a0, (uint64_t)a1, a2);
    case SYS_close:   return sys_close(p, a0);
    case SYS_open:    return sys_open (p, c, (uint64_t)a0, a1);
    case SYS_readdir: return sys_readdir(p, c, (uint64_t)a0, a1, (uint64_t)a2,
                                         (int64_t)NOM_NAME_MAX);
    case SYS_stat:    return sys_stat (p, c, (uint64_t)a0, (uint64_t)a1);
    case SYS_getarg:  return sys_getarg(p, c, (uint64_t)a0, a1);
    case SYS_getpid:  return p->pid;
    case SYS_bind: {
        char t[NOM_PATH_MAX], at[NOM_PATH_MAX];
        if (!guest_str(c, (uint64_t)a0, t, sizeof t)) return -1;
        if (!guest_str(c, (uint64_t)a1, at, sizeof at)) return -1;
        if (!p->info) return -1;
        char ta[NOM_PATH_MAX * 2], aa[NOM_PATH_MAX * 2];
        vfs_normalize(p->info->cwd, t, ta, sizeof ta);
        vfs_normalize(p->info->cwd, at, aa, sizeof aa);
        return ns_bind(&p->info->ns, ta, aa, NULL, 0) ? 0 : -1;
    }
    case SYS_unbind: {
        char at[NOM_PATH_MAX];
        if (!guest_str(c, (uint64_t)a0, at, sizeof at)) return -1;
        if (!p->info) return -1;
        char aa[NOM_PATH_MAX * 2];
        vfs_normalize(p->info->cwd, at, aa, sizeof aa);
        return ns_unbind(&p->info->ns, aa) ? 0 : -1;
    }
    case SYS_chdir: {
        char raw[NOM_PATH_MAX], path[NOM_PATH_MAX];
        if (!guest_str(c, (uint64_t)a0, raw, sizeof raw)) return -1;
        Vfs *cfs = resolve_fs(p, raw, path, sizeof path);
        int pid; char leaf[64];
        bool isproc = proc_split(path, &pid, leaf, sizeof leaf, p);
        VNode *d = isproc ? NULL : vfs_resolve(cfs, path, NULL);
        if (!isproc && (!d || d->kind != VN_DIR)) return -1;
        if (p->info) {
            char abs[NOM_PATH_MAX * 2];
            vfs_normalize(p->info->cwd, raw, abs, sizeof abs);
            snprintf(p->info->cwd, sizeof p->info->cwd, "%s", abs);
        }
        return 0;
    }
    case SYS_getcwd: {
        const char *cw = p->info ? p->info->cwd : "/";
        size_t n = strlen(cw);
        if ((int64_t)n + 1 > a1) return -1;
        return cpu_write(c, (uint64_t)a0, cw, n + 1) ? (int64_t)n : -1;
    }
    case SYS_chmod: {
        char raw[NOM_PATH_MAX], path[NOM_PATH_MAX];
        if (!guest_str(c, (uint64_t)a0, raw, sizeof raw)) return -1;
        Vfs *mfs = resolve_fs(p, raw, path, sizeof path);
        VNode *n = vfs_lookup(mfs, path);
        if (!n) return -1;
        n->mode = (unsigned)(a1 & 0777);
        return 0;
    }
    case SYS_dns: {
        /* A real query, over UDP, to whatever address /etc/resolv.conf
         * names. It can time out, and when it does it has really waited. */
        char name[128], ip[64];
        if (!guest_str(c, (uint64_t)a0, name, sizeof name)) return -1;
        if (!netsite_dns(p->m, name, ip, sizeof ip)) return -1;
        size_t n = strlen(ip);
        if ((int64_t)n + 1 > a2) return -1;
        return cpu_write(c, (uint64_t)a1, ip, n + 1) ? (int64_t)n : -1;
    }
    case SYS_http: {
        /* A real connection: a three-way handshake, a request line, a status
         * line, a body, and a teardown. It takes an ADDRESS and never a
         * name, which is what keeps "works by ip, not by name" reachable. */
        char ip[64], path[NOM_PATH_MAX];
        if (!guest_str(c, (uint64_t)a0, ip, sizeof ip)) return -1;
        if (!guest_str(c, (uint64_t)a1, path, sizeof path)) return -1;
        Buf b = {0};
        int64_t r = -1;
        if (netsite_http(p->m, ip, path, &b) && b.len < (1u << 16) &&
            cpu_write(c, (uint64_t)a2, b.p, b.len + 1))
            r = (int64_t)b.len;
        buf_free(&b);
        return r;
    }
    case SYS_netinfo: {
        /* Everything the machine can be shown about its own network, as
         * text. Read out of the running stack, never out of a config: an
         * address here is one that was really configured, and a neighbour
         * here is one that really answered. */
        Buf b = {0};
        netsite_info(p->m, (int)a0, &b);
        int64_t r = -1;
        if ((int64_t)b.len + 1 <= a2 && cpu_write(c, (uint64_t)a1, b.p ? b.p : "", b.len + 1))
            r = (int64_t)b.len;
        buf_free(&b);
        return r;
    }
    case SYS_netctl:
        switch ((int)a0) {
        case NETCTL_FWCLEAR: netsite_fw_clear(p->m); return 0;
        case NETCTL_FWADD:
            netsite_fw_add(p->m, (int)((a1 >> 24) & 0xff), (int)((a1 >> 16) & 0xff),
                           (int)(a1 & 0xffff), (int)a2);
            return 0;
        case NETCTL_TRACE:   netsite_trace(p->m, (int)a1); return 0;
        case NETCTL_PCAP:    netsite_pcap(p->m, (int)a1); return 0;
        case NETCTL_ARPDEL: {
            /* The address arrives as text, because that is what the tool was
             * typed with and parsing it in one place -- the stack's own
             * parser -- is what stops `arp -d 10.0.2.300` becoming a
             * delete of something else. */
            char a[64];
            if (!guest_str(c, (uint64_t)a1, a, sizeof a)) return -1;
            return netsite_arp_del(p->m, a);
        }
        default: return -1;
        }
    case SYS_traceroute: {
        char dst[64];
        if (!guest_str(c, (uint64_t)a0, dst, sizeof dst)) return -1;
        Buf b = {0};
        netsite_traceroute(p->m, dst, &b);
        int64_t r = -1;
        if ((int64_t)b.len + 1 <= a2 && cpu_write(c, (uint64_t)a1, b.p ? b.p : "", b.len + 1))
            r = (int64_t)b.len;
        buf_free(&b);
        return r;
    }
    case SYS_ping: {
        char dst[64];
        if (!guest_str(c, (uint64_t)a0, dst, sizeof dst)) return -1;
        int rtt = 0;
        int r = netsite_ping(p->m, dst, &rtt);
        if (a1) cpu_write(c, (uint64_t)a1, &rtt, sizeof rtt);
        return r;
    }
    case SYS_mkdir: {
        /* mkdir, and mkdir -p when a1 is set.
         *
         * vfs's own walk has -p semantics because the installer needs them,
         * so plain mkdir has to check the parent HERE -- otherwise
         * `mkdir /var/lgo/journal` would cheerfully invent /var/lgo too and
         * a typo would leave the player with a directory tree that looks
         * right and is not. */
        char raw[NOM_PATH_MAX], path[NOM_PATH_MAX];
        if (!guest_str(c, (uint64_t)a0, raw, sizeof raw)) return -1;
        Vfs *fs = resolve_fs(p, raw, path, sizeof path);
        if (!path[0] || strcmp(path, "/") == 0) return -1;
        if (strncmp(path, "/proc", 5) == 0) return -1;   /* generated, not a disk */
        VNode *there = vfs_lookup(fs, path);
        /* -p is the flag that means "make sure this exists", so an existing
         * directory is a success and not an error. Without -p it is an
         * error, which is what makes mkdir usable as a lock. */
        if (there) return (a1 && there->kind == VN_DIR) ? 0 : -1;
        if (!p->m->on_rescue && p->m->root_ro && fs == &p->m->disk) return -1;
        /* The medium is stamped: no new directories on it either. See the
         * note beside the same test in sys_open. */
        if (p->m->on_rescue && fs == &p->m->rescue &&
            strncmp(path, "/tmp", 4) != 0 && strncmp(path, "/run", 4) != 0)
            return -1;

        const char *slash = strrchr(path, '/');
        if (slash && slash != path) {
            char parent[NOM_PATH_MAX];
            size_t pl = (size_t)(slash - path);
            if (pl >= sizeof parent) return -1;
            memcpy(parent, path, pl);
            parent[pl] = 0;
            VNode *pd = vfs_lookup(fs, parent);
            if (!pd && !a1) return -1;                   /* no -p: no invention */
            if (pd && pd->kind != VN_DIR) return -1;
            /* Creating a directory is a write to the one above it. */
            if (pd && !(pd->mode & 0222)) return -1;
        }
        /* A directory is an inode like any other, and a filesystem out of
         * inodes cannot make one however much space df reports. */
        if (fs == &p->m->disk && p->m->fs_inodes_max &&
            machine_inodes_used(p->m) >= p->m->fs_inodes_max)
            return -1;
        return vfs_mkdir(fs, path) ? 0 : -1;
    }
    case SYS_dfused: {
        /* 0 bytes used, 1 bytes total, 2 inodes used, 3 inodes total */
        /* CAN THIS MACHINE SEE THAT FILESYSTEM AT ALL.
         *
         * These four answered about the customer's disk unconditionally, so
         * after `umount /mnt` on the rescue medium `df` printed
         * "/dev/sda1 1035K 523K 511K 50%" and then, four lines lower under
         * its own second header, "(nothing mounted)". Two halves of one
         * command disagreeing about the same filesystem, in one screenful.
         *
         * From the rescue medium the disk is a device, not a filesystem,
         * until somebody mounts it. Then there is nothing to measure, and
         * "nothing to measure" is an answer -- an unqualified number is not. */
        Machine *m = p->m;
        bool reachable = !m->on_rescue;
        for (int i = 0; !reachable && i < m->nmount; i++)
            if (m->mount[i].used && m->mount[i].fs == &m->disk) reachable = true;
        if (!reachable) return -1;
        switch ((int)a0) {
        case 1:  return (int64_t)p->m->fs_capacity;
        case 2:  return (int64_t)machine_inodes_used(p->m);
        case 3:  return (int64_t)p->m->fs_inodes_max;
        default: return (int64_t)machine_disk_used(p->m);
        }
    }
    case SYS_kill: {
        /* Leave the signal pending on the target. Nothing is interrupted --
         * there is no preemption here -- so a daemon sees it the next time it
         * looks, which is exactly what a cooperative system can promise and
         * exactly enough for "re-read your configuration". */
        struct Daemon *ds = (struct Daemon *)p->m->daemon;
        if (!ds) return -1;
        for (int i = 0; i < p->m->ndaemon; i++) {
            if (!ds[i].running) continue;
            if (ds[i].proc.info && ds[i].proc.info->pid == (int)a0) {
                ds[i].pending_sig = (int)a1;
                return 0;
            }
        }
        return -1;
    }
    case SYS_sigpend: {
        struct Daemon *ds = (struct Daemon *)p->m->daemon;
        if (!ds) return 0;
        for (int i = 0; i < p->m->ndaemon; i++) {
            if (&ds[i].proc != p) continue;
            int sig = ds[i].pending_sig;
            ds[i].pending_sig = 0;
            return sig;
        }
        return 0;
    }
    case SYS_pipe: {
        /* One stage of a pipeline. The child reads what this process's pipe
         * currently holds and writes into a fresh buffer, which then becomes
         * the pipe -- so the next stage reads this one's output. */
        char path[NOM_PATH_MAX], arg[NOM_ARG_MAX] = "";
        if (!guest_str(c, (uint64_t)a0, path, sizeof path)) return SPAWN_ENOENT;
        if (a1 && !guest_str(c, (uint64_t)a1, arg, sizeof arg)) return SPAWN_ENOENT;
        Buf next = {0};
        int64_t rc = kernel_spawn_piped(p->m, path, arg, p->console, p->depth + 1,
                                        p, &p->pipe, &next);
        buf_free(&p->pipe);
        p->pipe = next;
        return rc;
    }
    case SYS_pipeout:
        if (p->console && p->pipe.len) buf_put(p->console, p->pipe.p, p->pipe.len);
        buf_clear(&p->pipe);
        return 0;
    case SYS_piperead: {
        /* The other end of SYS_pipe: give the guest what the last stage
         * wrote, oldest first, and forget it. `>` on a real program and
         * `$(...)` are both this call plus somewhere to put the bytes. */
        if (a1 <= 0) return -1;
        size_t n = p->pipe.len < (size_t)a1 ? p->pipe.len : (size_t)a1;
        if (!n) return 0;
        if (!cpu_write(c, (uint64_t)a0, p->pipe.p, n)) return -1;
        memmove(p->pipe.p, p->pipe.p + n, p->pipe.len - n);
        p->pipe.len -= n;
        return (int64_t)n;
    }
    /* THE RUNBOOK BRIDGE. See the note on SYS_rbapi in abi.h -- this is the
     * only edit made to NOMINAL's kernel, and it is nine lines.
     *
     * The host installs rb_api_hook; when nothing has (a machine booted by a
     * gate with no world behind it), the call fails rather than inventing an
     * answer, because a script that gets a plausible reply from a game that
     * is not there is worse than one that gets an error. */
    case SYS_rbapi: {
        char line[NOM_ARG_MAX];
        if (!guest_str(c, (uint64_t)a0, line, sizeof line)) return -1;
        if (!rb_api_hook) return -1;
        /* A CALL INTO THE SHIP COSTS CPU, and it has to, or the budget means
         * nothing.
         *
         * The host does all the work of a command -- rendering the ship,
         * parsing the line, changing the world -- and the guest pays about
         * four instructions to ask for it. So a `while True: ship()` loop ran
         * three and a half THOUSAND times inside a single tick's budget,
         * which is not a script running, it is a script with the brakes off:
         * a player could poll the ship faster than the ship exists.
         *
         * Two thousand instructions per call is a syscall that feels like a
         * syscall. A control loop at one bar of computer gets about fifty a
         * tick, which is far more than any sane script needs and far less
         * than a spin loop wants. It is also why a tighter script is worth
         * more power, which is the whole economy. */
        c->charge += 2000;
        Buf resp;
        buf_init(&resp);
        rb_api_hook(p->m, line, &resp);
        size_t n = resp.len;
        if ((int64_t)n + 1 > a2) n = (size_t)(a2 > 0 ? a2 - 1 : 0);
        bool ok = cpu_write(c, (uint64_t)a1, resp.p ? resp.p : "", n) &&
                  cpu_write(c, (uint64_t)a1 + n, "", 1);
        buf_free(&resp);
        return ok ? (int64_t)n : -1;
    }

    case SYS_setvar: {
        char nm[32], val[192];
        if (!guest_str(c, (uint64_t)a0, nm, sizeof nm)) return -1;
        if (!guest_str(c, (uint64_t)a1, val, sizeof val)) return -1;
        if (!p->info || !nm[0]) return -1;
        ProcInfo *pi = p->info;
        for (int i = 0; i < pi->nvar; i++) {
            if (strcmp(pi->var[i].name, nm) != 0) continue;
            /* An empty value UNSETS, which is what `X=` means. */
            if (!val[0]) { pi->var[i] = pi->var[--pi->nvar]; return 0; }
            snprintf(pi->var[i].val, sizeof pi->var[i].val, "%s", val);
            return 0;
        }
        if (!val[0]) return 0;
        if (pi->nvar >= VAR_MAX) return -1;
        snprintf(pi->var[pi->nvar].name, sizeof pi->var[0].name, "%s", nm);
        snprintf(pi->var[pi->nvar].val,  sizeof pi->var[0].val,  "%s", val);
        pi->nvar++;
        return 0;
    }
    case SYS_getvar: {
        char nm[32];
        if (!guest_str(c, (uint64_t)a0, nm, sizeof nm)) return -1;
        if (!p->info) return -1;
        for (int i = 0; i < p->info->nvar; i++) {
            if (strcmp(p->info->var[i].name, nm) != 0) continue;
            size_t n = strlen(p->info->var[i].val);
            if ((int64_t)n + 1 > a2) return -1;
            return cpu_write(c, (uint64_t)a1, p->info->var[i].val, n + 1)
                 ? (int64_t)n : -1;
        }
        return -1;
    }
    case SYS_svcstart: {
        char path[NOM_PATH_MAX], name[64] = "";
        if (!guest_str(c, (uint64_t)a0, path, sizeof path)) return -1;
        if (a1) guest_str(c, (uint64_t)a1, name, sizeof name);
        char rp[NOM_PATH_MAX];
        resolve(p, path, rp, sizeof rp);
        return kernel_start_daemon(p->m, rp, "", name[0] ? name : path,
                                   (int)a2, p->console,
                                   p->info ? &p->info->ns : NULL);
    }
    case SYS_svcctl: {
        char nm[64];
        if (!guest_str(c, (uint64_t)a1, nm, sizeof nm)) return -1;
        if ((int)a0 == SVCCTL_STOP)   return kernel_svc_stop(p->m, nm);
        if ((int)a0 == SVCCTL_RELOAD) return kernel_svc_reload(p->m, nm, p->console);
        return -1;
    }
    case SYS_fsck: {
        char dev[64];
        if (!guest_str(c, (uint64_t)a0, dev, sizeof dev)) return -1;
        Buf b = {0};
        int r = machine_fsck(p->m, dev, &b);
        if (b.len < (size_t)a2 && !cpu_write(c, (uint64_t)a1, b.p, b.len + 1)) r = -1;
        buf_free(&b);
        return r;
    }
    case SYS_bootsec:
        /* a0 != 0 writes one, which is what zbl-install does */
        /* AND POINTS THE FIRMWARE AT THAT DISK. grub-install rewrites the
         * firmware's boot entry as well as the sector, for the reason it
         * matters here: a machine whose boot order names an empty optical
         * drive has a perfect disk and nothing to boot, and installing the
         * loader without telling the firmware where it went would be half a
         * job. `rcon boot disk` is the other way to say it. */
        if (a0) { p->m->bootsector = true; p->m->sp_bootdev = 0; }
        return p->m->bootsector ? 1 : 0;
    case SYS_rootuuid: {
        const char *u = p->m->root_uuid;
        size_t n = strlen(u);
        if ((int64_t)n + 1 > a1) return -1;
        return cpu_write(c, (uint64_t)a0, u, n + 1) ? (int64_t)n : -1;
    }
    case SYS_unlink: {
        char raw[NOM_PATH_MAX], path[NOM_PATH_MAX];
        if (!guest_str(c, (uint64_t)a0, raw, sizeof raw)) return -1;
        Vfs *ufs = resolve_fs(p, raw, path, sizeof path);
        return vfs_remove(ufs, path) ? 0 : -1;
    }
    case SYS_readlink: {
        char raw[NOM_PATH_MAX], path[NOM_PATH_MAX];
        if (!guest_str(c, (uint64_t)a0, raw, sizeof raw)) return -1;
        Vfs *lfs = resolve_fs(p, raw, path, sizeof path);
        VNode *n = vfs_lookup(lfs, path);
        if (!n || n->kind != VN_LINK) return -1;
        size_t tl = strlen(n->target);
        if ((int64_t)tl + 1 > a2) return -1;
        return cpu_write(c, (uint64_t)a1, n->target, tl + 1) ? (int64_t)tl : -1;
    }
    case SYS_mount: {
        char dev[64], raw[NOM_PATH_MAX], at[NOM_PATH_MAX];
        if (!guest_str(c, (uint64_t)a0, dev, sizeof dev)) return -1;
        if (!guest_str(c, (uint64_t)a1, raw, sizeof raw)) return -1;
        resolve_ns(p, raw, at, sizeof at);
        char rdev[NOM_PATH_MAX];
        if ((int)a2 & MNT_BIND) { resolve_ns(p, dev, rdev, sizeof rdev);
                                  snprintf(dev, sizeof dev, "%s", rdev); }
        return machine_mount(p->m, dev, at, (int)a2) ? 0 : -1;
    }
    case SYS_umount: {
        char raw[NOM_PATH_MAX], at[NOM_PATH_MAX];
        if (!guest_str(c, (uint64_t)a0, raw, sizeof raw)) return -1;
        resolve_ns(p, raw, at, sizeof at);
        return machine_umount(p->m, at) ? 0 : -1;
    }
    case SYS_chroot: {
        char raw[NOM_PATH_MAX], path[NOM_PATH_MAX];
        if (!guest_str(c, (uint64_t)a0, raw, sizeof raw)) return -1;
        /* The one escape hatch: step back out to the real root. A process
         * cannot normally leave its chroot, but the session is a person at a
         * rescue console and they must be able to put the disk down. */
        if (strcmp(raw, "//LEAVE") == 0) {
            if (!p->info || !p->info->root[0] ||
                strcmp(p->info->root, "/") == 0) return -1;
            p->info->root[0] = '\0';
            snprintf(p->info->cwd, sizeof p->info->cwd, "/");
            return 0;
        }
        Vfs *rfs = resolve_fs(p, raw, path, sizeof path);
        VNode *d = vfs_resolve(rfs, path, NULL);
        if (!d || d->kind != VN_DIR) return -1;
        if (!p->info) return -1;
        /* chroot is relative to the CURRENT root, so chrooting twice nests
         * rather than escaping -- which is the property that makes it a
         * containment tool and not just a path prefix. */
        char abs[NOM_PATH_MAX * 2], newroot[NOM_PATH_MAX * 2];
        vfs_normalize(p->info->cwd, raw, abs, sizeof abs);
        const char *cur = p->info->root[0] ? p->info->root : "/";
        if (strcmp(cur, "/") == 0) snprintf(newroot, sizeof newroot, "%s", abs);
        else snprintf(newroot, sizeof newroot, "%s%s", cur, abs);
        /* Can a shell actually RUN in there? On a real machine chroot execs
         * the target's shell, so a disk whose libc is broken refuses at the
         * door and you are never trapped. Here the session respawns /bin/sh
         * for every command, so entering such a chroot left the player unable
         * to run anything at all -- including `exit`, which is a builtin but
         * never got the chance to be one. A playtester was stuck until they
         * guessed `rescue`.
         *
         * Checking up front keeps the good half of that discovery: you still
         * learn that this disk is too broken to chroot into, which is exactly
         * why `pkg --root` exists. You just learn it as an error instead of a
         * dead end. */
        {
            /* Resolve the way every other path is resolved -- cwd, chroot,
             * namespace and MOUNT TABLE. Reaching straight into the raw Vfs
             * looked equivalent and was not: /mnt/bin/sh does not exist in
             * the rescue medium's own tree, it exists in the disk mounted at
             * /mnt, so the check refused every chroot on every machine and
             * quietly cost three of sixty solves. */
            /* THREE DIFFERENT ANSWERS, AND THEY WERE ALL ONE ERROR CODE.
             *
             * "no shell present" and "the shell will not link" both returned
             * -2, and the shell prints the libc story for -2 -- so `chroot
             * /mnt` with NOTHING mounted and /mnt empty said "there is a
             * /bin/sh in there, and it cannot run -- its libraries are
             * missing or the wrong version". There was no /bin/sh in there.
             *
             * A broken linker is a real fault class in this game, so that
             * sentence sends the player hunting a library problem that does
             * not exist, from a machine state that has no evidence in it at
             * all. The refusal was right and the reason was invented. */
            char shraw[NOM_PATH_MAX * 2], shpath[NOM_PATH_MAX * 2];
            snprintf(shraw, sizeof shraw, "%s/bin/sh", raw);
            Vfs *sfs = resolve_fs(p, shraw, shpath, sizeof shpath);
            VNode *sh = vfs_resolve(sfs, shpath, NULL);
            if (!sh || sh->kind != VN_FILE) {
                /* Is anything mounted at the target? That is the difference
                 * between "you forgot to mount the disk" -- overwhelmingly the
                 * common case, and the one the player can fix in one command
                 * -- and "you mounted something that is not a root
                 * filesystem". */
                for (int i = 0; i < p->m->nmount; i++)
                    if (p->m->mount[i].used &&
                        strcmp(p->m->mount[i].at, abs) == 0) return -4;
                return -3;
            }
            char lerr[NOM_ERR_MAX] = "", shneeds[512] = "";
            if (cpu_elf_needs((const uint8_t *)sh->data.p, sh->data.len,
                              shneeds, sizeof shneeds) &&
                !link_check(p->m, sfs, shneeds, lerr, sizeof lerr))
                return -2;
        }
        snprintf(p->info->root, sizeof p->info->root, "%s", newroot);
        snprintf(p->info->cwd, sizeof p->info->cwd, "/");
        return 0;
    }
    case SYS_reboot: {
        /* The machine restarts itself. `init 6` reached this by accident of
         * the runlevel code and `reboot` did not exist at all, which is
         * backwards -- nobody types `init 6` first. */
        Machine *m = p->m;
        if (a0) {
            kernel_stop_daemons(m);
            m->boot.running = false;
            buf_clear(&m->boot.console);
            buf_puts(&m->boot.console, "[halted]\n");
            return 0;
        }
        if (m->on_rescue) machine_boot_rescue(m);
        else              machine_boot(m);
        return 0;
    }

    case SYS_sp: {
        /* THE SERVICE PROCESSOR of the machine this one can reach.
         *
         * This is the whole remote-hands workflow in one syscall. The
         * technician's workstation runs `rcon`, which lands here, and the
         * target is p->m->peer -- a separate Machine with its own disk, cpu
         * and boot state. Nothing about the target's health matters: a
         * service processor is a small computer on the motherboard that is up
         * whether or not the host is, which is exactly why you can fix a box
         * that will not boot.
         *
         * The console is the target's own boot output, unchanged. It is not
         * summarised or interpreted here, because a console that editorialises
         * is not a console. */
        Machine *t = p->m->peer;
        if (!t) return -1;
        /* An air-gapped machine has nothing listening. The technician finds
         * this out the way they do in life: by trying. */
        if (t->airgapped) return -3;
        int op = (int)a0, arg = (int)a1;
        switch (op) {
        case SP_STATUS:
            /* Bit 0 is POWER, bit 4 is "it got all the way up". They are
             * different facts and the console must not merge them. */
            /* Bit 5 is WHAT IS RUNNING RIGHT NOW, which is not the same fact
             * as bit 3 (what it boots NEXT time) and is the one that was
             * missing. `rcon status` could say "media empty / boot the disk"
             * with the rescue image live, and be reporting both of its own
             * bits honestly. The medium and the boot device are the target's
             * own state now, so all four come from one machine. */
            return (t->powered ? 1 : 0)
                 | (t->boot.running ? 16 : 0)
                 | (p->m->sp_connected ? 2 : 0)
                 | (t->sp_media ? 4 : 0)
                 | (t->sp_bootdev ? 8 : 0)
                 | (t->on_rescue ? 32 : 0);
        case SP_CONNECT: {
            /* THE ADDRESS IS PART OF THE QUESTION.
             *
             * This attached to whatever the technician typed, so a mistyped
             * digit put them on the customer's console anyway and a whole
             * ticket could be worked against an address that was never
             * theirs. A service processor at 9.9.9.9 either exists or does
             * not, and the one at the address on the sticker is the only one
             * that answers.
             *
             * It is a different refusal from the air-gapped one above: there
             * is NO ROUTE to a machine that is on no network, and there is
             * nothing LISTENING at an address that is not theirs. A player
             * who cannot tell those two apart cannot tell "I typed it wrong"
             * from "this ticket is the hard kind". */
            char want[64] = "";
            if (a2 && !guest_str(c, (uint64_t)a2, want, sizeof want)) return -1;
            if (want[0] && p->m->peer_addr[0] &&
                strcmp(want, p->m->peer_addr) != 0) return -5;
            p->m->sp_connected = true;
            return 0;
        }
        case SP_POWER:
            if (arg == 0) {                       /* off */
                kernel_stop_daemons(t);
                t->powered = false;
                t->boot.running = false;
                buf_clear(&t->boot.console);
                buf_puts(&t->boot.console,
                    "[powered off -- the screen is black and the fans have "
                    "stopped]\n");
                return 0;
            }
            if (arg == 1 && t->powered) {
                /* Already on. Real hardware ignores the power-on request. */
                return -4;
            }
            /* on, or cycle: boot whatever the boot device says */
            if (t->sp_bootdev == 1 && t->sp_media) machine_boot_rescue(t);
            else                                   machine_boot(t);
            return 0;
        case SP_MEDIA:
            t->sp_media = (arg != 0);
            /* Emptying the drive cannot leave the firmware pointed at it.
             * It could, and then `rcon boot disk` looked like the step that
             * had not worked when the step that had not worked was earlier. */
            if (!t->sp_media) t->sp_bootdev = 0;
            return 0;
        case SP_BOOTDEV:
            if (arg == 1 && !t->sp_media) return -2;   /* nothing in the drive */
            t->sp_bootdev = arg;
            return 0;
        case SP_CONSOLE: {
            size_t n = t->boot.console.len;
            if (n > 60000) n = 60000;
            if (!cpu_write(c, (uint64_t)a2, t->boot.console.p, n)) return -1;
            return (int64_t)n;
        }
        default: return -1;
        }
    }

    case SYS_svcinfo: {
        /* WHY IS THIS ONE UNHAPPY. `svc` could say running or DEAD and
         * nothing else, so on a machine that boots with a service quietly
         * down -- the class of ticket this game is most interested in --
         * there was no way to ask the follow-up question. The daemon record
         * has carried the answer all along. */
        char want[64];
        if (!guest_str(c, (uint64_t)a0, want, sizeof want)) return -1;
        Buf b = {0};
        bool found = false;
        for (int i = 0; i < p->m->ndaemon; i++) {
            struct Daemon *d = &p->m->daemon[i];
            if (strcmp(d->name, want) != 0) continue;
            found = true;
            buf_printf(&b, "service   %s\n", d->name);
            buf_printf(&b, "exec      %s\n", d->path);
            buf_printf(&b, "state     %s\n",
                       d->running ? "running"
                                  : d->gave_up ? "gave up after repeated failures"
                                               : "not running");
            buf_printf(&b, "restarts  %d\n", d->restarts);
            if (!d->running)
                buf_printf(&b, "exit      %lld\n", (long long)d->exit_code);
            if (d->died[0])
                buf_printf(&b, "last said %s\n", d->died);
            if (d->pending_sig)
                buf_printf(&b, "signal    %d pending\n", d->pending_sig);
            /* WHAT IT IS ACTUALLY LOOKING AT. A service started under a bind
             * reads a different file from the one you `cat`, everything
             * passes `pkg verify`, and the only evidence is the namespace it
             * was handed. That was reachable through `ns <pid>` and only if
             * you already suspected it -- and on a service that is DEAD there
             * is no pid left to ask. */
            for (int k = 0; k < d->ns.n; k++)
                buf_printf(&b, "%-9s %s is really %s\n", k ? "" : "namespace",
                           d->ns.b[k].at, d->ns.b[k].target);
            /* A service somebody stopped by hand said nothing on the way
             * down and left nothing in the log, so sending the player to
             * `dmesg -f` for it is an invitation to read an empty answer as
             * a missing one. */
            if (!d->running && strcmp(d->died, "stopped by hand") != 0)
                buf_printf(&b, "\nwhat it said on the way down is in the boot log:\n"
                               "  dmesg -f %s\n", d->name);
            else if (!d->running)
                buf_printf(&b, "\nnothing failed: it was stopped from a shell.\n"
                               "  svc start %s\n", d->name);
            break;
        }
        if (!found) return 0;
        int64_t r = -1;
        if ((int64_t)b.len < a2 && cpu_write(c, (uint64_t)a1, b.p, b.len))
            r = (int64_t)b.len;
        buf_free(&b);
        return r;
    }

    case SYS_restore: {
        char pkg[64], raw[NOM_PATH_MAX], path[NOM_PATH_MAX];
        if (!guest_str(c, (uint64_t)a0, pkg, sizeof pkg)) return -1;
        if (!guest_str(c, (uint64_t)a1, raw, sizeof raw)) return -1;
        /* Through --root, so a rescue session repairs the mounted disk. */
        resolve_fs(p, raw, path, sizeof path);
        return pkg_restore_path(p->m, pkg, path) ? 0 : -1;
    }

    case SYS_fstype: {
        char dev[64];
        if (!guest_str(c, (uint64_t)a0, dev, sizeof dev)) return -1;
        const char *t = device_type(p->m, dev);
        /* fstab names the root by UUID, not by device, so a probe that only
         * understood /dev/... could never check the one line that matters. */
        if (!t && strncmp(dev, "UUID=", 5) == 0)
            t = strcmp(dev + 5, p->m->root_uuid) == 0 ? "ext4" : NULL;
        if (!t) return -1;
        size_t tl = strlen(t);
        if (tl > (size_t)a2) tl = (size_t)a2;
        if (!cpu_write(c, (uint64_t)a1, t, tl)) return -1;
        return (int64_t)tl;
    }

    case SYS_needs: {
        /* ldd's one job. The dependency list is read out of the ELF the same
         * way the loader reads it, so ldd cannot disagree with what actually
         * happens when you run the thing -- which is the only property that
         * makes ldd worth trusting on a real system too. */
        char raw[NOM_PATH_MAX], path[NOM_PATH_MAX];
        if (!guest_str(c, (uint64_t)a0, raw, sizeof raw)) return -1;
        Vfs *fs = resolve_fs(p, raw, path, sizeof path);
        VNode *n = vfs_resolve(fs, path, NULL);
        if (!n || n->kind != VN_FILE) return -1;
        char needs[512] = "";
        if (!cpu_elf_needs((const uint8_t *)n->data.p, n->data.len,
                           needs, sizeof needs))
            return 0;                      /* a valid file with no needs */
        size_t nl = strlen(needs);
        if (nl > (size_t)a2) nl = (size_t)a2;
        if (!cpu_write(c, (uint64_t)a1, needs, nl)) return -1;
        return (int64_t)nl;
    }

    case SYS_mounts: {
        Buf b = {0};
        /* THE ROOT FILESYSTEM IS MOUNTED, AND THIS TABLE LEFT IT OUT.
         *
         * df prints the space on /dev/sda1 in its top half and then, four
         * lines lower, a mount table of "none on /proc" and "none on /tmp" --
         * so one command said the root disk is 53% full and implied in the
         * same screenful that it is not mounted anywhere. `du -s /` agrees
         * with the top half to the byte, so the numbers were right and the
         * table was wrong.
         *
         * The root mount is not in m->mount[] because nothing in userland
         * performed it: the initrd did, before there was a userland. That is
         * a fact about how it got there and not about whether it is there.
         * `mount` reads this same call, so both were quietly short one line.
         *
         * Which device it is depends on which medium came up, and the machine
         * already knows -- the same flag df uses to decide whether the
         * customer's disk is reachable at all. */
        buf_printf(&b, "%s on / (%s)\n",
                   p->m->on_rescue ? "/dev/sr0" : "/dev/sda1",
                   p->m->on_rescue ? "iso9660" : "ext4");
        for (int i = 0; i < p->m->nmount; i++) {
            if (!p->m->mount[i].used) continue;
            buf_printf(&b, "%s on %s%s\n", p->m->mount[i].dev,
                       p->m->mount[i].at,
                       p->m->mount[i].sub[0] ? " (bind)" : "");
        }
        int64_t r = -1;
        if ((int64_t)b.len < a1 && cpu_write(c, (uint64_t)a0, b.p, b.len))
            r = (int64_t)b.len;
        buf_free(&b);
        return r;
    }
    case SYS_repo: {
        char pkg[64], path[NOM_PATH_MAX];
        /* Re-read the channel from the disk on every fetch, because the
         * player can edit the repo file and the next fetch must honour it. */
        machine_read_channel(p->m);
        if (!guest_str(c, (uint64_t)a0, pkg, sizeof pkg)) return -1;
        if (!guest_str(c, (uint64_t)a1, path, sizeof path)) return -1;
        Buf b = {0};
        bool ok = pkg_file_content(p->m, pkg, path, &b);
        int64_t r = -1;
        if (ok) {
            /* a2 is the buffer; its size is fixed by the ABI at 64k */
            if (b.len <= (1u << 16) && cpu_write(c, (uint64_t)a2, b.p, b.len))
                r = (int64_t)b.len;
        }
        buf_free(&b);
        return r;
    }
    case SYS_spawn: {
        char path[NOM_PATH_MAX], arg[NOM_ARG_MAX] = "";
        if (!guest_str(c, (uint64_t)a0, path, sizeof path)) return SPAWN_ENOENT;
        if (a1 && !guest_str(c, (uint64_t)a1, arg, sizeof arg)) return SPAWN_ENOENT;
        return kernel_spawn_p(p->m, path, arg, p->console, p->depth + 1, p, NULL, 0);
    }
    case SYS_exit:
        c->exit_code = a0;
        c->trap = TRAP_EXIT;
        return 0;
    default:
        return -1;                 /* unknown syscalls fail; they never crash */
    }
}

/* ---------------------------------------------------------------- spawn -- */

/* Say why a program could not be run, on the console, the way a loader does.
 * Without this a nested spawn fails silently and the player is left with the
 * last thing that DID work, which is evidence pointing at the wrong file. */
static int64_t spawn_fail(Buf *console, char *err, size_t errsz, int64_t code,
                          const char *fmt, ...)
{
    va_list ap;
    char line[NOM_ERR_MAX];
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (err && errsz) snprintf(err, errsz, "%s", line);
    if (console) { buf_puts(console, line); buf_putc(console, '\n'); }
    return code;
}

int64_t kernel_spawn_as(Machine *m, const char *path, const char *arg,
                        Buf *console, int depth, Proc *parent,
                        ProcInfo *as, char *err, size_t errsz);

/* MAKE ROOM IN THE PROCESS TABLE, WITHOUT LOSING TRACK OF THE DAEMONS.
 *
 * The table is finite and a real session runs hundreds of commands; when it
 * filled, a new process got no record at all and silently lost its chroot and
 * its namespace, quietly working on the wrong filesystem. Exited processes go
 * oldest-first, keeping the session (ppid -1) and anything still running.
 *
 * The records MOVE when that happens, and a running daemon holds a pointer
 * straight into this array. It was only ever compacted from spawn, where the
 * daemons had all been started during a boot that had the table nearly to
 * itself, so nothing had gone visibly wrong yet -- but `svc start` starts
 * services in the middle of a session now, hundreds of commands in, and a
 * daemon whose record moved under it reports another process's cpu and marks
 * the wrong pid dead when it exits. So the pointers are rebound by pid, which
 * is the only identity a process record has.
 */
static void reap_procs(Machine *m)
{
    int w = 0;
    for (int i = 0; i < m->nproc; i++) {
        ProcInfo *q = &m->proc[i];
        bool keep = q->alive || q->ppid == -1 || i >= m->nproc - PROC_MAX / 2;
        if (keep) m->proc[w++] = *q;
    }
    m->nproc = w;
    struct Daemon *ds = (struct Daemon *)m->daemon;
    for (int i = 0; ds && i < m->ndaemon; i++) {
        if (!ds[i].proc.info) continue;
        ds[i].proc.info = NULL;
        for (int j = 0; j < m->nproc; j++)
            if (m->proc[j].pid == ds[i].proc.pid) { ds[i].proc.info = &m->proc[j]; break; }
    }
}

/* Set on the next spawn, consumed by it. Threading two more parameters
 * through every caller of kernel_spawn_as would be worse than this, and the
 * whole thing is single-threaded. */
static Buf *g_next_stdin;
static Buf *g_next_capture;

int64_t kernel_spawn_piped(Machine *m, const char *path, const char *arg,
                           Buf *console, int depth, Proc *parent,
                           Buf *in, Buf *out)
{
    g_next_stdin = in;
    g_next_capture = out;
    int64_t rc = kernel_spawn_as(m, path, arg, console, depth, parent, NULL,
                                 NULL, 0);
    g_next_stdin = NULL;
    g_next_capture = NULL;
    return rc;
}

int64_t kernel_spawn_as(Machine *m, const char *path, const char *arg,
                        Buf *console, int depth, Proc *parent,
                        ProcInfo *as, char *err, size_t errsz)
{
    if (err && errsz) err[0] = '\0';
    if (depth > SPAWN_DEPTH)
        return spawn_fail(console, err, errsz, SPAWN_EDEPTH,
                          "%s: too many nested programs", path);

    /* The program name is looked up in the PARENT's namespace, because that
     * is who said it. A child that inherits a bind can be handed a path its
     * parent could not have resolved, and vice versa. */
    char rpath[NOM_PATH_MAX];
    Vfs *pfs;
    if (parent) {
        pfs = resolve_fs(parent, path, rpath, sizeof rpath);
    } else if (as) {
        Proc tmp; memset(&tmp, 0, sizeof tmp);
        tmp.m = m; tmp.info = as;
        pfs = resolve_fs(&tmp, path, rpath, sizeof rpath);
    } else {
        snprintf(rpath, sizeof rpath, "%s", path);
        pfs = m->on_rescue ? &m->rescue : &m->disk;
    }
    path = rpath;

    bool dangling = false;
    VNode *ln = vfs_lookup(pfs, path);
    VNode *n = vfs_resolve(pfs, path, &dangling);
    if (dangling)
        return spawn_fail(console, err, errsz, SPAWN_ENOENT,
                          "%s -> %s: no such file", path, ln ? ln->target : "?");
    if (!n)
        return spawn_fail(console, err, errsz, SPAWN_ENOENT, "%s: not found", path);
    if (n->kind != VN_FILE)
        return spawn_fail(console, err, errsz, SPAWN_ENOENT,
                          "%s: not a regular file", path);
    if (!(n->mode & 0111))
        return spawn_fail(console, err, errsz, SPAWN_EPERM,
                          "%s: permission denied (mode %04o)", path, n->mode);

    /* THE DYNAMIC LINKER. Before a single instruction runs, every library the
     * binary was linked against has to be present, on a path ld.so.conf names,
     * and at a compatible version. This is why a bad libc upgrade takes the
     * whole machine down at once -- including the tools you would reach for to
     * fix it, which is exactly why the rescue medium exists. */
    {
        char needs[512];
        if (cpu_elf_needs((const uint8_t *)n->data.p, n->data.len,
                          needs, sizeof needs)) {
            char lderr[NOM_ERR_MAX];
            if (!link_check(m, pfs, needs, lderr, sizeof lderr))
                return spawn_fail(console, err, errsz, SPAWN_ENOEXEC,
                                  "%s: %s", path, lderr);
        }
    }

    Cpu c;
    cpu_init(&c);
    char lerr[128] = "";
    if (!cpu_load_elf(&c, (const uint8_t *)n->data.p, n->data.len,
                      lerr, sizeof lerr)) {
        cpu_free(&c);
        return spawn_fail(console, err, errsz, SPAWN_ENOEXEC, "%s: %s", path, lerr);
    }

    /* Register the process. A pid is handed out even for a program that is
     * about to fail, because "pid 7 exited 1" is information the player wants
     * and a table that only lists successes is a lie. */
    Proc p;
    memset(&p, 0, sizeof p);
    p.m = m;
    p.console = console;
    p.depth = depth;
    snprintf(p.arg, sizeof p.arg, "%s", arg ? arg : "");
    /* Pipeline plumbing, set by kernel_spawn_piped and consumed here. */
    p.stdin_from   = g_next_stdin;
    p.capture_into = g_next_capture;
    p.capture      = (g_next_capture != NULL);

    if (!as && m->nproc >= PROC_MAX) reap_procs(m);

    ProcInfo *pi = as;
    if (as) {
        /* Running AS an existing process: this is what a shell session is.
         * cd and bind then change the session's own namespace, which is the
         * only way they can persist between commands. */
        snprintf(pi->name, sizeof pi->name, "%s", path);
        snprintf(pi->arg, sizeof pi->arg, "%s", arg ? arg : "");
        pi->alive = true;
        p.pid = pi->pid;
        p.info = pi;
    } else if (m->nproc < PROC_MAX) {
        pi = &m->proc[m->nproc++];
        memset(pi, 0, sizeof *pi);
        pi->pid  = m->next_pid ? m->next_pid++ : (m->next_pid = 2, 1);
        pi->ppid = parent ? parent->pid : 0;
        pi->alive = true;
        snprintf(pi->name, sizeof pi->name, "%s", path);
        snprintf(pi->arg, sizeof pi->arg, "%s", arg ? arg : "");
        /* A child inherits its parent's view of the world and may then change
         * its own copy. That is the whole of Plan 9 namespace inheritance. */
        if (parent && parent->info) {
            ns_copy(&pi->ns, &parent->info->ns);
            snprintf(pi->cwd, sizeof pi->cwd, "%s", parent->info->cwd);
            /* And the variables, because an environment that a child cannot
             * see is not an environment. */
            memcpy(pi->var, parent->info->var, sizeof pi->var);
            pi->nvar = parent->info->nvar;
            /* and the root: a child of a chrooted process is inside the same
             * chroot, which is the entire point of chroot */
            snprintf(pi->root, sizeof pi->root, "%s", parent->info->root);
        } else {
            ns_init(&pi->ns);
            snprintf(pi->cwd, sizeof pi->cwd, "/");
        }
        p.pid = pi->pid;
        p.info = pi;
    }

    c.syscall = kernel_syscall;
    c.ctx = &p;

    CpuTrap t;
    do {
        t = cpu_run(&c, 1000000);
    } while (t == TRAP_BUDGET && c.icount < PROC_BUDGET);

    int64_t rc;
    if (t == TRAP_EXIT) {
        rc = c.exit_code;
    } else if (t == TRAP_BUDGET) {
        rc = spawn_fail(console, err, errsz, SPAWN_EFAULT,
                        "%s: still running after %llu instructions -- killed",
                        path, (unsigned long long)c.icount);
    } else {
        /* A trap is the machine catching a program doing something impossible.
         * The pc matters: it is where in the binary the damage bit. */
        rc = spawn_fail(console, err, errsz, SPAWN_EFAULT,
                        "%s: %s at pc 0x%llx", path, cpu_trap_name(t),
                        (unsigned long long)c.pc);
    }

    for (int i = 0; i < FD_MAX; i++) if (p.fd[i].used) sys_close(&p, i);
    buf_free(&p.pipe);
    if (pi) {
        pi->alive = (as != NULL);      /* a session outlives its commands */
        pi->exit_code = (t == TRAP_EXIT) ? c.exit_code : rc;
        pi->icount = c.icount;
    }
    cpu_free(&c);
    return rc;
}

int64_t kernel_spawn_p(Machine *m, const char *path, const char *arg,
                       Buf *console, int depth, Proc *parent,
                       char *err, size_t errsz)
{
    return kernel_spawn_as(m, path, arg, console, depth, parent, NULL, err, errsz);
}

int64_t kernel_spawn(Machine *m, const char *path, const char *arg,
                     Buf *console, int depth, char *err, size_t errsz)
{
    return kernel_spawn_as(m, path, arg, console, depth, NULL, NULL, err, errsz);
}

/* The session: one long-lived process that a person is driving. Its namespace
 * and working directory persist between commands, because they belong to it
 * and not to the programs it runs. */
ProcInfo *kernel_session(Machine *m)
{
    for (int i = 0; i < m->nproc; i++)
        if (m->proc[i].ppid == -1) return &m->proc[i];
    if (m->nproc >= PROC_MAX) return NULL;
    ProcInfo *pi = &m->proc[m->nproc++];
    memset(pi, 0, sizeof *pi);
    pi->pid  = m->next_pid ? m->next_pid++ : (m->next_pid = 2, 1);
    pi->ppid = -1;                     /* marks it as the session */
    pi->alive = true;
    ns_init(&pi->ns);
    snprintf(pi->name, sizeof pi->name, "-sh");
    snprintf(pi->cwd, sizeof pi->cwd, "/");
    return pi;
}

int64_t kernel_run(Machine *m, const char *line, Buf *console)
{
    ProcInfo *ses = kernel_session(m);
    char err[NOM_ERR_MAX] = "";
    int64_t rc = kernel_spawn_as(m, "/bin/sh", line, console, 0, NULL, ses,
                                 err, sizeof err);
    /* Time passes while you work. Without this the daemons are frozen between
     * commands, so a signal sent with `kill -HUP` would sit pending forever
     * and a service could never die while you were looking at it. */
    kernel_tick(m, 2, console);
    return rc;
}

/* ------------------------------------------------------------ daemons --
 *
 * A service is not a program that runs and exits. It is a program that keeps
 * running, and everything interesting about services follows from that: it
 * can be running now and dead in ten minutes, it can be restarted, it can
 * respawn too fast, and `ps` can tell you which.
 *
 * There is no scheduler and no preemption. A daemon gets a slice of
 * instructions when the system ticks, and between slices its cpu and its
 * memory sit exactly where it left them. That is cooperative multitasking,
 * which is what this needs and a great deal less than a kernel.
 */

/* How much cpu a daemon gets to start up, and per tick afterwards. Starting
 * is generous because a daemon reads its config; running is not, because a
 * well-behaved one is mostly waiting. */
#define DAEMON_START_BUDGET  2000000ull
#define DAEMON_TICK_BUDGET     50000ull


/* How many times a service may come back before the system decides it is
 * never going to work. Real init systems all have a number like this and
 * they all print something close to the same sentence, because a daemon
 * that dies instantly and is restarted instantly is a machine that does
 * nothing else forever. */
#define RESPAWN_LIMIT 5

#define DAEMON_MAX 16

static struct Daemon *daemons(Machine *m)
{
    if (!m->daemon) {
        m->daemon = nom_alloc(sizeof(struct Daemon) * DAEMON_MAX);
        memset(m->daemon, 0, sizeof(struct Daemon) * DAEMON_MAX);
    }
    return (struct Daemon *)m->daemon;
}

/* Is this machine actually WORKING?
 *
 * Booting to a login prompt is not the same thing. A service that gave up
 * after five restarts leaves the machine up and useless in a specific way,
 * and the boot console scrolled past it twenty lines ago. Widening a ticket
 * from "will not boot" to "is not healthy" is what lets that be a ticket at
 * all -- and it is the commoner kind of call. */
/* The first line of a file that is not blank and not a comment. */
/* Does this unit's file ask to be started? `enabled: no` means it does not,
 * which is the whole point of `svc disable`, and the health check has to
 * respect it or disabling a service becomes a fault the player cannot undo. */
static bool unit_wants_to_run(Vfs *fs, const char *name)
{
    char path[NOM_PATH_MAX];
    snprintf(path, sizeof path, "/etc/services.d/%s.svc", name);
    VNode *n = vfs_resolve(fs, path, NULL);
    if (!n || n->kind != VN_FILE) return true;   /* no unit: judge by the boot */
    const char *p = n->data.p, *end = p + n->data.len;
    while (p && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (len >= 8 && memcmp(p, "enabled:", 8) == 0) {
            const char *v = p + 8;
            while (v < p + len && (*v == ' ' || *v == '\t')) v++;
            return !(v + 1 < p + len && (v[0] == 'n' || v[0] == 'N'));
        }
        if (!nl) break;
        p = nl + 1;
    }
    return true;
}


static bool first_real_line(Vfs *fs, const char *path, char *out, size_t outsz)
{
    VNode *n = vfs_resolve(fs, path, NULL);
    if (!n || n->kind != VN_FILE) return false;
    const char *p = n->data.p, *end = n->data.p + n->data.len;
    while (p && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *t = p; size_t tl = len;
        while (tl && (*t == ' ' || *t == '\t')) { t++; tl--; }
        while (tl && (t[tl-1] == ' ' || t[tl-1] == '\r')) tl--;
        if (tl && *t != '#') {
            if (tl >= outsz) tl = outsz - 1;
            memcpy(out, t, tl);
            out[tl] = '\0';
            return true;
        }
        p = nl ? nl + 1 : NULL;
    }
    return false;
}

/* IS THAT SERVICE ACTUALLY RUNNING RIGHT NOW.
 *
 * The network needs this and nothing else could answer it: kernel_health
 * reports what is WRONG, and a caller that wants one bit about one daemon had
 * to parse prose. netsite.c asks it about netd, because a machine whose
 * network daemon refused to start is a machine with no address -- and that
 * has to be read off the daemon table rather than guessed at from the config
 * file, since the whole point is that the file can be fine and the daemon can
 * still have died. */
bool kernel_svc_running(Machine *m, const char *name)
{
    if (!m || !m->daemon) return false;
    struct Daemon *d = daemons(m);
    for (int i = 0; i < m->ndaemon; i++)
        if (d[i].running && strcmp(d[i].name, name) == 0) return true;
    return false;
}

int kernel_health(Machine *m, Buf *out)
{
    if (!m->daemon) return 0;
    struct Daemon *d = daemons(m);
    Vfs *fs = m->on_rescue ? &m->rescue : &m->disk;
    int dead = 0;
    /* A daemon can be running and still wrong: the file on disk says one
     * thing and the process loaded another, because somebody edited it and
     * never reloaded. Nothing is corrupt, `pkg verify` is clean, and the
     * machine does not do what its configuration says it does. */
    for (int i = 0; i < m->ndaemon; i++) {
        if (!d[i].running) continue;
        char statepath[NOM_PATH_MAX];
        snprintf(statepath, sizeof statepath, "/run/%s.state", d[i].name);
        VNode *sn = vfs_resolve(fs, statepath, NULL);
        if (!sn || sn->kind != VN_FILE || !sn->data.len) continue;
        char confpath[NOM_PATH_MAX] = "", loaded[256] = "";
        const char *nl = memchr(sn->data.p, '\n', sn->data.len);
        if (!nl) continue;
        size_t l1 = (size_t)(nl - sn->data.p);
        if (l1 >= sizeof confpath) continue;
        memcpy(confpath, sn->data.p, l1); confpath[l1] = 0;
        const char *v = nl + 1;
        size_t vlen = sn->data.len - l1 - 1;
        while (vlen && (v[vlen-1] == '\n' || v[vlen-1] == '\r')) vlen--;
        if (vlen >= sizeof loaded) continue;
        memcpy(loaded, v, vlen); loaded[vlen] = 0;

        char ondisk[256];
        if (!first_real_line(fs, confpath, ondisk, sizeof ondisk)) continue;
        if (strcmp(ondisk, loaded) == 0) continue;
        if (!dead) buf_puts(out, "services that are not doing what they are configured to do:\n");
        buf_printf(out, "  %-14s running with a stale %s\n", d[i].name, confpath);
        buf_printf(out, "  %-14s   on disk:  %s\n", "", ondisk);
        buf_printf(out, "  %-14s   running: %s\n", "", loaded);
        dead++;
    }

    for (int i = 0; i < m->ndaemon; i++) {
        if (d[i].running) continue;
        /* A DISABLED UNIT IS ONE NOBODY EXPECTS TO RUN.
         *
         * This counted every stopped daemon as a fault, and did not know what
         * `enabled: no` means. So a machine that ships with postfix disabled --
         * which our own healthy workstation does -- was fine only while nobody
         * touched it, and became unfixable the moment somebody did.
         *
         * A playtester walked straight into it: `svc` showed postfix disabled
         * among ten running services and the customer said it sends her email,
         * so they enabled it, which is what the job asks for. `done` then
         * called the rewritten unit file damage. They put it back, and `done`
         * called postfix "stopped by hand". The only state the game accepted
         * was postfix RUNNING with its unit still saying `enabled: no` -- a
         * machine that silently loses mail at the next reboot -- and it closed
         * the ticket claiming every service that should be running was
         * running. Both halves false, and the correct play was the punished
         * one. */
        if (!unit_wants_to_run(fs, d[i].name)) continue;
        if (!dead) buf_puts(out, "services that should be running and are not:\n");
        buf_printf(out, "  %-14s %s\n", d[i].name,
                   d[i].gave_up ? "gave up after repeated failures"
                                : (d[i].died[0] ? d[i].died : "not running"));
        dead++;
    }
    return dead;
}

void kernel_stop_daemons(Machine *m)
{
    if (!m->daemon) return;
    struct Daemon *d = daemons(m);
    for (int i = 0; i < m->ndaemon; i++) if (d[i].running) cpu_free(&d[i].cpu);
    nom_free(m->daemon);
    m->daemon = NULL;
    m->ndaemon = 0;
}

static int64_t daemon_launch(Machine *m, struct Daemon *d, Buf *console);

int64_t kernel_start_daemon(Machine *m, const char *path, const char *arg,
                            const char *name, int restart, Buf *console,
                            const Ns *inherit)
{
    struct Daemon *ds = daemons(m);
    /* A SERVICE STARTED TWICE IS STILL ONE SERVICE.
     *
     * `svc start` and `svc restart` arrive here exactly the way the boot
     * does, and appending a second record for a name already in the table
     * would leave `svc status` answering from whichever it found first and
     * kernel_health counting the corpse of the old one for ever. A start of
     * something already known is THAT record starting again -- which is also
     * what makes a restart genuinely re-read the unit and the config from
     * disk, since everything about loading it happens below. */
    const char *want = name ? name : path;
    for (int i = 0; i < m->ndaemon; i++) {
        if (strcmp(ds[i].name, want) != 0) continue;
        if (ds[i].running) return SPAWN_EBUSY;
        struct Daemon *e = &ds[i];
        snprintf(e->path, sizeof e->path, "%s", path);
        snprintf(e->proc.arg, sizeof e->proc.arg, "%s", arg ? arg : "");
        e->restart_policy = restart;
        e->restarts = 0;
        e->gave_up = false;
        e->exit_code = 0;
        e->died[0] = '\0';
        e->pending_sig = 0;
        /* Its OWN namespace, not the caller's: a service comes back up where
         * it was, so a bind a unit made before it started is still under it.
         * Inheriting the shell's view instead would make `svc restart` a way
         * to quietly undo a namespace fault that is still on the disk. */
        return daemon_launch(m, e, console);
    }
    if (m->ndaemon >= DAEMON_MAX) return SPAWN_EDEPTH;
    struct Daemon *d = &ds[m->ndaemon];
    memset(d, 0, sizeof *d);
    if (inherit) ns_copy(&d->ns, inherit); else ns_init(&d->ns);
    snprintf(d->name, sizeof d->name, "%s", name ? name : path);
    snprintf(d->path, sizeof d->path, "%s", path);
    d->restart_policy = restart;
    snprintf(d->proc.arg, sizeof d->proc.arg, "%s", arg ? arg : "");
    m->ndaemon++;
    return daemon_launch(m, d, console);
}

/* Start, or restart, one daemon. Everything about loading and running it
 * lives here so that a restart is genuinely the same operation as a start --
 * a restart that took a different path would eventually diverge from it. */
static int64_t daemon_launch(Machine *m, struct Daemon *d, Buf *console)
{
    const char *path = d->path;

    Vfs *fs = m->on_rescue ? &m->rescue : &m->disk;
    bool dangling = false;
    VNode *n = vfs_resolve(fs, path, &dangling);
    char err[NOM_ERR_MAX] = "";
    if (dangling || !n)
        return spawn_fail(console, err, sizeof err, SPAWN_ENOENT, "%s: not found", path);
    if (n->kind != VN_FILE)
        return spawn_fail(console, err, sizeof err, SPAWN_ENOENT, "%s: not a regular file", path);
    if (!(n->mode & 0111))
        return spawn_fail(console, err, sizeof err, SPAWN_EPERM,
                          "%s: permission denied (mode %04o)", path, n->mode);

    {
        char needs[512];
        if (cpu_elf_needs((const uint8_t *)n->data.p, n->data.len, needs, sizeof needs)) {
            char lderr[NOM_ERR_MAX];
            if (!link_check(m, fs, needs, lderr, sizeof lderr))
                return spawn_fail(console, err, sizeof err, SPAWN_ENOEXEC,
                                  "%s: %s", path, lderr);
        }
    }

    cpu_init(&d->cpu);
    char lerr[128] = "";
    d->died[0] = '\0';
    if (!cpu_load_elf(&d->cpu, (const uint8_t *)n->data.p, n->data.len, lerr, sizeof lerr)) {
        cpu_free(&d->cpu);
        return spawn_fail(console, err, sizeof err, SPAWN_ENOEXEC, "%s: %s", path, lerr);
    }

    /* Register it in the process table like anything else -- INCLUDING the
     * part where a full table is made room in. A service started mid-session
     * got no record, so it was invisible to /proc: `svc` called it DEAD and
     * `netstat` dropped its port while the daemon sat there running. The
     * repair worked and every instrument on the machine said it had not. */
    if (m->nproc >= PROC_MAX) reap_procs(m);
    ProcInfo *pi = NULL;
    if (m->nproc < PROC_MAX) {
        pi = &m->proc[m->nproc++];
        memset(pi, 0, sizeof *pi);
        pi->pid = m->next_pid ? m->next_pid++ : (m->next_pid = 2, 1);
        pi->ppid = 1;
        pi->alive = true;
        snprintf(pi->name, sizeof pi->name, "%s", path);
        snprintf(pi->arg, sizeof pi->arg, "%s", d->proc.arg);
        ns_copy(&pi->ns, &d->ns);
        snprintf(pi->cwd, sizeof pi->cwd, "/");
    }

    char savearg[NOM_ARG_MAX];
    snprintf(savearg, sizeof savearg, "%s", d->proc.arg);
    memset(&d->proc, 0, sizeof d->proc);
    d->proc.m = m;
    d->proc.console = console;
    d->proc.info = pi;
    d->proc.pid = pi ? pi->pid : 0;
    snprintf(d->proc.arg, sizeof d->proc.arg, "%s", savearg);
    d->cpu.syscall = kernel_syscall;
    d->cpu.ctx = &d->proc;

    CpuTrap t = cpu_run(&d->cpu, DAEMON_START_BUDGET);
    if (t == TRAP_BUDGET) {
        /* Still going: that is a daemon. */
        d->running = true;
        if (pi) pi->icount = d->cpu.icount;
        return 0;
    }
    /* It finished during startup, which for a service means it fell over. */
    if (pi) { pi->alive = false; pi->exit_code = d->cpu.exit_code; pi->icount = d->cpu.icount; }
    d->exit_code = d->cpu.exit_code;
    if (t == TRAP_EXIT)
        snprintf(d->died, sizeof d->died, "exited immediately with status %lld",
                 (long long)d->cpu.exit_code);
    else
        snprintf(d->died, sizeof d->died, "%s at pc 0x%llx",
                 cpu_trap_name(t), (unsigned long long)d->cpu.pc);
    cpu_free(&d->cpu);

    /* It fell over on startup. Bring it back if it is allowed to come back,
     * and give up out loud once it is obviously never going to work. */
    if (d->restart_policy != 0) {
        if (++d->restarts >= RESPAWN_LIMIT) {
            d->gave_up = true;
            if (console)
                buf_printf(console,
                           "kernel: %s respawning too fast, giving up on it\n",
                           d->name);
            return SPAWN_EFAULT;
        }
        if (console)
            buf_printf(console, "kernel: %s died -- %s, restarting (%d)\n",
                       d->name, d->died, d->restarts);
        return daemon_launch(m, d, console);
    }
    return SPAWN_EFAULT;
}

void kernel_tick(Machine *m, int slices, Buf *console)
{
    if (!m->daemon) return;
    struct Daemon *d = daemons(m);
    for (int s = 0; s < slices; s++) {
        for (int i = 0; i < m->ndaemon; i++) {
            if (!d[i].running) continue;
            d[i].proc.console = console;
            CpuTrap t = cpu_run(&d[i].cpu, DAEMON_TICK_BUDGET);
            if (d[i].proc.info) d[i].proc.info->icount = d[i].cpu.icount;
            if (t == TRAP_BUDGET) continue;      /* still going, as expected */

            d[i].running = false;
            d[i].exit_code = d[i].cpu.exit_code;
            if (t == TRAP_EXIT)
                snprintf(d[i].died, sizeof d[i].died, "exited with status %lld",
                         (long long)d[i].cpu.exit_code);
            else
                snprintf(d[i].died, sizeof d[i].died, "%s at pc 0x%llx",
                         cpu_trap_name(t), (unsigned long long)d[i].cpu.pc);
            if (d[i].proc.info) {
                d[i].proc.info->alive = false;
                d[i].proc.info->exit_code = d[i].cpu.exit_code;
            }
            cpu_free(&d[i].cpu);
            if (d[i].restart_policy != 0 && !d[i].gave_up) {
                if (++d[i].restarts >= RESPAWN_LIMIT) {
                    d[i].gave_up = true;
                    if (console)
                        buf_printf(console, "kernel: %s respawning too fast, "
                                            "giving up on it\n", d[i].name);
                } else {
                    if (console)
                        buf_printf(console, "kernel: %s died -- %s, restarting (%d)\n",
                                   d[i].name, d[i].died, d[i].restarts);
                    daemon_launch(m, &d[i], console);
                }
            } else if (console) {
                buf_printf(console, "kernel: %s died -- %s\n", d[i].name, d[i].died);
            }
        }
    }
}

/* STOPPING ONE, AND ASKING ONE TO RE-READ ITSELF.
 *
 * Everything a running-and-wrong machine needs repairing with, short of the
 * power switch -- and the power switch is exactly what must not be needed,
 * because rebooting destroys the evidence for the whole fault class where a
 * process is out of step with a file.
 */
int kernel_svc_stop(Machine *m, const char *name)
{
    if (!m->daemon) return SVCCTL_ENOSVC;
    struct Daemon *d = daemons(m);
    for (int i = 0; i < m->ndaemon; i++) {
        if (strcmp(d[i].name, name) != 0) continue;
        if (!d[i].running) return SVCCTL_ENOTRUN;
        cpu_free(&d[i].cpu);
        d[i].running = false;
        d[i].gave_up = false;
        d[i].pending_sig = 0;
        d[i].exit_code = 0;
        snprintf(d[i].died, sizeof d[i].died, "stopped by hand");
        /* A UNIT THAT LOADED SOMETHING INTO THE KERNEL UNLOADS IT AGAIN.
         *
         * nft(8) is not a daemon that holds the ruleset in its own memory --
         * it loads it and exits into the filter, so killing the process used
         * to leave the rules in place, counting up, on a machine whose `svc`
         * said DEAD. The one lever the shell offered was cosmetic: a player
         * could watch `netstat -F` go from `matched 28` to `matched 48`
         * after stopping the filter. Two views of one machine disagreeing,
         * which is the thing this project does not allow.
         *
         * A real nftables unit has `ExecStop=nft flush ruleset` for exactly
         * this reason. Stopping the filter takes the filter off. */
        if (strstr(d[i].path, "nft")) netsite_fw_clear(m);
        /* Out of the process table too, which is what makes `ps` and
         * `netstat` agree with `svc` about it -- netstat lists a port only
         * while the process that opens it is alive, so stopping the web
         * server really does take :80 off the machine. */
        if (d[i].proc.info) {
            d[i].proc.info->alive = false;
            d[i].proc.info->exit_code = 0;
        }
        return 0;
    }
    return SVCCTL_ENOSVC;
}

int kernel_svc_reload(Machine *m, const char *name, Buf *console)
{
    if (!m->daemon) return SVCCTL_ENOSVC;
    struct Daemon *d = daemons(m);
    for (int i = 0; i < m->ndaemon; i++) {
        if (strcmp(d[i].name, name) != 0) continue;
        if (!d[i].running) return SVCCTL_ENOTRUN;
        d[i].pending_sig = SIG_HUP;
        /* Give it the slice it needs to notice. A signal here is left
         * pending and collected by a daemon that polls for it, so "does this
         * daemon support reload" has an answer nobody has to write down: the
         * ones that poll take it, and the ones that do not leave it sitting
         * there. Declaring the capability in the unit file would be a second
         * copy of that fact, free to be wrong. */
        kernel_tick(m, 2, console);
        if (d[i].pending_sig == SIG_HUP) {
            d[i].pending_sig = 0;      /* not left to go off later */
            return SVCCTL_ENOSIG;
        }
        return 0;
    }
    return SVCCTL_ENOSVC;
}

/* --------------------------------------------------------- the linker -- */

/* The search path, read from /etc/ld.so.conf. A library that is installed but
 * on a path nobody lists is a library that is not found -- which is a real
 * fault and a genuinely annoying one. */
static bool find_lib(Machine *m, Vfs *fs, const char *soname,
                     char *out, size_t outsz)
{
    Buf conf = {0};
    VNode *cn = vfs_resolve(fs, "/etc/ld.so.conf", NULL);
    if (cn && cn->kind == VN_FILE) buf_put(&conf, cn->data.p, cn->data.len);
    else buf_puts(&conf, "/lib\n/usr/lib\n");     /* the built-in default */

    const char *p = conf.p, *end = conf.p + conf.len;
    bool found = false;
    while (p && p < end && !found) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char dir[NOM_PATH_MAX];
        if (len && len < sizeof dir) {
            memcpy(dir, p, len);
            dir[len] = '\0';
            char cand[NOM_PATH_MAX];
            snprintf(cand, sizeof cand, "%s/%s", dir, soname);
            VNode *ln = vfs_resolve(fs, cand, NULL);
            if (ln && ln->kind == VN_FILE) {
                snprintf(out, outsz, "%s", cand);
                found = true;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    buf_free(&conf);
    return found;
}

/* A library declares its own version on its first line: "<stub> <name> <ver>".
 * Reading it out of the file is what makes an upgrade real -- replace the file
 * and every consumer sees the new number. */
static bool lib_version(Vfs *fs, const char *path, char *out, size_t outsz)
{
    VNode *n = vfs_resolve(fs, path, NULL);
    if (!n || n->kind != VN_FILE) return false;
    size_t len = n->data.len;
    const char *nl = memchr(n->data.p, '\n', len);
    if (nl) len = (size_t)(nl - n->data.p);
    /* the version is the last whitespace-separated word of the first line */
    size_t e = len;
    while (e && (n->data.p[e-1] == ' ' || n->data.p[e-1] == '\r')) e--;
    size_t b = e;
    while (b && n->data.p[b-1] != ' ') b--;
    if (b >= e) return false;
    size_t nl2 = e - b;
    if (nl2 >= outsz) nl2 = outsz - 1;
    memcpy(out, n->data.p + b, nl2);
    out[nl2] = '\0';
    return true;
}

static bool link_check(Machine *m, Vfs *fs, const char *needs,
                       char *err, size_t errsz)
{
    const char *p = needs;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[160];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = nl ? nl + 1 : p + strlen(p);

        char soname[80] = "", want[40] = "";
        if (sscanf(line, "%79s %39s", soname, want) < 1) continue;
        if (!soname[0]) continue;

        char path[NOM_PATH_MAX];
        if (!find_lib(m, fs, soname, path, sizeof path)) {
            snprintf(err, errsz,
                     "error while loading shared libraries: %s: "
                     "cannot open shared object file", soname);
            return false;
        }
        char have[40] = "";
        bool known = lib_version(fs, path, have, sizeof have);
        if (want[0] && known && version_older(have, want)) {
            snprintf(err, errsz,
                     "error while loading shared libraries: %s: "
                     "version %s not found (installed: %s)",
                     soname, want, have);
            return false;
        }

        /* A LIBC FROM THE RELEASE AFTER THIS ONE, which is the other way a
         * libc can be wrong and the one the documentation has been promising.
         *
         * A newer library satisfies an older requirement -- that is the whole
         * point of symbol versioning, and the linker above is right to allow
         * it. But 12.0's libc is not merely newer, it is built against a
         * kernel this machine is not running, and the first thing it does is
         * check. That is the real failure mode of a glibc dragged in from the
         * next release, down to the wording, and it is what makes the testing
         * channel a fault at all: without it, `channel = testing` installed a
         * perfectly working library, the machine came up healthy, and the
         * generator threw the ticket away and drew again. The fault existed,
         * was documented in three places, and could not be dealt -- it was
         * measured at zero in four hundred tickets.
         *
         * The repair is in /etc/pkg/repos.d, not in the file: reinstall libc
         * with the channel still wrong and the repository hands back the same
         * version, reported as restored. */
        if (known && strncmp(soname, "libc.so", 7) == 0 &&
            !version_older(have, "2.40")) {
            const char *kv = m->booted_kver[0] ? m->booted_kver : "6.4.11";
            if (version_older(kv, "7.0")) {
                snprintf(err, errsz,
                         "FATAL: kernel too old -- %s %s needs kernel 7.0 or "
                         "newer and this kernel is %s", soname, have, kv);
                return false;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------- capacity --
 *
 * Counted from the tree rather than tracked incrementally, because a running
 * total is a thing that drifts out of step with reality and this one has to
 * be trusted: it decides whether a write fails. */
uint64_t machine_disk_used(const Machine *m)
{
    uint64_t total = 0;
    VNode *stack[256];
    int sp = 0;
    if (m->disk.root) stack[sp++] = m->disk.root;
    while (sp) {
        VNode *n = stack[--sp];
        for (VNode *k = n->child; k; k = k->next) {
            if (k->kind == VN_DIR) { if (sp < 256) stack[sp++] = k; }
            else total += k->data.len;
        }
    }
    return total;
}

/* How many INODES are in use.
 *
 * A filesystem runs out of two things independently, and running out of the
 * second one is far more confusing than the first: `df` shows plenty of room,
 * every write fails anyway, and nothing is corrupt. It is a genuinely
 * different diagnosis from a full disk -- the tool that answers it is `df -i`
 * and nothing else will tell you -- which is exactly the kind of variety this
 * game needs more of.
 *
 * Directories count, as they do on a real filesystem. */
uint64_t machine_inodes_used(const Machine *m)
{
    uint64_t n = 0;
    VNode *stack[256];
    int sp = 0;
    if (m->disk.root) stack[sp++] = m->disk.root;
    while (sp) {
        VNode *d = stack[--sp];
        for (VNode *k = d->child; k; k = k->next) {
            n++;
            if (k->kind == VN_DIR && sp < 256) stack[sp++] = k;
        }
    }
    return n;
}

/* --------------------------------------------------------------- mount -- */

/* The block devices this machine has. The customer's disk is /dev/sda1 --
 * present as a device whether or not anything on it works, which is exactly
 * why you can rescue it. */
/* What a device really is, as opposed to what fstab claims it is. mount(8)
 * probes rather than trusting the file, which is why "wrong fs type" is a
 * distinct and very recognisable error rather than a mysterious failure. */
/* AN EMPTY DRIVE IS EMPTY. /dev/sr0 answered "iso9660" from a constant, so
 * `rcon media eject` said "virtual drive emptied" and `blkid` on that machine
 * went on reporting a medium in it -- the tool whose entire job is to probe
 * the device rather than believe a config file was the one believing a
 * constant. The removable device is present exactly when something is in it. */
static const char *device_type(const Machine *m, const char *dev)
{
    if (strcmp(dev, "/dev/sda1") == 0 || strcmp(dev, "/dev/sda") == 0)
        return "ext4";
    if (strcmp(dev, "/dev/sr0") == 0) return m->sp_media ? "iso9660" : NULL;
    return NULL;
}

/* BLKID FROM THE SERVICE PROCESSOR, ON A MACHINE THAT NEVER BOOTED.
 *
 * mountall stops the boot with
 *
 *   mountall: /etc/fstab:2: UUID=1b46-...: no device on this machine has that uuid
 *             `blkid` says what /dev/sda1 actually is.
 *
 * and until now the very next thing the player typed was refused, because
 * there is no shell on a machine that did not finish booting -- so the
 * console recommended a command the console had just made unreachable, and
 * the only route to the answer was the whole rescue-medium round trip.
 *
 * This is the one command a service processor really can answer without the
 * machine's cooperation. What blkid reports is not read off a filesystem: it
 * is the identity of the block device, which is exactly the kind of thing
 * iDRAC and iLO show you about a box that is sitting at a POST error. So it
 * is answered HERE, from the device table, and the reply says so -- it is
 * out-of-band information and the player should know that is what they are
 * looking at, because nothing else on that console is.
 *
 * Same device table, same probe as /sbin/blkid, so the two cannot disagree. */
void kernel_sp_blkid(Machine *m, Buf *out)
{
    const char *t1 = device_type(m, "/dev/sda1");
    const char *t2 = device_type(m, "/dev/sr0");
    buf_puts(out, "[service processor: the machine has no shell, so this is read\n"
                  " off the drives themselves -- it is the one thing out-of-band\n"
                  " management can answer while the machine is down]\n");
    if (t1) buf_printf(out, "/dev/sda1: UUID=\"%s\" TYPE=\"%s\"\n", m->root_uuid, t1);
    else    buf_puts(out, "/dev/sda1: the controller does not see a disk there\n");
    if (t2) buf_printf(out, "/dev/sr0:  TYPE=\"%s\"\n", t2);
    else    buf_puts(out, "/dev/sr0:  no medium (the drive is empty)\n");
}

/* THE REFUSAL ITSELF, IN ONE PLACE.
 *
 * Both front ends have to say this and they were saying it separately: the
 * socket printed it, the desktop returned an empty string, and a command that
 * answers with nothing on a machine that will not boot reads as "I looked and
 * the file is fine". Silence is the one answer this game may never give.
 *
 * It lives here so that a console cannot be honest in one window and mute in
 * another, which is the same rule as blkid above: one machine, one answer. */
/* WHAT A CONSOLE ON A MACHINE THAT NEVER BOOTED DOES WITH A COMMAND.
 *
 * Returns true if it handled the line. The refusal itself was already shared,
 * and the two front ends STILL disagreed, because the socket had learned an
 * exception the extension had not: blkid is answered by the service processor
 * reading the drives, so it must work on a box with no shell -- mountall's own
 * error names it as the next step. The desktop refused it and the socket
 * answered it. Sharing the WORDS but not the DECISION is not sharing.
 *
 * So the whole decision lives here, and a front end asks one question. */
bool kernel_console_dead(Machine *m, const char *cmd, Buf *out)
{
    if (m->boot.running) return false;
    if (strncmp(cmd, "blkid", 5) == 0 && (cmd[5] == 0 || cmd[5] == ' ')) {
        buf_puts(out, "\n");
        kernel_sp_blkid(m, out);
        return true;
    }
    kernel_no_shell(out);
    return true;
}


void kernel_no_shell(Buf *out)
{
    buf_puts(out,
        "\n[no shell here -- this machine did not finish booting]\n"
        "  the console shows what it managed to say. `rcon console` to\n"
        "  re-read it, `rcon media insert` + `rcon boot media` +\n"
        "  `rcon power cycle` to bring it up on the rescue medium.\n"
        "  `blkid` is the one command that still answers: the service\n"
        "  processor reads the drives without the machine's help.\n");
}

static Vfs *device_fs(Machine *m, const char *dev)
{
    if (strcmp(dev, "/dev/sda1") == 0 || strcmp(dev, "/dev/sda") == 0)
        return &m->disk;
    if (strcmp(dev, "/dev/sr0") == 0)
        return m->sp_media ? &m->rescue : NULL;
    return NULL;
}

bool machine_mount(Machine *m, const char *dev, const char *at, int flags)
{
    if (m->nmount >= MOUNT_MAX) return false;
    /* Mounting at / would shadow the running system with itself and make
     * every subsequent lookup nonsense. Real mount(8) allows it; here it is
     * only ever a mistake, and one that is very hard to see afterwards. */
    if (!at) return false;
    if (strcmp(at, "/") == 0) {
        /* The single exception. Real init mounts / read-only from the initrd
         * and then remounts it read-write once fsck is happy; an fstab that
         * says ro means that second step never happens and the machine comes
         * up unable to write to its own disk. */
        if (flags & MNT_RO) { m->root_ro = true; return true; }
        m->root_ro = false;
        return true;
    }
    if (!dev || !*dev) return false;

    Vfs *fs = NULL;
    char sub[NOM_PATH_MAX] = "";
    if (flags & MNT_BIND) {
        /* A bind mount grafts a subtree of the CURRENT root, which is how
         * `mount /dev /mnt/dev` works before you chroot. */
        fs = m->on_rescue ? &m->rescue : &m->disk;
        snprintf(sub, sizeof sub, "%s", dev);
        if (!vfs_lookup(fs, dev)) return false;
    } else if (strcmp(dev, "none") == 0 || strcmp(dev, "proc") == 0 ||
               strcmp(dev, "tmpfs") == 0 || strcmp(dev, "sysfs") == 0 ||
               strcmp(dev, "devtmpfs") == 0) {
        /* A virtual filesystem has no backing device. It is recorded so that
         * `mount` and `df` show it -- which is the whole reason it is in
         * fstab -- but nothing is layered over the path, because /proc is
         * synthesised by the kernel and /tmp is already a directory. */
        fs = NULL;
    } else {
        fs = device_fs(m, dev);
        if (!fs) return false;
        /* A dirty filesystem will not mount. That is not us being awkward: it
         * is the whole reason fsck exists, and it forces the repair to happen
         * in the right order. */
        if (fs == &m->disk && m->fs_dirty) return false;
    }

    /* The mountpoint has to exist, as on any real system -- and it is looked
     * for through the mounts already in place, so /mnt/dev can be a directory
     * on the customer's disk that we mounted a moment ago. */
    Vfs *host = m->on_rescue ? &m->rescue : &m->disk;
    const char *rest = at;
    for (int i = 0; i < m->nmount; i++) {
        if (!m->mount[i].used || !m->mount[i].fs) continue;
        size_t al = strlen(m->mount[i].at);
        if (strncmp(at, m->mount[i].at, al) != 0) continue;
        if (!(al == 1 || at[al] == 0 || at[al] == '/')) continue;
        host = m->mount[i].fs;
        rest = (al == 1) ? at : at + al;
        if (!*rest) rest = "/";
    }
    VNode *mp = vfs_resolve(host, rest, NULL);
    if (!mp || mp->kind != VN_DIR) return false;

    for (int i = 0; i < m->nmount; i++)
        if (m->mount[i].used && strcmp(m->mount[i].at, at) == 0) return false;

    Mount *mt = &m->mount[m->nmount++];
    memset(mt, 0, sizeof *mt);
    mt->used = true;
    mt->fs = fs;
    snprintf(mt->at, sizeof mt->at, "%s", at);
    snprintf(mt->dev, sizeof mt->dev, "%s", dev);
    snprintf(mt->sub, sizeof mt->sub, "%s", sub);
    return true;
}

/* fsck. A filesystem marked dirty was interrupted mid-write; the metadata can
 * be rebuilt and the contents of whatever was being written cannot. So this
 * clears the flag and tells you what it could not save -- and the files it
 * lost then show up in `pkg verify`, which is the second repair. */
int machine_fsck(Machine *m, const char *dev, Buf *out)
{
    if (strcmp(dev, "/dev/sda1") != 0 && strcmp(dev, "/dev/sda") != 0) {
        buf_printf(out, "fsck: %s: not a filesystem this tool understands\n", dev);
        return -1;
    }
    if (!m->fs_dirty) {
        buf_printf(out, "%s: clean\n", dev);
        return 0;
    }
    buf_printf(out, "%s: recovering journal\n", dev);
    buf_puts(out, "Pass 1: checking inodes, blocks, and sizes\n");
    buf_puts(out, "Pass 2: checking directory structure\n");
    if (m->fs_lost > 0)
        buf_printf(out, "Pass 4: %d inode(s) with bad content, cleared\n",
                   m->fs_lost);
    buf_puts(out, "Pass 5: checking group summary information\n");
    buf_printf(out, "%s: FILE SYSTEM WAS MODIFIED\n", dev);
    if (m->fs_lost > 0)
        buf_puts(out, "\nfsck repaired the filesystem. It could not repair the\n"
                      "CONTENTS of what was being written -- check the packages.\n");
    m->fs_dirty = false;
    return 1;
}

bool machine_umount(Machine *m, const char *at)
{
    for (int i = 0; i < m->nmount; i++) {
        if (!m->mount[i].used || strcmp(m->mount[i].at, at) != 0) continue;
        for (int j = i; j < m->nmount - 1; j++) m->mount[j] = m->mount[j + 1];
        m->nmount--;
        return true;
    }
    return false;
}

/* The configured repository channel, read off the machine's own disk. A
 * machine with no repo configuration falls back to `stable`, which is the
 * safe reading and matches what pkg would do. */
void machine_read_channel(Machine *m)
{
    snprintf(m->channel, sizeof m->channel, "stable");
    Buf names = {0};
    if (vfs_list(&m->disk, "/etc/pkg/repos.d", &names) != IO_OK) {
        buf_free(&names);
        return;
    }
    const char *p = names.p, *end = names.p + names.len;
    while (p && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        /* ONLY *.repo IS A REPOSITORY. This read everything in the directory,
         * and `pkg reinstall --force` puts the file it replaces beside it as
         * `.pkgsave` -- so correcting the channel and reinstalling left the
         * OLD channel in a backup file, the last one read won, and the
         * repository went on serving the wrong version to a player who had
         * just fixed the only line that matters. The saved copy is evidence,
         * not configuration. */
        if (len < 5 || memcmp(p + len - 5, ".repo", 5) != 0) {
            p = nl ? nl + 1 : NULL;
            continue;
        }
        char path[NOM_PATH_MAX];
        snprintf(path, sizeof path, "/etc/pkg/repos.d/%.*s", (int)len, p);
        VNode *n = vfs_resolve(&m->disk, path, NULL);
        if (n && n->kind == VN_FILE) {
            const char *q = n->data.p, *qe = n->data.p + n->data.len;
            while (q && q < qe) {
                const char *ql = memchr(q, '\n', (size_t)(qe - q));
                size_t l = ql ? (size_t)(ql - q) : (size_t)(qe - q);
                char line[160];
                if (l < sizeof line) {
                    memcpy(line, q, l);
                    line[l] = 0;
                    char key[40] = "", val[40] = "";
                    if (sscanf(line, " %39[^= ] = %39s", key, val) == 2 &&
                        strcmp(key, "channel") == 0)
                        snprintf(m->channel, sizeof m->channel, "%s", val);
                }
                q = ql ? ql + 1 : NULL;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    buf_free(&names);
}
