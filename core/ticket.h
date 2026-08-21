/* ticket.h — tickets, and the checks that close them.
 *
 * HANDOFF DECISION 9, WHICH EVERYTHING ELSE DEPENDS ON: tickets close by
 * state verification, never by player assertion. There is no "mark as done"
 * verb and there must never be one. A ticket names the state checks the game
 * will run, the checks are visible to the player from the moment it opens,
 * and closing is the game evaluating them against the world.
 *
 * That is the oracle. It is what lets an agent grade a run, what lets the
 * vacation test be machine-checkable, and what makes "did you actually do it"
 * a question with an answer instead of a judgement call.
 *
 * Ticket types are content, in specs/, for the same reason appliances are:
 * they must scale, and agents must be able to author them (decision 6). A
 * check is declarative — a named predicate over an appliance collection —
 * because a check written in C is a check nobody outside this file can add.
 */
#ifndef RB_TICKET_H
#define RB_TICKET_H

#include "appl.h"

#define TK_MAX_CHECKS 10
#define TK_MAX_WHERE   4
#define TK_MAX_FIELDS  4

typedef enum {
    /* A record matching `where` exists in `coll` on some appliance of
     * `appliance`. The workhorse: it is what "the account exists" means. */
    CHK_EXISTS = 0,
    /* No such record. Offboarding, and the migration audit. */
    CHK_ABSENT,
    /* A bound record's field equals a value. "status is active". */
    CHK_EQUALS,
    /* A bound record's field follows the org's login convention for the
     * ticket's subject, allowing a numeric de-collision suffix. Named as its
     * own kind rather than expressed as a comparison because the convention
     * lives in the model and must not be duplicated into content. */
    CHK_CONVENTION,
    CHK__N
} CheckKind;

typedef struct {
    char      id[RB_NAME_MAX];
    CheckKind kind;
    char      appliance[RB_NAME_MAX];   /* an appliance KIND, not an instance */
    char      coll[RB_NAME_MAX];
    Field     where[TK_MAX_WHERE];
    int       nwhere;
    char      bind[RB_NAME_MAX];        /* name the found record for later checks */
    char      field[RB_NAME_MAX];       /* CHK_EQUALS, CHK_CONVENTION */
    char      value[RB_VAL_MAX];        /* CHK_EQUALS */
    /* CONDITIONAL CHECKS, and why they are not a vacuous pass.
     *
     * `when: also_dept` means the check applies only to tickets that carry an
     * also_dept field; `unless: share_override` means it applies only to
     * those that do not. That is how one ticket type covers the ordinary
     * case and the exceptional one without the exception becoming a separate
     * type the player can recognise by its name.
     *
     * A skipped check is reported as "n/a", never as "PASS". The difference
     * matters: a check that silently passes because it did not apply is the
     * vacuous-gate failure, in the player's own tooling. */
    char      when[RB_NAME_MAX];
    char      unless[RB_NAME_MAX];
    char      doc[SPEC_DOC_MAX];
} Check;

typedef struct TicketType {
    char  id[RB_NAME_MAX];              /* user.onboard */
    char  subject_kind[RB_NAME_MAX];    /* user */
    int   sla_minutes;
    int   weight;                       /* relative frequency in the generator */
    char  description[SPEC_DOC_MAX];    /* prose, ALWAYS redundant with the fields */
    char  doc[SPEC_DOC_MAX];
    Check check[TK_MAX_CHECKS];
    int   ncheck;
} TicketType;

typedef struct Ticket {
    char             id[RB_ID_MAX];     /* TCK-00042 */
    const TicketType *type;
    int32_t          opened_day, opened_ms;
    int32_t          closed_day;        /* -1 while open */
    int32_t          due_day, due_ms;
    bool             breached;          /* missed its SLA, closed or not */
    char             subject[RB_ID_MAX];
    Field            fields[TK_MAX_FIELDS];
    int              nfields;
    uint8_t          closed_prov;       /* who did the work that closed it */
    int32_t          followups;         /* how many this one has spawned */
    /* THE TICKET THIS ONE IS CHASING, empty for original work.
     *
     * A chase is not a second piece of work -- it is the same work, asked
     * about again, and it closes when the original does. The distinction
     * matters to anything that measures: --naive's failure rate is a rate per
     * PIECE OF WORK, and counting chases would make a bot that fails one
     * ticket look like a bot that fails five, purely because the first
     * failure generated its own denominator. */
    char             parent[RB_ID_MAX];
    int64_t          followup_milli;    /* fractional follow-ups, carried */
} Ticket;

/* One check's verdict, with a reason. The reason is shown to the player: a
 * ticket that will not close and will not say why is the single most
 * frustrating thing this game could do. */
typedef struct {
    bool passed[TK_MAX_CHECKS];
    bool skipped[TK_MAX_CHECKS];   /* did not apply to this ticket */
    char why[TK_MAX_CHECKS][SPEC_DOC_MAX];
    int  n;
    bool all;
} Verdict;

typedef struct World World;

/* ----------------------------------------------------------- the queue */
Ticket *world_ticket_new(World *w, const TicketType *tt, const char *subject);
Ticket *world_ticket_find(World *w, const char *id);
/* Give a freshly raised ticket a history, if the dice say so. Separate from
 * world_ticket_new() because a chase must never draw one -- it is the same
 * work, and rolling again would make the exception rate depend on how far
 * behind the player is. */
void    world_ticket_exception(World *w, Ticket *t);
/* Settle every open ticket. Called at each day roll and whenever anything
 * reads the queue, so what the player sees is never stale. Free. */
int     world_ticket_sweep(World *w);
/* The day's chasing: still open, still past its SLA, so somebody asks again.
 * Called from world_day_advance() after the sweep, so nobody is chased for
 * work that was finished yesterday. */
void    world_ticket_day(World *w);
void    world_ticket_stats(const World *w, Buf *out);

/* Evaluate every acceptance check. Free of in-game cost, on purpose: knowing
 * whether you did the job is not a resource, it is the oracle. */
void ticket_evaluate(World *w, Ticket *t, Verdict *v);
/* Evaluate, and close if it passes. Returns true if it closed on this call. */
bool ticket_settle(World *w, Ticket *t);
void ticket_render(const World *w, const Ticket *t, Buf *out);
/* The prose, with the subject's details filled in. It is flavour and
 * onboarding, never the only source of a required fact (handoff §7). */
void ticket_describe(const World *w, const Ticket *t, char *out, size_t cap);

#endif
