/* spec.h — appliances are declarative (handoff decision 6).
 *
 * One file per appliance model drives the API surface, the web forms the
 * Godot client will render, the in-game manual, and the examples --mancheck
 * executes. One source, so they cannot disagree — a manual documenting an
 * endpoint that does not exist teaches the player to distrust everything, and
 * the trust is the product (§13).
 *
 * WHAT THE HANDOFF'S §9 SKETCH DID NOT SAY, and had to be decided here:
 * endpoints need an operation. `method: POST` is HTTP cosplay and tells the
 * runtime nothing, and hand-writing C behind each endpoint would make content
 * unscalable, which defeats the point of the format. So every endpoint
 * declares one of five operations over one named collection:
 *
 *   list    every record, or those matching a filter
 *   get     one record by its key
 *   create  a record, with uniqueness and idempotency declared
 *   update  fields of an existing record
 *   delete  a record
 *
 * That is a small CRUD dialect and it covers the whole task domain, because
 * the task domain is object lifecycle (§6). If an appliance ever needs a sixth
 * operation, think hard: it is more likely that the mechanic wants to be
 * expressed as state than as a verb.
 */
#ifndef RB_SPEC_H
#define RB_SPEC_H

#include "yaml.h"

#define SPEC_MAX_FIELDS   12
#define SPEC_MAX_KEYS      3
#define SPEC_MAX_EP       16
#define SPEC_MAX_COLL      6
#define SPEC_MAX_FORMS     8
#define SPEC_MAX_EXAMPLES  4
#define SPEC_DOC_MAX     256

/* --------------------------------------------------------------- vendors */
/* Handoff §9: vendors are characterised through interface quality, and that
 * is the whole of their personality. Nothing here is flavour text — every
 * field changes what a script has to do. */
typedef enum {
    VEN_GOOD = 0,   /* clean API, sane errors, honest status codes, expensive */
    VEN_CHEAP,      /* web UI only: every unit is permanent manual labour */
    VEN_LEGACY,     /* API exists, lies about status codes, returns 200 on failure */
    VEN_FLAKY,      /* good API, poor uptime, rate limits aggressively */
    VEN__N
} VendorArch;

typedef struct {
    char       id[RB_NAME_MAX];      /* caldera */
    char       name[RB_NAME_MAX];    /* Caldera Systems */
    VendorArch arch;
    char       theme[RB_NAME_MAX];   /* per-vendor Godot theme (§14) */
    int        cost;                 /* relative, for the purchasing decision */
    int        rate_limit;           /* requests per minute per instance (§10) */
    int        transient_bp;         /* transient failure, basis points (1/10000) */
    char       doc[SPEC_DOC_MAX];
} Vendor;

/* ----------------------------------------------------------- collections */
typedef struct {
    char   name[RB_NAME_MAX];                       /* accounts */
    char   key[SPEC_MAX_KEYS][RB_NAME_MAX];         /* the unique, indexed key */
    int    nkey;
    char   field[SPEC_MAX_FIELDS][RB_NAME_MAX];
    int    nfield;
    /* May a deleted record's key be used again?
     *
     * For memberships, yes: removing someone from a group and adding them
     * back is ordinary. For accounts, NO, and that `no` is a mechanic. A
     * login is not free when its owner leaves -- their mailbox, home folder
     * and entries in six other systems are still there, and an account that
     * reuses it reads somebody else's mail. Offboarding is unforgiving of
     * records that were never written (handoff §6); this is where that starts. */
    bool   reuse_key;
    /* Reference data, present on every instance of the kind.
     *
     * Groups and shares are the org's, not one appliance's. When a second
     * directory server is racked it comes up with the same department groups
     * on it, because that is what happens -- and because the alternative is
     * making the player configure each new instance by hand, which is CONFIG
     * MANAGEMENT, which handoff decision 11 puts firmly in the sequel.
     *
     * Accounts, mailboxes, home folders and grants are not replicated: those
     * live where they were put, and where to put them is the Act III
     * decision. */
    bool   replicated;
    /* ONE EXTRA INDEXED FIELD, and exactly one.
     *
     * Accounts are keyed by login, but the thing that asks about them most
     * often is an acceptance check looking for "the account belonging to this
     * person" -- by user_ref, which is not the key. That is a linear scan,
     * and at 6,000 accounts and 7,000 tickets it was the difference between
     * a gate that takes 77 seconds and one that takes four.
     *
     * One, not a general secondary-index facility, because one is what the
     * content needs and a facility nobody uses is a facility that rots. If a
     * second appliance ever wants two, this becomes an array and the code
     * below barely changes. */
    char   index_field[RB_NAME_MAX];
    /* WHERE THE KEY HAS TO BE UNIQUE: this appliance, or the whole service.
     *
     * You can shard storage. You cannot shard names. A mailbox lives on one
     * mail server and it is perfectly sensible to spread them over six; an
     * ADDRESS is unique across the company, and so is a directory login,
     * because they are identity rather than storage.
     *
     * Without this, an Act III estate quietly grows two accounts with the
     * same login on different directory servers -- and since acceptance
     * checks search the whole service, the person ends up with an account
     * that exists, an account that works, and they are not the same account.
     * That is not a puzzle, it is a bug that looks like one. */
    bool   service_scope;
} CollSpec;

