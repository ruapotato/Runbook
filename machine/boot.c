/* boot.c — the boot chain.
 *
 * THE RULE (D17): every stage reads real files and fails because of what it
 * finds. Nothing in this file may ask "which fault was injected?" — there is
 * no such question to ask, because the breaker only ever edits the disk.
 *
 * The console output is the player's primary evidence, so it obeys one rule of
 * its own: a stage says what it TRIED and what it GOT. It never says what is
 * wrong, because the machine does not know, and a machine that diagnoses
 * itself is a machine that plays the game for you.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "nom.h"
#include "machine.h"

#include "kernel.h"

/* The last non-blank line the machine printed, which is what it was
 * complaining about when it stopped. Returned in a static buffer: this is
 * called once, at the end of a boot. */
static const char *last_line(const Buf *b)
{
    static char out[NOM_ERR_MAX];
    if (!b->len) return NULL;
    size_t end = b->len;
    while (end && (b->p[end-1] == '\n' || b->p[end-1] == '\r')) end--;
    if (!end) return NULL;
    size_t start = end;
    while (start && b->p[start-1] != '\n') start--;
    size_t n = end - start;
    if (n >= sizeof out) n = sizeof out - 1;
    memcpy(out, b->p + start, n);
    out[n] = '\0';
    return out;
}

/* Did the console print this yet? Used only to say WHICH stage a failure
 * happened in, which is an observation about the output, not a flag the
 * runtime carries. */
static bool buf_contains(const Buf *b, const char *needle)
{
    size_t nl = strlen(needle);
    if (b->len < nl) return false;
    for (size_t i = 0; i + nl <= b->len; i++)
        if (memcmp(b->p + i, needle, nl) == 0) return true;
    return false;
}

const char *boot_stage_name(BootStage s)
{
    switch (s) {
    case BOOT_FIRMWARE: return "firmware";
    case BOOT_LOADER:   return "bootloader";
    case BOOT_KERNEL:   return "kernel";
    case BOOT_INITRD:   return "initrd";
    case BOOT_INIT:     return "init";
    case BOOT_SERVICES: return "services";
    case BOOT_LOGIN:    return "login";
    case BOOT_TARGET:   return "target";
    default:            return "?";
    }
}

typedef struct {
    Machine *m;
    Buf     *con;
} BootCtx;

static void say(BootCtx *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char line[256];
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    buf_puts(c->con, line);
    buf_putc(c->con, '\n');
}

static bool fail(Machine *m, BootCtx *c, BootStage at, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->boot.reason, sizeof m->boot.reason, fmt, ap);
    va_end(ap);
    m->boot.failed_at = at;
    m->boot.running   = false;
    say(c, "%s", m->boot.reason);
    return false;
}

/* THE INITRD PROMISED A SHELL IT DOES NOT HAVE.
 *
 * Four boot failures ended "entering emergency shell", and a player who went
 * looking for it found this instead, from the person standing at the machine:
 * "There is nowhere to type it. It has not finished starting up -- there is
 * no prompt, just the writing that stopped."  A console that names a way out
 * that is not there is the same lie as a console faking a boot, and it is
 * worse than saying nothing, because the player spends the next ten minutes
 * hunting for the prompt rather than reaching for the rescue medium.
 *
 * A REAL emergency shell was the other option and it is not the right one
 * here. /bin is on the filesystem that would not mount, so the shell would
 * have to live inside the image -- and this machine's initrd is BUILT, by
 * mkinitrd, out of nothing but the modules in /lib/modules. Putting a
 * userland in it means mkinitrd has to put one there too, or rebuilding the
 * initrd silently removes the rescue tool the console just recommended. That
 * is a second fault class invented to serve a message.
 *
 * So the message tells the truth: this image carries drivers and no shell,
 * the machine stops here, and the way in is the medium that has a userland
 * on it. Every stop in the initrd says the same thing in the same words. */
static void initrd_no_shell(BootCtx *c)
{
    say(c, "initrd: this image carries driver modules and no shell, so there");
    say(c, "        is no prompt here and nothing to type at. The machine stops.");
    say(c, "        Bring it up on the rescue medium to reach the disk:");
    say(c, "        rcon media insert / rcon boot media / rcon power cycle");
}

/* Anything read off a damaged disk can be arbitrary bytes, and it gets echoed
 * into console messages. Real consoles show you the mess without becoming
 * unreadable, so: printable ASCII passes, everything else becomes a dot, and
 * the whole thing is clipped. The player still sees that a file is garbage —
 * that is evidence — without the output turning into control codes. */
static const char *clean(const char *src, size_t len, char *out, size_t outsz)
{
    size_t j = 0;
    for (size_t i = 0; i < len && j + 4 < outsz; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '\n' || ch == '\t') out[j++] = ' ';
        else if (ch >= 0x20 && ch < 0x7f) out[j++] = (char)ch;
        else out[j++] = '.';
    }
    if (j + 4 >= outsz && len > j) { out[j++] = '.'; out[j++] = '.'; out[j++] = '.'; }
    out[j] = '\0';
    return out;
}

