/* appl.h — appliance instances, and calling them.
 *
 * A Model is a spec: what a Veridian Ledger DX can do. An Inst is one of them
 * installed in this org, with its own records, its own rate-limit window and
 * its own load. Buying a second directory server means a second Inst of the
 * same Model, which is the whole of the Act III scaling mechanic (§5).
 *
 * EVERY CALL GOES THROUGH appl_call(). The web forms the client renders at M3
 * call it, the socket calls it, and the player's scripts call it. There is no
 * privileged path into an appliance's records except world seeding, which
 * runs once, before the player exists, and is marked as such.
 */
#ifndef RB_APPL_H
#define RB_APPL_H

#include "spec.h"

#define RB_VAL_MAX 64

typedef struct { char k[RB_NAME_MAX]; char v[RB_VAL_MAX]; } Field;

typedef struct {
    Field   f[SPEC_MAX_FIELDS];
    int     nf;
    uint8_t prov;          /* how this record came to exist (handoff §11) */
    int32_t created_day;
    bool    dead;          /* deleted, but still here; see reuse_key */
} Rec;

typedef struct {
    const CollSpec *cs;
    bool      reuse;       /* may a deleted record's key be used again? */
    Rec      *r;
    size_t    nr, cap;
    uint32_t *idx;         /* open addressing over the key; 0 empty, ~0 tombstone */
    size_t    icap;
} Coll;

typedef struct {
    char          id[RB_ID_MAX];       /* dir_01 */
    const Model  *m;
    const Vendor *v;
    Coll          coll[SPEC_MAX_COLL];
    int           ncoll;
    int32_t       installed_day;
    int32_t       cred_expires_day;    /* -1 when the model has no credential */
    /* Rate limiting is per instance per in-game minute (§10). It is the belt
     * speed of this game: the dial that decides how much a script can get
     * through in a day, and therefore when the player must start running
     * calls concurrently instead of in a row. */
    int32_t       rl_minute, rl_count;
    int64_t       calls;               /* lifetime, for the run report */
} Inst;

/* What a call did. `minutes` is in-game time consumed, which is the currency
 * of the whole game -- the day budget is 480 of them (§5). */
typedef struct {
    int  status;
    Buf  body;
    int  ms;              /* in-game milliseconds consumed */
    bool committed;       /* the write landed even though the call failed */
} ApiResult;

/* HTTP-SHAPED STATUS CODES, AND NOT HTTP (handoff §14). These are numbers the
 * player learns to branch on; the protocol note is that nothing here parses a
 * header, negotiates anything, or has a wire format. Believable beats
 * accurate, and a status code is believable. */
#define RB_OK          200
#define RB_CREATED     201
#define RB_BAD_REQUEST 400
#define RB_EXPIRED     401
#define RB_NOT_FOUND   404
#define RB_CONFLICT    409
#define RB_MISSING_REF 422
#define RB_RATE_LIMIT  429
#define RB_TRANSIENT   500
#define RB_NO_API      501

typedef struct World World;

Inst *inst_new(World *w, const Model *m, const Vendor *v, const char *id);
void  inst_free(Inst *in);
Coll *inst_coll(Inst *in, const char *name);

/* Look a record up by its key. Returns NULL for a record that does not exist;
 * a dead record is returned, because "this login is spent" and "there is no
 * such login" are different answers and the player needs both. */
Rec  *coll_find(Coll *c, const char *const *keyvals, int nkey);
Rec  *coll_insert(Coll *c, Prov prov, int32_t day);
/* Index a record once its key fields are set. Insert and index are separate
 * because the key is not known until the caller has filled it in. */
void  coll_index_rec(Coll *c, Rec *r);
const char *rec_get(const Rec *r, const char *k);
void  rec_set(Rec *r, const char *k, const char *v);

/* The one way in. args are the caller's fields; prov is who to blame. */
void appl_call(World *w, Inst *in, const char *endpoint,
               const Field *args, int nargs, Prov prov, ApiResult *out);

/* Seeding: build a record without going through an endpoint. Used once, by
 * world_new(), for the org that existed before the player was hired. It is
 * not exported to the API and must not be. */
Rec *appl_seed(Inst *in, const char *coll, Prov prov, int32_t day);

/* Instance load as a percentage of the model's nominal capacity. Past 100 the
 * appliance gets slower and fails more (§5 Act III, §10). */
int  inst_load_pct(const Inst *in);
void inst_render(const Inst *in, Buf *out);

#endif
