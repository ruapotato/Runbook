/* nom.h — the shim that lets NOMINAL's machine layer build here, unchanged.
 *
 * THE MACHINE IS LIFTED, NOT FORKED. cpu.c, kernel.c, image.c, boot.c, vfs.c
 * and ns.c are byte-for-byte the files from ~/NOMINAL/core, and the whole
 * point of this header is that they stay that way: an emulator, a kernel and
 * a package database that have been debugged against real play are worth far
 * more than the satisfaction of retyping them, and every edit made to them
 * here is an edit that has to be re-made when NOMINAL fixes something.
 *
 * So instead of touching eleven thousand lines, this file says `nom_alloc is
 * rb_alloc` and `NOM_PATH_MAX is 256`, and the machine compiles.
 *
 * WHAT IT DELIBERATELY DOES NOT BRING: the network. Handoff §2 is explicit --
 * no packet, frame, ARP, VLAN, DHCP or routing simulation, not "later", not
 * "lightweight". kernel.c has network syscalls because NOMINAL's game was
 * partly about them; here they are answered by machine/nonet.c, which says
 * there is no network and means it.
 */
#ifndef NOM_SHIM_H
#define NOM_SHIM_H

#include "rb.h"
#include <string.h>

/* ---------------------------------------------------------------- limits */
#define NOM_PATH_MAX   256
#define NOM_NAME_MAX   64
/* How long ONE program's argument string may be. Lifted with its reasoning
 * intact: a glob over a directory expands to every name in it, and a small
 * ceiling silently throws away everything past the first few matches --
 * leaving the player looking at a complete-looking answer with the evidence
 * missing. Must match GARG_MAX in the guest. */
#define NOM_ARG_MAX    16384
#define NOM_ERR_MAX    RB_ERR_MAX

/* Forward declarations NOMINAL's nom.h made near the top of the file. */
typedef struct Vfs Vfs;

/* ------------------------------------------------------------ allocation */
#define nom_alloc   rb_alloc
#define nom_realloc rb_realloc
#define nom_free    rb_free
#define nom_strdup  rb_strdup

/* ------------------------------------------------------------------- vfs */
/* A device file is a read callback plus a write callback plus a "would this
 * block right now" answer. D5. */
typedef struct VNode VNode;

/* VN_BIND grafts one path onto another (a bind mount). VN_LINK is a real
 * symlink: it derefs the same way, but it is a file you can see, edit and
 * break, and a dangling one is a legitimate state rather than an error in the
 * tree. The boot chain needs both. */
typedef enum { VN_DIR, VN_FILE, VN_DEV, VN_BIND, VN_LINK } VNodeKind;

/* Return values shared by device callbacks. */
typedef enum {
    IO_OK = 0,
    IO_BLOCK,     /* no data available yet; caller should suspend and retry */
    IO_ERR        /* err string set on the Vfs */
} IoStatus;

typedef IoStatus (*DevRead) (VNode *n, Buf *out, void *ctx);
typedef IoStatus (*DevWrite)(VNode *n, const char *data, size_t len, void *ctx);

struct VNode {
    char       name[NOM_NAME_MAX];
    VNodeKind  kind;
    VNode     *parent;
    VNode     *child;    /* first child, ordered by insertion */
    VNode     *next;     /* next sibling */
    /* VN_FILE */
    Buf        data;
    /* VN_DEV */
    DevRead    read;
    DevWrite   write;
    void      *ctx;
    int        id;       /* device-specific discriminator */
    int        src;      /* for a field file: the aggregate it reads out of */
    char       target[NOM_PATH_MAX];  /* VN_BIND/VN_LINK: what this stands for */
    /* Unix mode bits, low 9 only. An init that is not executable is a real
     * fault with a real error message, so this has to be modelled. */
    unsigned   mode;
    
    /* bookkeeping the sim uses to surface honest diagnostics */
    uint64_t   reads, writes, blocks;
    uint64_t   last_read_tick, last_write_tick;
};

struct Vfs {
    VNode *root;
    char   err[NOM_ERR_MAX];
    void  *ctx;          /* Sim *, for device callbacks */
};

void   vfs_init(Vfs *fs);
void   vfs_free(Vfs *fs);
VNode *vfs_mkdir(Vfs *fs, const char *path);
VNode *vfs_mkfile(Vfs *fs, const char *path, const char *contents);
VNode *vfs_mkdev(Vfs *fs, const char *path, DevRead rd, DevWrite wr, int id);
/* A single field of an aggregate status device, as its own file. Plan 9 shape:
 * `read("/dev/reactor/state")` beats `parse(read(".../status"))["state"]`. */
VNode *vfs_mkfield(Vfs *fs, const char *path, DevRead rd, int id, int src);
VNode *vfs_lookup(Vfs *fs, const char *path);
IoStatus vfs_read (Vfs *fs, const char *path, Buf *out);
IoStatus vfs_write(Vfs *fs, const char *path, const char *data, size_t len);
IoStatus vfs_list (Vfs *fs, const char *path, Buf *out);  /* one name per line */
bool   vfs_remove(Vfs *fs, const char *path);
/* Graft `target` at `path`. Reads and writes on `path` are performed on
 * `target`, so a script that only knows /dev/scrubber does not care that it is
 * really /n/wreck/dev/thm-04. This is the whole point of the namespace. */
VNode *vfs_bind(Vfs *fs, const char *target, const char *path);
VNode *vfs_symlink(Vfs *fs, const char *target, const char *path);
/* Resolve through symlinks and binds. Returns NULL for a dangling link, and
 * sets *dangling so the caller can tell "missing" from "points at nothing". */
VNode *vfs_resolve(Vfs *fs, const char *path, bool *dangling);
void   vfs_normalize(const char *cwd, const char *in, char *out, size_t outsz);

#endif /* NOM_SHIM_H */