/* Copy one line out of a raw buffer into a NUL-terminated scratch string.
 * Parsing straight out of a Buf with sscanf reads past the end when the file
 * has no trailing newline, which is exactly what a truncating corruption
 * produces. */
static size_t line_at(const char *p, const char *end, char *out, size_t outsz)
{
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
    size_t n = len < outsz - 1 ? len : outsz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return len;
}

/* Read a whole file, following symlinks. Distinguishes the three states that
 * matter to a boot: present, absent, and present-but-pointing-at-nothing. */
typedef enum { F_OK, F_MISSING, F_DANGLING, F_NOTFILE } FileState;

static FileState slurp(Machine *m, const char *path, Buf *out, unsigned *mode,
                       char *linktarget, size_t ltsz)
{
    VNode *ln = vfs_lookup(&m->disk, path);
    if (!ln) return F_MISSING;
    if (ln->kind == VN_LINK && linktarget)
        snprintf(linktarget, ltsz, "%s", ln->target);
    bool dangling = false;
    VNode *n = vfs_resolve(&m->disk, path, &dangling);
    if (dangling) return F_DANGLING;
    if (!n) return F_MISSING;
    if (n->kind != VN_FILE) return F_NOTFILE;
    if (mode) *mode = n->mode;
    if (out) buf_put(out, n->data.p, n->data.len);
    return F_OK;
}

/* Pull `key` out of an indented config block. Returns NULL if absent. The
 * parser is deliberately unforgiving about nothing: a config with a typo'd
 * key simply does not have the key, which is how real config breaks. */
static bool cfg_get(const Buf *b, const char *key, char *out, size_t outsz)
{
    size_t klen = strlen(key);
    const char *p = b->p, *end = b->p + b->len;
    while (p && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *s = p;
        while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
        if (len > klen && strncmp(s, key, klen) == 0 &&
            (s[klen] == ' ' || s[klen] == '\t' || s[klen] == '=')) {
            const char *v = s + klen;
            size_t vl = len - klen;
            while (vl && (*v == ' ' || *v == '\t' || *v == '=')) { v++; vl--; }
            while (vl && (v[vl-1] == ' ' || v[vl-1] == '\r')) vl--;
            if (vl >= outsz) vl = outsz - 1;
            memcpy(out, v, vl);
            out[vl] = '\0';
            return true;
        }
        p = nl ? nl + 1 : NULL;
    }
    return false;
}

/* A BOOTLOADER CONFIGURATION HAS ENTRIES, AND ONE OF THEM IS THE DEFAULT.
 *
 * The loader used to read the first `kernel`, `initrd` and `root` line
 * anywhere in the file and ignore `default` and `entry` completely -- so the
 * menu it printed was decoration, and the commonest real bootloader mistake
 * there is (an upgrade appends an entry, the default still names the old one)
 * could not be expressed at all. Now the file is read the way zbl reads it:
 * lines before the first `entry` are global, each `entry` opens a block, and
 * `default N` says which block to boot.
 *
 * A config with no `entry` line at all is still read whole, because that is
 * what a truncated or hand-written one looks like and it should fail on what
 * it is missing rather than on its shape.
 */
static int cfg_entry_count(const Buf *b)
{
    int n = 0;
    const char *p = b->p, *end = b->p + b->len;
    while (p && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *s = p;
        while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
        if (len >= 5 && strncmp(s, "entry", 5) == 0 &&
            (len == 5 || s[5] == ' ' || s[5] == '\t')) n++;
        p = nl ? nl + 1 : NULL;
    }
    return n;
}

/* The TITLE of entry `idx` -- what `entry "NomnixOS 11.4"` is calling it.
 * The menu used to be three lines of decoration printed regardless of what
 * the file said, so a machine whose config held one entry drew a box
 * offering three and then announced `booting entry 0 of 1` underneath it. A
 * playtester read that box, went looking for the rescue entry it advertised,
 * and there was no way to pick anything: zbl has no menu keys and the rescue
 * medium is reached from the service processor instead. */
