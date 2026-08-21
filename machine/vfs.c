/* vfs.c — the virtual file tree.
 *
 * Plan 9 shape, small: directories, plain files, and device files that are a
 * read callback plus a write callback. Blocking is a first-class answer, not
 * an error: a device that has nothing to say returns IO_BLOCK and the script
 * that asked suspends. That is the game. See D5.
 */
#include "nom.h"
#include <string.h>
#include <stdio.h>

static VNode *node_new(const char *name, VNodeKind kind, VNode *parent)
{
    VNode *n = nom_alloc(sizeof(VNode));
    snprintf(n->name, sizeof n->name, "%s", name);
    n->kind = kind;
    n->parent = parent;
    buf_init(&n->data);
    if (parent) {
        VNode **tail = &parent->child;
        while (*tail) tail = &(*tail)->next;
        *tail = n;
    }
    return n;
}

static void node_free(VNode *n)
{
    VNode *c = n->child;
    while (c) { VNode *next = c->next; node_free(c); c = next; }
    buf_free(&n->data);
    nom_free(n);
}

void vfs_init(Vfs *fs)
{
    memset(fs, 0, sizeof *fs);
    fs->root = node_new("", VN_DIR, NULL);
}

void vfs_free(Vfs *fs)
{
    if (fs->root) node_free(fs->root);
    fs->root = NULL;
}

/* Resolve `in` against `cwd`, collapsing "." and "..". Output always starts
 * with '/' and never ends with one (except the root itself). */
void vfs_normalize(const char *cwd, const char *in, char *out, size_t outsz)
{
    char tmp[NOM_PATH_MAX * 2];
    if (in && in[0] == '/')
        snprintf(tmp, sizeof tmp, "%s", in);
    else
        snprintf(tmp, sizeof tmp, "%s/%s", (cwd && *cwd) ? cwd : "", in ? in : "");

    /* split and rebuild */
    const char *segs[64];
    int nseg = 0;
    char *p = tmp;
    while (*p) {
        while (*p == '/') *p++ = 0;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        if (strcmp(start, ".") == 0) continue;
        if (strcmp(start, "..") == 0) { if (nseg) nseg--; continue; }
        if (nseg < 64) segs[nseg++] = start;
    }
    size_t o = 0;
    if (nseg == 0) { snprintf(out, outsz, "/"); return; }
    for (int i = 0; i < nseg && o + 1 < outsz; i++)
        o += (size_t)snprintf(out + o, outsz - o, "/%s", segs[i]);
    out[outsz - 1] = 0;
}

static VNode *walk(Vfs *fs, const char *path, bool create, VNodeKind leafkind)
{
    char buf[NOM_PATH_MAX * 2];
    snprintf(buf, sizeof buf, "%s", path);

    char *seg[64];
    int nseg = 0;
    for (char *p = buf; *p; ) {
        while (*p == '/') p++;
        if (!*p) break;
        if (nseg == 64) return NULL;
        seg[nseg++] = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
    }

    VNode *cur = fs->root;
    for (int i = 0; i < nseg; i++) {
        VNode *c = cur->child;
        while (c && strcmp(c->name, seg[i]) != 0) c = c->next;
        if (!c) {
            if (!create) return NULL;
            c = node_new(seg[i], (i == nseg - 1) ? leafkind : VN_DIR, cur);
        }
        cur = c;
    }
    return cur;
}

VNode *vfs_lookup(Vfs *fs, const char *path)
{
    char norm[NOM_PATH_MAX * 2];
    vfs_normalize("/", path, norm, sizeof norm);
    return walk(fs, norm, false, VN_FILE);
}

VNode *vfs_mkdir(Vfs *fs, const char *path)
{
    char norm[NOM_PATH_MAX * 2];
    vfs_normalize("/", path, norm, sizeof norm);
    VNode *n = walk(fs, norm, true, VN_DIR);
    /* A directory you cannot enter is a real fault, so directories carry a
     * mode like everything else rather than reading as 0000. */
    if (n->mode == 0) n->mode = 0755;
    return n;
}

VNode *vfs_mkfile(Vfs *fs, const char *path, const char *contents)
{
    char norm[NOM_PATH_MAX * 2];
    vfs_normalize("/", path, norm, sizeof norm);
    VNode *n = walk(fs, norm, true, VN_FILE);
    if (!n) return NULL;
    n->kind = VN_FILE;
    if (n->mode == 0) n->mode = 0644;
    buf_clear(&n->data);
    if (contents) buf_puts(&n->data, contents);
    return n;
}

VNode *vfs_mkdev(Vfs *fs, const char *path, DevRead rd, DevWrite wr, int id)
{
    char norm[NOM_PATH_MAX * 2];
    vfs_normalize("/", path, norm, sizeof norm);
    VNode *n = walk(fs, norm, true, VN_DEV);
    if (!n) return NULL;
    n->kind = VN_DEV;
    n->read = rd;
    n->write = wr;
    n->id = id;
    return n;
}

VNode *vfs_mkfield(Vfs *fs, const char *path, DevRead rd, int id, int src)
{
    /* If a writable control file already lives here, give it a read side
     * rather than replacing it: `echo 350 > /dev/helm/heading` and
     * `cat /dev/helm/heading` should both work on the one file. */
    VNode *existing = vfs_lookup(fs, path);
    if (existing && existing->kind == VN_DEV) {
        existing->read = rd;
        existing->src = src;
        return existing;
    }
    VNode *n = vfs_mkdev(fs, path, rd, NULL, id);
    if (n) n->src = src;
    return n;
}

/* A symlink, unlike a bind, may point at nothing. That is the whole reason it
 * is useful here: `/sbin/init -> /usr/lib/sysinit` with the target gone is one
 * of the most common real ways a machine stops booting. */