/* ------------------------------------------------------------- endpoints */
typedef enum { OP_LIST, OP_GET, OP_CREATE, OP_UPDATE, OP_DELETE, OP__N } EndpointOp;

/* Failure modes, as a bitmask. Every one of these is a teacher (§10):
 *  RATE_LIMITED          teaches queuing and backoff
 *  TRANSIENT             teaches retry
 *  TIMEOUT_AFTER_COMMIT  teaches idempotency, because retry alone is wrong
 *  STALL                 teaches concurrency, by making sequential feel slow
 * A fifth is deliberately absent: there is no failure mode that cannot be
 * handled. Every one of them has a correct response the player can write. */
#define FM_RATE_LIMITED         (1u << 0)
#define FM_TRANSIENT            (1u << 1)
#define FM_TIMEOUT_AFTER_COMMIT (1u << 2)
#define FM_STALL                (1u << 3)

typedef struct {
    char       id[RB_NAME_MAX];
    EndpointOp op;
    char       coll[RB_NAME_MAX];
    char       field[SPEC_MAX_FIELDS][RB_NAME_MAX];
    int        nfield;
    char       required[SPEC_MAX_FIELDS][RB_NAME_MAX];
    int        nrequired;
    char       filter[RB_NAME_MAX];       /* list: optional field to filter on */
    char       idem[SPEC_MAX_KEYS][RB_NAME_MAX];
    int        nidem;
    /* Referential integrity, and the reason it is declarative: it is exception
     * class 2 (handoff §8.2, ordering). `add_member` referencing `accounts`
     * means adding a group before the account exists fails — and a script that
     * does the steps in the wrong order produces a partial object that looks
     * correct and is not. Declaring it per endpoint keeps that lesson in the
     * content, where an agent authoring a new appliance can reach it. */
    char       reffield[SPEC_MAX_KEYS][RB_NAME_MAX];
    char       refcoll[SPEC_MAX_KEYS][RB_NAME_MAX];
    int        nref;
    bool       needs_credential;
    int        latency_ms;
    uint32_t   failure_modes;
    char       doc[SPEC_DOC_MAX];
    char       example[SPEC_MAX_EXAMPLES][RB_LINE_MAX / 32];
    int        nexample;
} Endpoint;

/* ------------------------------------------------------------------ form */
/* The web UI, generated (handoff decision 6). No client exists until M3; the
 * forms are declared now so that when it arrives it renders these rather than
 * growing a parallel definition of what the appliance can do. */
typedef struct {
    char id[RB_NAME_MAX];
    char calls[RB_NAME_MAX];
    char field[SPEC_MAX_FIELDS][RB_NAME_MAX];
    int  nfield;
    char title[SPEC_DOC_MAX];
} Form;

/* ----------------------------------------------------------------- model */
typedef struct {
    char     id[RB_NAME_MAX];         /* caldera_4400 — the API handle */
    char     model[RB_NAME_MAX];      /* Caldera 4400 MFP — what a human calls it */
    char     vendor[RB_NAME_MAX];
    char     kind[RB_NAME_MAX];       /* directory | mfp | mail | fileserver */
    bool     has_api, has_web;
    char     theme[RB_NAME_MAX];
    int      capacity;                /* objects served at nominal (§5, Act III) */
    int      credential_days;         /* 0 = no credential to expire */
    CollSpec coll[SPEC_MAX_COLL];
    int      ncoll;
    Endpoint ep[SPEC_MAX_EP];
    int      nep;
    Form     form[SPEC_MAX_FORMS];
    int      nform;
    char     doc[SPEC_DOC_MAX];
} Model;

/* -------------------------------------------------------------- registry */
struct TicketType;

typedef struct {
    Vendor            *vendor; size_t nvendor;
    Model             *model;  size_t nmodel;
    struct TicketType *ticket; size_t nticket;
    char               err[RB_ERR_MAX];
} Specs;

const struct TicketType *spec_ticket(const Specs *s, const char *id);

/* Load every spec. `dir` overrides the specs built into the binary, which is
 * how an author iterates without a rebuild; pass NULL for the embedded set.
 * Returns NULL and leaves the reason in `err` on any failure — a spec that
 * half-loads is worse than one that does not load. */
Specs *specs_load(const char *dir, char *err, size_t errcap);
void   specs_free(Specs *s);

const Vendor   *spec_vendor(const Specs *s, const char *id);
const Model    *spec_model(const Specs *s, const char *id);
const Endpoint *model_endpoint(const Model *m, const char *id);
const CollSpec *model_coll(const Model *m, const char *name);
const char     *op_name(EndpointOp op);
const char     *arch_name(VendorArch a);

#endif