static bool cfg_entry_title(const Buf *b, int idx, char *out, size_t cap)
{
    int n = -1;
    const char *p = b->p, *end = b->p + b->len;
    while (p && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *s = p;
        while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
        if (len >= 5 && strncmp(s, "entry", 5) == 0 &&
            (len == 5 || s[5] == ' ' || s[5] == '\t')) {
            if (++n == idx) {
                const char *t = s + 5;
                size_t tl = len - 5;
                while (tl && (*t == ' ' || *t == '\t' || *t == '"')) { t++; tl--; }
                while (tl && (t[tl - 1] == ' ' || t[tl - 1] == '\t' ||
                              t[tl - 1] == '"' || t[tl - 1] == '\r')) tl--;
                if (tl >= cap) tl = cap - 1;
                memcpy(out, t, tl);
                out[tl] = 0;
                return tl > 0;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    return false;
}

/* The bytes of entry `idx`, not including the `entry` line itself. */
static bool cfg_entry_range(const Buf *b, int idx, Buf *out)
{
    int n = -1;
    const char *p = b->p, *end = b->p + b->len;
    const char *start = NULL;
    while (p && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *s = p;
        size_t sl = len;
        while (sl && (*s == ' ' || *s == '\t')) { s++; sl--; }
        bool is_entry = sl >= 5 && strncmp(s, "entry", 5) == 0 &&
                        (sl == 5 || s[5] == ' ' || s[5] == '\t');
        if (is_entry) {
            if (start) { out->p = (char *)start; out->len = (size_t)(p - start);
                         out->cap = 0; return true; }
            n++;
            if (n == idx) start = nl ? nl + 1 : end;
        }
        p = nl ? nl + 1 : NULL;
    }
    if (!start) return false;
    out->p = (char *)start;
    out->len = (size_t)(end - start);
    out->cap = 0;
    return true;
}

/* The version an image says it is, out of its own header line: the second
 * word of "\x7fKRNL 6.4.11 rv64" or "\x7fINITRD 6.4.11". A kernel and its
 * modules and its initrd all have to be the same one, and the whole family of
 * upgrade failures is what happens when they are not. */
static void image_version(const Buf *b, char *out, size_t outsz)
{
    out[0] = '\0';
    if (!b->len) return;
    const char *p = b->p, *end = b->p + b->len;
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    if (nl) end = nl;
    while (p < end && *p != ' ') p++;             /* past the magic word */
    while (p < end && *p == ' ') p++;
    const char *s = p;
    while (p < end && *p != ' ' && *p != '\r') p++;
    size_t n = (size_t)(p - s);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

/* What is actually installed under /lib/modules, as a list. Saying this is
 * what separates "the kernel is the wrong one" from "the modules are the
 * wrong ones", which are opposite repairs. */
static void modules_installed(Machine *m, char *out, size_t outsz)
{
    size_t o = 0;
    out[0] = '\0';
    VNode *d = vfs_lookup(&m->disk, "/lib/modules");
    for (VNode *k = d ? d->child : NULL; k; k = k->next) {
        if (k->kind != VN_DIR) continue;
        size_t n = strlen(k->name);
        if (o + n + 3 >= outsz) break;
        if (o) { out[o++] = ','; out[o++] = ' '; }
        memcpy(out + o, k->name, n);
        o += n;
        out[o] = '\0';
    }
    if (!o) snprintf(out, outsz, "(nothing)");
}

/* Does this initrd carry a module by this name? */
static bool initrd_has_module(const Buf *b, const char *name)
{
    const char *p = b->p, *end = b->p + b->len;
    while (p && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (len > 7 && strncmp(p, "module ", 7) == 0) {
            const char *v = p + 7; size_t vl = len - 7;
            while (vl && (v[vl-1] == ' ' || v[vl-1] == '\r')) vl--;
            if (vl == strlen(name) && strncmp(v, name, vl) == 0) return true;
        }
        p = nl ? nl + 1 : NULL;
    }
    return false;
}

/* WHAT THE INITRD DOES CARRY, in one line.
 *
 * "no driver for the root device" is true and useless on its own: it reads
 * the same whether the image has no modules at all, or a full set built for
 * somebody else's hardware. Those are different faults with different repairs
 * -- one is `mkinitrd`, the other is a machine the image was never meant for
 * -- and the difference is visible the moment the loader says what it has. */
static void initrd_modules(const Buf *b, char *out, size_t outsz)
{
    size_t o = 0;
    out[0] = '\0';
    const char *p = b->p, *end = b->p + b->len;
    while (p && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (len > 7 && strncmp(p, "module ", 7) == 0) {
            const char *v = p + 7; size_t vl = len - 7;
            while (vl && (v[vl-1] == ' ' || v[vl-1] == '\r')) vl--;
            if (o + vl + 3 < outsz) {
                if (o) { out[o++] = ','; out[o++] = ' '; }
                memcpy(out + o, v, vl);
                o += vl;
                out[o] = '\0';
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    if (!o) snprintf(out, outsz, "(none)");
}

/* --- the chain --------------------------------------------------------- */

/* Persist what the machine said while it was booting.
 *
 * THE PRIMARY INSTRUMENT. A real administrator's first move on a machine that
 * will not come up is not to checksum the filesystem, it is to read what it
 * said on the way down. We never had that: the console scrolled past and was
 * gone, so `pkg verify` became the first thing anyone reached for, and three
 * playtests in a row reported the same thing -- that verify hands you the
 * answer before you have thought about anything.
 *
 * boot.log.1 is the one that matters. The customer rebooted before they rang,
 * so the boot worth reading is the one that already scrolled away, and
 * keeping exactly one generation is what makes that recoverable.
 *
 * Written to the DISK, so it is there at /mnt/var/log/boot.log when you are
 * standing on the rescue medium, which is where you will actually be. Failing
 * to write it is not an error worth reporting: a machine whose root is
 * read-only or whose /var/log is missing genuinely cannot keep a log, and
 * noticing that the log stops is itself a diagnosis. */
static void persist_boot_log(Machine *m)
{
    VNode *d = vfs_lookup(&m->disk, "/var/log");
    if (!d || d->kind != VN_DIR || !(d->mode & 0111) || !(d->mode & 0222))
        return;                         /* nowhere to put it, and that is fine */
    if (m->root_ro) return;             /* a read-only root keeps no logs */

    /* A FULL DISK HAS NO ROOM FOR A LOG, and this write goes straight at the
     * vfs, underneath the capacity check the syscall layer enforces -- so
     * without this it could push the disk past full and then the repair had
     * nowhere to write zbl.cfg. One seed of sixty went unfixable exactly that
     * way. Losing the log when the disk is full is correct behaviour and is
     * itself a diagnosis: the log stops, and where it stops tells you when. */
    uint64_t used = machine_disk_used(m);
    if (used >= m->fs_capacity) return;
    uint64_t room = m->fs_capacity - used;
    VNode *old = vfs_lookup(&m->disk, "/var/log/boot.log");
    if (old && old->kind == VN_FILE) room += old->data.len;   /* we overwrite it */
    if (room < 4096) return;            /* not worth the space we would take */

    /* Rotate: this boot's predecessor becomes .1, one generation only. */
    VNode *cur = vfs_lookup(&m->disk, "/var/log/boot.log");
    if (cur && cur->kind == VN_FILE) {
        VNode *prev = vfs_mkfile(&m->disk, "/var/log/boot.log.1", "");
        if (prev) {
            buf_clear(&prev->data);
            buf_put(&prev->data, cur->data.p, cur->data.len);
            prev->mode = 0644;
        }
    }

    VNode *n = vfs_mkfile(&m->disk, "/var/log/boot.log", "");
    if (!n) return;
    buf_clear(&n->data);
    /* Capped: a respawn loop can produce a great deal of output, and the log
     * must not be the thing that fills the disk. The tail is what matters
     * anyway -- the end of the log is where the boot stopped. */
    const char *p = m->boot.console.p;
    size_t len = m->boot.console.len;
    size_t CAP = 16u * 1024u;
    /* Never take more than a quarter of what is left. */
    if (CAP > room / 4) CAP = room / 4;
    if (len > CAP) {
        size_t skip = len - CAP;
        while (skip < len && p[skip] != '\n') skip++;
        if (skip < len) skip++;
        buf_puts(&n->data, "[earlier output dropped: this log is capped]\n");
        buf_put(&n->data, p + skip, len - skip);
    } else {
        buf_put(&n->data, p, len);
    }
    n->mode = 0644;
}

void machine_boot(Machine *m)
{
    /* A reboot kills everything that was running. Without this the daemon
     * table filled up across successive boots and later services were
     * refused a slot without ever being run -- so they failed silently, with
     * no message from the program itself, which is the worst kind of bug to
     * read. */
    kernel_stop_daemons(m);
    m->nproc = 0;
    m->next_pid = 1;
    /* A reboot unmounts everything. machine_boot_rescue did this and
     * machine_boot did not, so after any rescue session the old mounts were
     * still in the table and mountall's `mount none /proc` collided with a
     * /proc that was already there. The boot then failed at a line of fstab
     * that was perfectly correct, and `pkg verify` -- rightly -- never
     * implicated the filesystem package, because nothing was wrong with it.
     * A playtester called that a fairness bug and was completely right. */
    m->nmount = 0;
    /* THIS FUNCTION IS THE DISK BOOT. It is the one place that knows so, and
     * it is therefore the one place allowed to say which medium is running.
     *
     * It did not say. Every caller was expected to clear on_rescue itself,
     * and the caller that matters most -- the service processor's power
     * button -- did not: `rcon media eject`, `rcon boot disk`, `rcon power
     * cycle` printed the whole disk boot, zbios through kernel, and then
     * handed /sbin/init the RESCUE filesystem, because on_rescue was still
     * set from the last live boot. `cat /etc/hostname` said "rescue" while
     * `rcon status` said the drive was empty and the boot device was the
     * disk. Both halves were reporting honestly; one of them was reporting a
     * variable nobody had updated.
     *
     * So there was no way out of rescue through the service processor at all,
     * which is the only instrument the ticket gives you. A player with a
     * correct fix already on the disk could not get the machine to boot it. */
    m->on_rescue = false;

    buf_clear(&m->boot.console);
    m->powered        = true;    /* something is running the boot chain */
    m->boot.running   = false;
    m->boot.emergency = 0;
    m->boot.reason[0] = '\0';
    m->boot.reached   = BOOT_FIRMWARE;
    m->boot.failed_at = BOOT_FIRMWARE;

    BootCtx cx = { m, &m->boot.console };
    BootCtx *c = &cx;
    Buf f = {0};
    char link[NOM_PATH_MAX];
    unsigned mode = 0;

    /* ---- firmware ---- */
    /* A REAL POST, because a machine coming up should look like a machine
     * coming up. David: "if you power cycle we should clear the console and
     * show a grub like boot process." The screen is cleared by the caller;
     * this is what fills it. */
    say(c, "zbios 1.4  --  node-%s", m->id);
    say(c, "  memory ....... 512 MB ok");
    say(c, "  cpu .......... rv64im @ 1 core");
    say(c, "  storage ...... /dev/sda 1 partition, /dev/sr0 removable");
    /* WHAT THE FIRMWARE HAS BEEN TOLD TO BOOT, which is not a file and which
     * no package owns. Somebody put the installer in, set the boot order to
     * the optical drive, finished the job and took the disc out -- and the
     * machine has been coming up ever since only because nobody rebooted it.
     * Every file on the disk is perfect and there is nothing to boot.
     *
     * `zbl-install` puts the order back, the way grub-install rewrites the
     * firmware's boot entry, and so does `rcon boot disk` from the service
     * processor. */
    say(c, "  boot order ... %s", m->sp_bootdev == 1
                                  ? "/dev/sr0 (removable)"
                                  : "/dev/sda, /dev/sr0 (removable)");
    if (m->sp_bootdev == 1 && !m->sp_media) {
        say(c, "zbios: /dev/sr0: no medium in the drive");
        fail(m, c, BOOT_FIRMWARE,
             "zbios: nothing to boot -- the boot order lists only the "
             "removable drive");
        goto done;
    }
    if (!m->bootsector) {
        fail(m, c, BOOT_FIRMWARE, "no bootable device -- insert boot media");
        goto done;
    }

    /* ---- bootloader ---- */
    m->boot.reached = BOOT_LOADER;
    switch (slurp(m, "/boot/zbl/zbl.cfg", &f, NULL, NULL, 0)) {
    case F_MISSING:
    case F_DANGLING:
        fail(m, c, BOOT_LOADER, "zbl: /boot/zbl/zbl.cfg: not found");
        goto done;
    case F_NOTFILE:
        fail(m, c, BOOT_LOADER, "zbl: /boot/zbl/zbl.cfg: not a file");
        goto done;
    default: break;
    }
    say(c, "");
    say(c, "zbl 2.06");
    /* THE MENU IS THE FILE. Every line in this box is an entry zbl.cfg
     * really holds, so the count under it agrees with it, and a config
     * somebody has edited shows what they edited. */
    {
        int shown = cfg_entry_count(&f);
        char ds[32];
        int def = cfg_get(&f, "default", ds, sizeof ds) ? atoi(ds) : 0;
        say(c, "  +----------------------------------------------+");
        for (int i = 0; i < shown; i++) {
            char title[64];
            if (!cfg_entry_title(&f, i, title, sizeof title))
                snprintf(title, sizeof title, "entry %d", i);
            say(c, "  | %c %-42.42s |", i == def ? '*' : ' ', title);
        }
        if (shown == 0)
            say(c, "  | %-44.44s |", "(no entries in zbl.cfg)");
        say(c, "  +----------------------------------------------+");
        /* And the way to the OTHER medium, which is not in this file and
         * never was: zbl has no menu keys, so advertising `rescue medium` as
         * a fourth line was offering something nobody could pick. */
        say(c, "  * is the default. zbl has no keyboard: `default` in");
        say(c, "  zbl.cfg is what picks. The rescue medium is not booted");
        say(c, "  from here -- it is the service processor's `rescue`.");
        say(c, "  booting the default entry in 0s...");
    }
    say(c, "");

    /* Validate the whole file before using any of it, the way a real loader
     * does. Random damage inside a config should say WHICH LINE it choked on;
     * silently losing a key and failing later is a worse game and a worse
     * bootloader. */
    {
        static const char *DIRECTIVE[] = { "default", "timeout", "entry",
                                           "kernel", "initrd", "root", NULL };
        const char *p = f.p, *end = f.p + f.len;
        int lineno = 0;
        while (p && p < end) {
            char raw[256], scrub[256], word[64] = {0};
            size_t len = line_at(p, end, raw, sizeof raw);
            lineno++;
            const char *s2 = raw;
            while (*s2 == ' ' || *s2 == '\t') s2++;
            if (*s2 && *s2 != '#') {
                sscanf(s2, "%63s", word);
                bool known = false;
                for (int i = 0; DIRECTIVE[i]; i++)
                    if (strcmp(word, DIRECTIVE[i]) == 0) known = true;
                if (!known) {
                    fail(m, c, BOOT_LOADER, "zbl: zbl.cfg:%d: unrecognised directive: %s",
                         lineno, clean(word, strlen(word), scrub, sizeof scrub));
                    goto done;
                }
            }
            p = (p + len < end) ? p + len + 1 : NULL;
        }
    }

    /* WHICH ENTRY. A menu with three lines on it boots one of them, and which
     * one is `default`. An entry that is not there is a real and thoroughly
     * ordinary mistake -- somebody adds a test entry, boots it, deletes the
     * entry and leaves the default pointing past the end of the list. */
    Buf ent = f;
    {
        int nent = cfg_entry_count(&f);
        char ds[32];
        int def = cfg_get(&f, "default", ds, sizeof ds) ? atoi(ds) : 0;
        if (nent > 0) {
            if (def < 0 || def >= nent || !cfg_entry_range(&f, def, &ent)) {
                fail(m, c, BOOT_LOADER,
                     "zbl: default entry %d: there %s only %d entr%s in this "
                     "configuration", def, nent == 1 ? "is" : "are", nent,
                     nent == 1 ? "y" : "ies");
                goto done;
            }
            say(c, "zbl: booting entry %d of %d", def, nent);
        }
    }

    char kpath[NOM_PATH_MAX], ipath[NOM_PATH_MAX], rootspec[64];
    if (!cfg_get(&ent, "kernel", kpath, sizeof kpath)) {
        fail(m, c, BOOT_LOADER, "zbl: no kernel line in configuration");
        goto done;
    }
    if (!cfg_get(&ent, "initrd", ipath, sizeof ipath)) {
        fail(m, c, BOOT_LOADER, "zbl: no initrd line in configuration");
        goto done;
    }
    if (!cfg_get(&ent, "root", rootspec, sizeof rootspec)) {
        fail(m, c, BOOT_LOADER, "zbl: no root line in configuration");
        goto done;
    }

    /* ---- kernel ---- */
    m->boot.reached = BOOT_KERNEL;
    buf_clear(&f);
    link[0] = '\0';
    FileState st = slurp(m, kpath, &f, &mode, link, sizeof link);
    if (st == F_DANGLING) {
        fail(m, c, BOOT_KERNEL, "zbl: %s -> %s: no such file", kpath, link);
        goto done;
    }
    if (st != F_OK) {
        char scrub[256];
        fail(m, c, BOOT_KERNEL, "zbl: %s: not found",
             clean(kpath, strlen(kpath), scrub, sizeof scrub));
        goto done;
    }
    if (f.len < 5 || memcmp(f.p, "\x7fKRNL", 5) != 0) {
        fail(m, c, BOOT_KERNEL, "zbl: %s: bad magic -- not a kernel image", kpath);
        goto done;
    }
    /* WHICH KERNEL THIS ACTUALLY IS, out of the image rather than out of its
     * filename. The two can disagree -- a restore from backup, a downgrade,
     * an image copied over the top of another one -- and when they do, the
     * name on the disk is the thing lying. */
    char kver[32], scrubv[64];
    image_version(&f, kver, sizeof kver);
    say(c, "zbl: loading %s (%s)", kpath,
        clean(kver, strlen(kver), scrubv, sizeof scrubv));

    /* ---- initrd: find and mount the root filesystem ---- */
    m->boot.reached = BOOT_INITRD;
    buf_clear(&f);
    link[0] = '\0';
    st = slurp(m, ipath, &f, &mode, link, sizeof link);
    if (st == F_DANGLING) {
        fail(m, c, BOOT_INITRD, "zbl: %s -> %s: no such file", ipath, link);
        goto done;
    }
    if (st != F_OK) {
        char scrub[256];
        fail(m, c, BOOT_INITRD, "zbl: %s: not found",
             clean(ipath, strlen(ipath), scrub, sizeof scrub));
        goto done;
    }
    if (f.len < 7 || memcmp(f.p, "\x7fINITRD", 7) != 0) {
        fail(m, c, BOOT_INITRD, "zbl: %s: bad magic -- not an initrd image", ipath);
        goto done;
    }
    say(c, "kernel %s booting",
        clean(kver, strlen(kver), scrubv, sizeof scrubv));
    /* The machine now knows what it is running, so `uname` can stop guessing. */
    snprintf(m->booted_kver, sizeof m->booted_kver, "%s", kver);

    /* The initrd must carry the driver for the root device and the filesystem
     * it is formatted with. This is the classic one: regenerate the initrd
     * without a module and the machine cannot reach its own root. */
    if (!initrd_has_module(&f, "virtio_blk")) {
        char mods[192];
        initrd_modules(&f, mods, sizeof mods);
        say(c, "initrd: modules in this image: %s", mods);
        say(c, "initrd: no driver for the root device (virtio_blk)");
        m->boot.emergency = 1;
        fail(m, c, BOOT_INITRD,
             "initrd: waiting for %s ... timed out (30s), and no driver was "
             "going to appear",
             rootspec);
        initrd_no_shell(c);
        goto done;
    }
    if (!initrd_has_module(&f, "ext4")) {
        char mods[192];
        initrd_modules(&f, mods, sizeof mods);
        say(c, "initrd: modules in this image: %s", mods);
        say(c, "initrd: no filesystem driver for ext4");
        m->boot.emergency = 1;
        fail(m, c, BOOT_INITRD,
             "initrd: mount %s: unknown filesystem type",
             rootspec);
        initrd_no_shell(c);
        goto done;
    }

    /* The root the bootloader named has to be the root that exists. */
    const char *want = rootspec;
    char scrubu[256];
    /* A ROOT NAMED BY DEVICE, WHICH IS HOW IT WAS DONE BEFORE UUIDS AND HOW
     * people still write it by hand. It is legal and it works right up until
     * a disk is added and the numbering moves under it, which is exactly why
     * the installer writes a uuid instead. Either the node is there or it is
     * not, and that is a different sentence from "no disk carries that
     * uuid". */
    if (want[0] == '/') {
        /* This machine has one partition and it is /dev/sda1. Anything else
         * named as a device is a node that is not here. */
        if (strcmp(want, "/dev/sda1") != 0) {
            m->boot.emergency = 1;
            fail(m, c, BOOT_INITRD,
                 "initrd: waiting for %s ... timed out (30s), no such device "
                 "on this machine",
                 clean(want, strlen(want), scrubu, sizeof scrubu));
            initrd_no_shell(c);
            goto done;
        }
    } else {
        if (strncmp(want, "UUID=", 5) == 0) want += 5;
        if (strcmp(want, m->root_uuid) != 0) {
            m->boot.emergency = 1;
            fail(m, c, BOOT_INITRD,
                 "initrd: waiting for /dev/disk/by-uuid/%s ... timed out (30s), "
                 "no disk here carries that uuid",
                 clean(want, strlen(want), scrubu, sizeof scrubu));
            initrd_no_shell(c);
            goto done;
        }
    }
    /* The filesystem is checked before it is mounted, by the initrd, which is
     * the only thing running at this point. A machine that stops here has a
     * smaller toolbox than one that stops later -- there is no /bin yet,
     * because /bin is on the filesystem that will not mount. */
    if (m->fs_dirty) {
        say(c, "initrd: %s contains a file system with errors, check forced", rootspec);
        say(c, "initrd: UNEXPECTED INCONSISTENCY; RUN fsck MANUALLY");
        m->boot.emergency = 1;
        fail(m, c, BOOT_INITRD,
             "initrd: cannot mount root -- boot the rescue medium and run "
             "`fsck /dev/sda1`");
        goto done;
    }
    say(c, "initrd: mounted %s on /", rootspec);

    /* AND NOW THE KERNEL WANTS ITS MODULES, which live on the filesystem that
     * has only just been mounted. /lib/modules/<version> is a directory per
     * kernel, and an upgrade that lands one half of the pair leaves the two
     * out of step: either the modules are for a kernel that is not here, or
     * the kernel is one the modules were never built for. The console says
     * which, because the repairs are opposite -- reinstall the kernel package
     * in one direction, and in the other the image is the odd one out. */
    if (kver[0]) {
        char moddir[NOM_PATH_MAX];
        snprintf(moddir, sizeof moddir, "/lib/modules/%s", kver);
        VNode *md = vfs_lookup(&m->disk, moddir);
        bool empty = true;
        for (VNode *k = md ? md->child : NULL; k; k = k->next)
            if (k->kind == VN_FILE) { empty = false; break; }
        if (!md || md->kind != VN_DIR || empty) {
            char have[160], sv3[64];
            modules_installed(m, have, sizeof have);
            say(c, "kernel: /lib/modules holds: %s", have);
            m->boot.emergency = 1;
            fail(m, c, BOOT_KERNEL,
                 "kernel: %s: no modules for this kernel -- the image and "
                 "/lib/modules are out of step",
                 clean(moddir, strlen(moddir), sv3, sizeof sv3));
            goto done;
        }
    }

    /* AND THE INITRD BELONGS TO A KERNEL TOO. It carries that kernel's
     * modules and nothing else can load them, so an image built for another
     * version is not a smaller problem than a missing one. This is checked
     * AFTER /lib/modules, deliberately: when the kernel image itself is the
     * wrong one, everything disagrees with it at once and the console should
     * say which single thing is the odd one out, not blame the initrd for
     * being right. The repair here is `mkinitrd`, which builds one for the
     * kernel that is actually installed. */
    {
        char iver[32], sv2[64], sv4[64];
        image_version(&f, iver, sizeof iver);
        if (kver[0] && iver[0] && strcmp(kver, iver) != 0) {
            m->boot.emergency = 1;
            fail(m, c, BOOT_INITRD,
                 "initrd: %s was built for %s, and this kernel is %s -- "
                 "the initrd is the odd one out",
                 ipath,
                 clean(iver, strlen(iver), sv2, sizeof sv2),
                 clean(kver, strlen(kver), sv4, sizeof sv4));
            goto done;
        }
    }

    /* ---- PID 1: from here it is real, executed userland ----
     * The kernel does exactly what a kernel does: it finds /sbin/init and
     * runs it. It does not know what init will do, because init is a program
     * on the disk and the machine's behaviour from here is whatever that
     * program says it is. */
    m->boot.reached = BOOT_INIT;
    {
        char uerr[NOM_ERR_MAX] = "";
        int64_t rc = kernel_spawn(m, "/sbin/init", "", &m->boot.console, 0,
                                  uerr, sizeof uerr);
        if (rc != 0) {
            /* When a guest program fails it says why, on the console, in its
             * own words. That line IS the reason -- synthesising "init exited
             * with status 1" over the top would throw away the only evidence
             * the player has. Only fall back if nothing was said. */
            /* Every level already printed its own reason. The last thing
             * said is the reason the machine is down. */
            const char *last = last_line(&m->boot.console);
            bool from_console = (last != NULL);
            if (last) snprintf(uerr, sizeof uerr, "%s", last);
            else snprintf(uerr, sizeof uerr,
                          "init exited with status %lld -- nothing left to run",
                          (long long)rc);
            /* Which stage the machine died in is a fact about how far the
             * console got, not something the runtime was told. */
            BootStage at = BOOT_INIT;
            if (buf_contains(&m->boot.console, "rc.boot:")) at = BOOT_SERVICES;
            if (buf_contains(&m->boot.console, "rc.3:") ||
                buf_contains(&m->boot.console, "entering runlevel")) at = BOOT_SERVICES;
            /* getty runs after every service is up, so a failure there is a
             * different problem from a service that would not start: the
             * machine is running and simply cannot be logged into. */
            if (buf_contains(&m->boot.console, "getty:")) at = BOOT_LOGIN;
            /* The reason was taken FROM the console, so echoing it back would
             * print it twice. Record it without re-saying it. */
            if (from_console) {
                snprintf(m->boot.reason, sizeof m->boot.reason, "%s", uerr);
                m->boot.failed_at = at;
                m->boot.running = false;
            } else {
                fail(m, c, at, "%s", uerr[0] ? uerr : "init failed");
            }
            goto done;
        }
    }

    /* Let the daemons run for a moment before we call it up. A service that
     * starts cleanly and then falls over ten seconds later is a real and
     * miserable failure, and it can only exist because they keep running. */
    kernel_tick(m, 4, &m->boot.console);

    m->boot.reached   = BOOT_TARGET;
    m->boot.failed_at = BOOT_TARGET;
    m->boot.running   = true;

done:
    persist_boot_log(m);
    buf_free(&f);
}

/* Boot the rescue medium. There is no bootloader chain to walk: a live image
 * is loaded by the firmware directly, which is precisely why it still works
 * when the installed system's boot chain does not. */
void machine_boot_rescue(Machine *m)
{
    kernel_stop_daemons(m);
    m->nproc = 0;
    m->next_pid = 1;
    buf_clear(&m->boot.console);
    m->on_rescue = true;
    /* IT IS RUNNING, SO IT HAS POWER IN IT. machine_boot said so and this did
     * not, so after booting the live medium `rcon status` reported "power OFF
     * -- nothing is running in it" while the player was typing commands at
     * that machine's shell. Same class of bug as on_rescue: two halves of the
     * service processor disagreeing because one of them was never told. */
    m->powered = true;
    /* IT CANNOT BOOT A MEDIUM THAT IS NOT IN THE DRIVE. Booting one is
     * therefore a statement that it is, and the device table, blkid and the
     * service processor all read the same field. */
    m->sp_media = true;
    m->sp_bootdev = 1;
    m->nmount = 0;                 /* a fresh boot has nothing mounted */
    m->boot.running = false;
    m->boot.reason[0] = '\0';
    m->boot.reached = BOOT_FIRMWARE;

    BootCtx cx = { m, &m->boot.console };
    say(&cx, "zbios 1.4  booting from /dev/sr0 (rescue medium)");

    char uerr[NOM_ERR_MAX] = "";
    int64_t rc = kernel_spawn(m, "/sbin/init", "", &m->boot.console, 0,
                              uerr, sizeof uerr);
    if (rc != 0) {
        /* The rescue medium failing is a bug in NOMINAL, not a ticket: it is
         * never corrupted, so if it will not come up something is wrong with
         * the game rather than with the customer's machine. */
        fail(m, &cx, BOOT_INIT, "rescue medium failed to start: %s",
             uerr[0] ? uerr : "unknown");
        return;
    }
    m->boot.reached = BOOT_TARGET;
    m->boot.failed_at = BOOT_TARGET;
    m->boot.running = true;
}