static VNode *deref(Vfs *fs, VNode *n, char *pathout, size_t outsz);

VNode *vfs_symlink(Vfs *fs, const char *target, const char *path)
{
    char norm[NOM_PATH_MAX * 2];
    vfs_normalize("/", path, norm, sizeof norm);
    VNode *n = walk(fs, norm, true, VN_LINK);
    if (!n) return NULL;
    n->kind = VN_LINK;
    n->mode = 0777;
    snprintf(n->target, sizeof n->target, "%s", target);
    return n;
}

VNode *vfs_resolve(Vfs *fs, const char *path, bool *dangling)
{
    if (dangling) *dangling = false;
    VNode *n = vfs_lookup(fs, path);
    if (!n) return NULL;
    bool was_link = (n->kind == VN_LINK || n->kind == VN_BIND);
    n = deref(fs, n, NULL, 0);
    if (!n && was_link && dangling) *dangling = true;
    return n;
}

VNode *vfs_bind(Vfs *fs, const char *target, const char *path)
{
    char norm[NOM_PATH_MAX * 2], tnorm[NOM_PATH_MAX * 2];
    vfs_normalize("/", path, norm, sizeof norm);
    vfs_normalize("/", target, tnorm, sizeof tnorm);
    if (strlen(tnorm) >= NOM_PATH_MAX) {
        snprintf(fs->err, sizeof fs->err, "%s: path too long to bind", target);
        return NULL;
    }
    if (!walk(fs, tnorm, false, VN_FILE)) {
        snprintf(fs->err, sizeof fs->err, "%s: no such file to bind", target);
        return NULL;
    }
    VNode *n = walk(fs, norm, true, VN_BIND);
    if (!n) return NULL;
    n->kind = VN_BIND;
    snprintf(n->target, sizeof n->target, "%s", tnorm);
    return n;
}

/* Follow a chain of binds to the node that actually holds the data. */
static VNode *deref(Vfs *fs, VNode *n, char *pathout, size_t outsz)
{
    for (int hop = 0; n && (n->kind == VN_BIND || n->kind == VN_LINK) && hop < 8; hop++) {
        if (pathout) snprintf(pathout, outsz, "%s", n->target);
        n = vfs_lookup(fs, n->target);
    }
    return n;
}

IoStatus vfs_read(Vfs *fs, const char *path, Buf *out)
{
    VNode *n = vfs_lookup(fs, path);
    n = deref(fs, n, NULL, 0);
    if (!n) {
        snprintf(fs->err, sizeof fs->err, "%s: no such file", path);
        return IO_ERR;
    }
    if (n->kind == VN_DIR) {
        snprintf(fs->err, sizeof fs->err, "%s: is a directory", path);
        return IO_ERR;
    }
    n->reads++;
    if (n->kind == VN_FILE) {
        buf_put(out, n->data.p, n->data.len);
        return IO_OK;
    }
    if (!n->read) {
        snprintf(fs->err, sizeof fs->err, "%s: device is write-only", path);
        return IO_ERR;
    }
    IoStatus st = n->read(n, out, fs->ctx);
    if (st == IO_BLOCK) n->blocks++;
    return st;
}

IoStatus vfs_write(Vfs *fs, const char *path, const char *data, size_t len)
{
    VNode *n = vfs_lookup(fs, path);
    n = deref(fs, n, NULL, 0);
    if (!n) {
        /* `>` semantics: writing to a path whose parent exists creates a plain
         * file. A shim needs to be able to own a path the ship reads. */
        char norm[NOM_PATH_MAX * 2];
        vfs_normalize("/", path, norm, sizeof norm);
        char *slash = strrchr(norm, '/');
        if (slash && slash != norm) {
            *slash = 0;
            VNode *parent = walk(fs, norm, false, VN_DIR);
            *slash = '/';
            if (parent && parent->kind == VN_DIR) {
                n = vfs_mkfile(fs, norm, "");
            }
        }
        if (!n) {
            snprintf(fs->err, sizeof fs->err, "%s: no such file", path);
            return IO_ERR;
        }
    }
    if (n->kind == VN_DIR) {
        snprintf(fs->err, sizeof fs->err, "%s: is a directory", path);
        return IO_ERR;
    }
    n->writes++;
    if (n->kind == VN_FILE) {
        buf_clear(&n->data);
        buf_put(&n->data, data, len);
        return IO_OK;
    }
    if (!n->write) {
        snprintf(fs->err, sizeof fs->err, "%s: device is read-only", path);
        return IO_ERR;
    }
    return n->write(n, data, len, fs->ctx);
}

IoStatus vfs_list(Vfs *fs, const char *path, Buf *out)
{
    VNode *n = vfs_lookup(fs, path);
    n = deref(fs, n, NULL, 0);
    if (!n) {
        snprintf(fs->err, sizeof fs->err, "%s: no such file", path);
        return IO_ERR;
    }
    if (n->kind != VN_DIR) { buf_puts(out, n->name); buf_putc(out, '\n'); return IO_OK; }
    for (VNode *c = n->child; c; c = c->next) {
        buf_puts(out, c->name);
        if (c->kind == VN_DIR) buf_putc(out, '/');
        else if (c->kind == VN_DEV) buf_putc(out, '*');
        else if (c->kind == VN_BIND) buf_puts(out, "@");
        buf_putc(out, '\n');
    }
    return IO_OK;
}

bool vfs_remove(Vfs *fs, const char *path)
{
    VNode *n = vfs_lookup(fs, path);
    if (!n || !n->parent) return false;
    VNode **link = &n->parent->child;
    while (*link && *link != n) link = &(*link)->next;
    if (!*link) return false;
    *link = n->next;
    n->next = NULL;
    node_free(n);
    return true;
}
