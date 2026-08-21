/* play.c — the reference agent, and the naive one.
 *
 * Handoff §13: "--play: a reference agent plays through all three acts via
 * the API; reports per-act wall time and where it stalled." Together with
 * --naive this is the balance harness. Every number in §5 and §10 is meant to
 * be settled by running these, not by argument.
 *
 * IT PLAYS THROUGH THE API, NOT THROUGH THE STRUCTS. Everything below goes
 * through proto_exec() and reads the answers back as text, exactly as a
 * player's script will. An agent that reached into World* would be measuring
 * a game nobody can play, and would keep passing after the API broke.
 *
 * TWO AGENTS, ONE FILE, BECAUSE THE DIFFERENCE IS THE POINT:
 *
 *   careful  looks before it writes, retries what the manual says is
 *            retryable, verifies after the writes that can commit and then
 *            time out, and de-collides logins. This is what a competent
 *            player's script looks like by the end of Act II.
 *
 *   naive    parses the ticket, calls the obvious endpoints in order, checks
 *            nothing. This is the degeneracy gate of §8: if the naive agent
 *            can clear the queue, the exception design is dead content and
 *            Act II is a form-filling simulator with extra steps.
 *
 * Their failure rates are not a vibe check. They are a CI gate.
 */
#include "proto.h"
#include "ticket.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>

/* ------------------------------------------------------- reading answers */
/* A scanner, not a JSON parser, and deliberately: the agent reads the same
 * few fields out of every answer, and a parser here would be a second
 * implementation of the response format that could drift from the first. */
static bool json_str(const Buf *b, const char *key, char *out, size_t cap)
{
    if (!b->p) return false;
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(b->p, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return true;
}

static int answer_status(const Buf *b)
{
    if (!b->p || strncmp(b->p, "+OK", 3)) return -1;
    int st = 0;
    return sscanf(b->p + 3, " %d", &st) == 1 ? st : 0;
}

/* THE LEGACY VENDOR'S ANSWER, READ PROPERLY.
 *
 * Halcyon returns 200 whatever happened and puts the error in the body, in
 * XML. A script that trusts the status code reports a clean run over people
 * who have no home folder. This one line is the difference between the two
 * kinds of script, and it is why the careful agent uses this everywhere
 * instead of answer_status(). */
static bool answer_ok(const Buf *b)
{
    int st = answer_status(b);
    if (st < 0 || st >= 400) return false;
    if (b->p && strstr(b->p, "<error")) return false;
    if (b->p && strstr(b->p, "\"error\"")) return false;
    return true;
}

typedef struct {
    Session s;
    World  *w;
    Buf     out;
    /* what happened, for the report */
    long    calls, retries, failures;
    /* WHICH TICKETS HAVE BEEN TRIED, so the headline number means what §8
     * says it means: the fraction of TICKETS a naive bot fails, not the
     * fraction of ATTEMPTS. A ticket the naive agent can never close comes
     * back every day forever; counting each of those days separately drives
     * any long run toward 100% and hides the curve the gate is looking for. */
    uint8_t *tried;
    size_t   tried_cap;
} Agent;

static bool first_try(Agent *a, const char *tid)
{
    long n = strtol(tid + 4, NULL, 10);      /* TCK-00042 */
    if (n < 0) return false;
    size_t byte = (size_t)n / 8;
    if (byte >= a->tried_cap) {
        size_t cap = a->tried_cap ? a->tried_cap : 1024;
        while (byte >= cap) cap *= 2;
        a->tried = rb_realloc(a->tried, cap);
        memset(a->tried + a->tried_cap, 0, cap - a->tried_cap);
        a->tried_cap = cap;
    }
    uint8_t bit = (uint8_t)(1u << (n % 8));
    if (a->tried[byte] & bit) return false;
    a->tried[byte] |= bit;
    return true;
}

static void ex(Agent *a, const char *fmt, ...)
{
    char line[RB_LINE_MAX];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    buf_clear(&a->out);
    proto_exec(&a->s, line, &a->out);
    a->calls++;
}

/* Call, and retry what the manual documents as retryable — 500 and 429 — and
 * nothing else. Retrying a 409 is how a script spins forever; retrying a 400
 * is how it spins forever having learned nothing. */
static bool ex_retry(Agent *a, int tries, const char *fmt, ...)
{
    char line[RB_LINE_MAX];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    for (int i = 0; i < tries; i++) {
        buf_clear(&a->out);
        proto_exec(&a->s, line, &a->out);
        a->calls++;
        if (answer_ok(&a->out)) return true;
        int st = answer_status(&a->out);
        bool retryable = (st == RB_TRANSIENT || st == RB_RATE_LIMIT) ||
                         (a->out.p && strstr(a->out.p, "<error code=\"500\"")) ||
                         (a->out.p && strstr(a->out.p, "<error code=\"429\""));
        if (!retryable) break;
        a->retries++;
    }
    a->failures++;
    return false;
}

/* ------------------------------------------------- placement, in Act III
 *
 * Handoff §5: "placement and replica count are the decisions; there is no
 * cabling." This is both of them, and they are three lines each.
 *
 * The agent keeps a current instance per appliance kind and moves to the
 * least loaded one whenever it looks. It does NOT migrate anything: an
 * appliance that is full stops receiving new work and stays where it is,
 * which is what the aggregate capacity check is designed to allow (see
 * ticket.h). The cost of getting this wrong is not a ticket -- it is
 * latency, and latency is the day budget, and the day budget is the game. */
#define KINDS 3
static const char *const KIND[KINDS] = { "directory", "mail", "fileserver" };
static const char *const KIND_MODEL[KINDS] = { "veridian_dx", "veridian_post", "halcyon_fs9" };

/* NOT ONE INSTANCE PER KIND -- ALL OF THEM, plus which is emptiest.
 *
 * The first version kept only the least-loaded instance of each kind and
 * treated it as "the directory". That works right up until there are two
 * directory servers, and then it is wrong in a way that takes a while to see:
 * a returning contractor's old account is on directory_01, the agent looks
 * for it on directory_04, does not find it, and creates a SECOND account with
 * the same login on a different box. Both exist. The acceptance check, which
 * searches the whole service rather than one appliance, finds the old one and
 * says the person is still a contractor.
 *
 * The service is the union of its instances. A script that has learned that
 * is a script that has understood Act III; one that has not gets six of seven
 * checks and a person who cannot log in. */
#define MAX_INST 24
typedef struct {
    char id[KINDS][MAX_INST][RB_ID_MAX];
    int  n[KINDS];
    int  best[KINDS];          /* index of the emptiest */
    int  best_load[KINDS];
} Placement;

static void place(Agent *a, Placement *pl)
{
    ex(a, "appl.list");
    for (int k = 0; k < KINDS; k++) {
        int best = 1000;
        pl->n[k] = 0;
        pl->best[k] = 0;
        pl->best_load[k] = 0;
        for (const char *p = a->out.p; p && *p; ) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len && *p == '{') {
                Buf one;
                buf_init(&one);
                buf_put(&one, p, len);
                char kind[RB_NAME_MAX] = "", id[RB_ID_MAX] = "";
                if (json_str(&one, "kind", kind, sizeof kind) && !strcmp(kind, KIND[k]) &&
                    json_str(&one, "id", id, sizeof id) && pl->n[k] < MAX_INST) {
                    const char *lp = strstr(one.p, "\"load_pct\":");
                    int load = lp ? atoi(lp + 11) : 0;
                    snprintf(pl->id[k][pl->n[k]], RB_ID_MAX, "%s", id);
                    if (load < best) { best = load; pl->best[k] = pl->n[k]; pl->best_load[k] = load; }
                    pl->n[k]++;
                }
                buf_free(&one);
            }
            if (!nl) break;
            p = nl + 1;
        }
    }
}

/* The first instance of the kind is the fallback, and it always exists: the
 * pristine org boots with one of each (see world.c). If that ever stops being
 * true, --health's "the org has an appliance installed" check fails first. */
static const char *where(const Placement *pl, int k)
{
    static char fallback[KINDS][RB_ID_MAX];
    if (pl->n[k]) return pl->id[k][pl->best[k]];
    snprintf(fallback[k], RB_ID_MAX, "%s_01", KIND[k]);
    return fallback[k];
}

/* ------------------------------------------------------ the careful agent */
/* The org's login convention, in the agent, because the player has to write
 * it themselves. The game does not hand it over — this is a reimplementation
 * of an in-game document, and if the document and the model ever disagree,
 * --health's convention check fails before this does. */
static void convention(const char *given, const char *family, char *out, size_t cap)
{
    size_t n = 0;
    char base[RB_NAME_MAX];
    if (given[0]) base[n++] = (char)(given[0] | 0x20);
    for (const char *p = family; *p && n < sizeof base - 8; p++) {
        if (*p >= 'A' && *p <= 'Z')      base[n++] = (char)(*p | 0x20);
        else if (*p >= 'a' && *p <= 'z') base[n++] = *p;
        else if (*p >= '0' && *p <= '9') base[n++] = *p;
    }
    base[n] = 0;
    snprintf(out, cap, "%s", base);
}

/* Find an account by login anywhere in the directory service. Returns the
 * instance it is on, or NULL, and fills in who it belongs to. */
static const char *find_account(Agent *a, const Placement *pl, const char *login,
                                char *user_ref, size_t urcap, char *status, size_t stcap)
{
    for (int i = 0; i < pl->n[0]; i++) {
        ex(a, "api.call %s get_account login=%s", pl->id[0][i], login);
        if (answer_status(&a->out) == RB_OK && answer_ok(&a->out)) {
            if (user_ref) json_str(&a->out, "user_ref", user_ref, urcap);
            if (status)   json_str(&a->out, "status", status, stcap);
            return pl->id[0][i];
        }
    }
    return NULL;
}

static bool onboard_careful(Agent *a, const Placement *pl, const char *ticket_id, const char *subject)
{
    /* READ THE TICKET'S FIELDS, ALL OF THEM.
     *
     * Handoff §7: the description is prose and is always redundant with the
     * structured fields; the fields are the automatable part. A script that
     * reads `dept` and stops is the naive one. This is the whole difference
     * between the two agents in this file, and between a player's first
     * script and their fifth. */
    char also_dept[RB_NAME_MAX] = "", share[RB_VAL_MAX] = "", rehire[16] = "";
    ex(a, "ticket.get %s", ticket_id);
    json_str(&a->out, "also_dept", also_dept, sizeof also_dept);
    json_str(&a->out, "share_override", share, sizeof share);
    json_str(&a->out, "rehire", rehire, sizeof rehire);

    char given[RB_NAME_MAX] = "", family[RB_NAME_MAX] = "", dept[RB_NAME_MAX] = "";
    ex(a, "user.get %s", subject);
    if (!json_str(&a->out, "given", given, sizeof given) ||
        !json_str(&a->out, "family", family, sizeof family) ||
        !json_str(&a->out, "dept", dept, sizeof dept)) return false;

    /* LOOK FIRST. The convention gives a login; the directory decides whether
     * it is free. Exception class 1 (handoff §8): a name collision, or a new
     * hire who is already in the system as a contractor. A script that skips
     * this gets a 409 and, if it is the naive one, moves on regardless. */
    char base[RB_NAME_MAX], login[RB_VAL_MAX];
    convention(given, family, base, sizeof base);
    snprintf(login, sizeof login, "%s", base);

    /* LOOK FIRST, ACROSS THE WHOLE SERVICE. The convention gives a login; the
     * directory decides whether it is free. Exception class 1 (handoff §8): a
     * name collision, or a new hire who is already in the system as a
     * contractor. Which instance the answer is on matters -- memberships
     * reference the account on their own appliance, so everything that
     * follows has to happen where the account actually is. */
    const char *dir = NULL;
    char status[RB_NAME_MAX] = "";
    for (int suffix = 2; suffix < 100; suffix++) {
        char who[RB_ID_MAX] = "";
        const char *on = find_account(a, pl, login, who, sizeof who, status, sizeof status);
        if (!on) break;                       /* free */
        if (!strcmp(who, subject)) { dir = on; break; }   /* already theirs */
        snprintf(login, sizeof login, "%s%d", base, suffix);
        status[0] = 0;
    }
    if (!dir) dir = where(pl, 0);             /* a new account goes on the emptiest */

    if (!ex_retry(a, 4, "api.call %s create_account login=%s user_ref=%s display_name=\"%s %s\" dept=%s status=active",
                  dir, login, subject, given, family, dept)) {
        /* THE WRITE THAT COMMITTED AND THEN TIMED OUT (§10). Retrying is not
         * enough on its own, because the retry may be the duplicate. The
         * endpoint is idempotent on login, so the retry is safe — and this
         * check is what tells us it landed. */
        ex(a, "api.call %s get_account login=%s", dir, login);
        if (!answer_ok(&a->out)) return false;
    }
    /* RECONCILE, DO NOT ASSUME. Idempotency got the account there; it did not
     * make it right. A returning contractor's account already existed, and
     * the create that "succeeded" returned it untouched with its old status
     * on it. This is the lesson idempotency alone does not teach. */
    ex(a, "api.call %s get_account login=%s", dir, login);
    status[0] = 0;
    json_str(&a->out, "status", status, sizeof status);
    if (strcmp(status, "active"))
        if (!ex_retry(a, 4, "api.call %s update_account login=%s status=active dept=%s", dir, login, dept))
            return false;

    if (!ex_retry(a, 4, "api.call %s add_member login=%s group=dept-%s", dir, login, dept)) return false;
    if (also_dept[0] && !ex_retry(a, 4, "api.call %s add_member login=%s group=dept-%s", dir, login, also_dept))
        return false;
    if (!ex_retry(a, 4, "api.call %s create_mailbox login=%s address=%s@harbrook.example quota_mb=2048 status=active",
                  where(pl, 1), login, login)) {
        ex(a, "api.call %s get_mailbox login=%s", where(pl, 1), login);
        if (!answer_ok(&a->out)) return false;
    }
    if (!ex_retry(a, 5, "api.call %s create_home login=%s path=/home/%s quota_mb=8192", where(pl, 2), login, login)) {
        ex(a, "api.call %s get_home login=%s", where(pl, 2), login);
        if (!answer_ok(&a->out)) return false;
    }
    char sharename[RB_VAL_MAX];
    if (share[0]) snprintf(sharename, sizeof sharename, "%s", share);
    else          snprintf(sharename, sizeof sharename, "share-%s", dept);
    /* The share the ticket names, or the department default when it names
     * none. Not a special case in the script -- a field with a fallback,
     * which is what it will always look like once the player stops treating
     * the exception as an exception. */
    if (!ex_retry(a, 5, "api.call %s grant_share login=%s share=%s access=rw",
                  where(pl, 2), login, sharename)) return false;

    /* And ask the game, which is the only opinion that counts. */
    ex(a, "ticket.check %s", ticket_id);
    bool passed = a->out.p && strstr(a->out.p, "passes") != NULL;
    if (!passed && getenv("RUNBOOK_PLAY_DEBUG"))
        fprintf(stderr, "DEBUG careful %s login=%s\n%s\n", ticket_id, login, a->out.p ? a->out.p : "");
    return passed;
}

/* -------------------------------------------------------- the naive agent */
/* Parse the ticket, call the obvious endpoint, no branching, no retry, no
 * verification. Exactly the bot §8 says must fail a rising fraction of
 * tickets. It is not a strawman: it is what a competent programmer writes on
 * their first pass, before the world has taught them otherwise. */
static bool onboard_naive(Agent *a, const Placement *pl, const char *ticket_id, const char *subject)
{
    char given[RB_NAME_MAX] = "", family[RB_NAME_MAX] = "", dept[RB_NAME_MAX] = "";
    ex(a, "user.get %s", subject);
    if (!json_str(&a->out, "given", given, sizeof given) ||
        !json_str(&a->out, "family", family, sizeof family) ||
        !json_str(&a->out, "dept", dept, sizeof dept)) return false;

    char login[RB_VAL_MAX];
    convention(given, family, login, sizeof login);

    ex(a, "api.call %s create_account login=%s user_ref=%s display_name=\"%s %s\" dept=%s status=active",
       where(pl, 0), login, subject, given, family, dept);
    ex(a, "api.call %s add_member login=%s group=dept-%s", where(pl, 0), login, dept);
    ex(a, "api.call %s create_mailbox login=%s address=%s@harbrook.example quota_mb=2048 status=active", where(pl, 1), login, login);
    ex(a, "api.call %s create_home login=%s path=/home/%s quota_mb=8192", where(pl, 2), login, login);
    ex(a, "api.call %s grant_share login=%s share=share-%s access=rw", where(pl, 2), login, dept);

    ex(a, "ticket.check %s", ticket_id);
    if (getenv("RUNBOOK_PLAY_DEBUG") && !(a->out.p && strstr(a->out.p, "passes")))
        fprintf(stderr, "DEBUG naive %s/%s login=%s\n%s\n", ticket_id, subject, login, a->out.p ? a->out.p : "");
    return a->out.p && strstr(a->out.p, "passes") != NULL;
}

/* AND LOOK AGAIN AS THE DAY GOES ON.
 *
 * Placement decided once a morning sends every one of the day's three hundred
 * new accounts to whichever box was emptiest at nine o'clock -- which fills
 * it to 119% of nominal by teatime while nine others sit half empty. The
 * emptiest appliance stops being the emptiest as soon as you start using it.
 * Looking every thirty-two tickets costs nothing (appl.list is free) and
 * keeps the estate level. */
#define REPLACE_EVERY 32

/* ------------------------------------------------------- capacity tickets
 *
 * The whole of Act III's new work, and it is six lines, which is the point.
 * The hard part of scaling here is not the command -- it is that the command
 * costs forty in-game minutes and there is no longer a forty-minute hole in
 * the day. */
static const char *model_for(const char *kind)
{
    for (int k = 0; k < KINDS; k++) if (!strcmp(KIND[k], kind)) return KIND_MODEL[k];
    return NULL;
}

static bool capacity_careful(Agent *a, const char *ticket_id, const char *kind)
{
    const char *model = model_for(kind);
    if (!model) {
        /* An appliance kind this agent was never taught about. Say so rather
         * than failing silently -- when M8 adds vendors, this is the line
         * that will tell somebody the reference agent needs updating. */
        fprintf(stderr, "play: no model known for appliance kind '%s'\n", kind);
        return false;
    }
    /* ONE BOX MAY NOT BE ENOUGH. If the org outgrew the estate by more than a
     * single appliance's worth -- which happens after a hiring wave, or after
     * a few days when nobody was watching -- installing one and declaring
     * victory leaves the ticket open and the service still full. Keep buying
     * until the acceptance check is satisfied, and stop at four so a bug in
     * the check cannot spend the whole day racking hardware. */
    for (int i = 0; i < 4; i++) {
        ex(a, "appl.install %s", model);
        if (!answer_ok(&a->out)) return false;
        ex(a, "ticket.check %s", ticket_id);
        if (a->out.p && strstr(a->out.p, "passes")) return true;
    }
    return false;
}

/* AND BUY BEFORE THE TICKET, which is what separates a system from a script.
 *
 * The capacity ticket fires at 90% of nominal. A company growing at 6% a day
 * goes from 90% to over 100% overnight, so a system that waits to be asked
 * spends every night degraded -- and §12 allows a service to be over nominal
 * for thirty in-game minutes, not for a night. Watching the emptiest instance
 * and racking another at 80% costs forty minutes and keeps everything under
 * nominal, which is the whole job. */
static void capacity_ahead(Agent *a, Placement *pl)
{
    for (int k = 0; k < KINDS; k++) {
        if (!pl->n[k] || pl->best_load[k] <= 80) continue;
        const char *model = model_for(KIND[k]);
        if (!model) continue;
        ex(a, "appl.install %s", model);
        place(a, pl);
    }
}

/* ---------------------------------------------------------- THE VACATION
 *
 * Handoff §12, and the win condition: seven simulated days, zero player
 * input, at 4,000 users.
 *
 *   - at least 99% of tickets resolved within SLA
 *   - queue depth at the end no greater than at the start
 *   - no service below its capacity threshold for more than 30 consecutive
 *     in-game minutes
 *
 * The player triggers it voluntarily and can abort. FAILING IT IS DIAGNOSTIC,
 * NOT FATAL (§12): the report says which ticket types went unhandled and
 * which service fell over, and the player goes back and fixes their systems.
 * That is the endgame loop, and it is the reason there is no fail screen
 * anywhere in this game.
 *
 * Headless, the reference agent stands in for the systems the player built.
 * That is not a cheat -- it is the definition. The vacation asks whether the
 * automation that exists can run the company without anybody watching, and
 * here the automation that exists is this file. */
typedef struct {
    int  open_start, open_end;
    int  within_pct;
    int  degraded_minutes;      /* longest run of a service over nominal */
    char worst_service[RB_ID_MAX];
    int  worst_load;
    char unhandled[8][RB_NAME_MAX];
    int  nunhandled;
} Vacation;

static void vacation_note_unhandled(Vacation *v, const char *type)
{
    for (int i = 0; i < v->nunhandled; i++) if (!strcmp(v->unhandled[i], type)) return;
    if (v->nunhandled < 8) snprintf(v->unhandled[v->nunhandled++], RB_NAME_MAX, "%s", type);
}

/* -------------------------------------------------------------- the acts */
/* Act boundaries are headcount, from handoff §5. They are reported rather
 * than enforced: the acts are a description of what the game feels like at a
 * given size, not a state machine, and there is nothing to unlock. */
static const char *act_of(int users)
{
    if (users < 150) return "I";
    if (users < 800) return "II";
    return "III";
}

/* Wall clock is a measurement of the harness, never an input to the world.
 * Nothing the simulation does may depend on this number, which is why it is
 * allowed to differ between platforms -- and it has to, because mingw's
 * clock_gettime is not there to link against. */
#ifdef _WIN32
#  include <windows.h>
static double now_ms(void) { return (double)GetTickCount64(); }
#else
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#endif

/* ------------------------------------------------------- THE NAIVE GATE
 *
 * Handoff §8: "a deliberately naive bot -- parse ticket, call the obvious
 * endpoint, no branching, no retry -- must fail a rising fraction of tickets:
 * <=5% at the start of Act II, >=35% by its end. If the naive bot clears the
 * queue, the exception design is degenerate and the act is dead content. This
 * is a CI gate, not a vibe check."
 *
 * MEASURING IT BY LETTING THE NAIVE BOT PLAY DOES NOT WORK, and finding that
 * out was most of the work here. Set loose, it fails a ticket on day three,
 * cannot close it, gets chased for it, and drowns: by day twenty its queue is
 * two thousand deep, it never reaches a ticket raised after day fourteen, and
 * its measured rate flattens at whatever the org size was when it went under.
 * The number that comes out is real -- it really would drown -- but it is not
 * the number §8 is asking for, and it can never report on an 800-user org
 * because a naive bot never gets to see one.
 *
 * So the gate asks the question that actually matters: IF YOU WERE RUNNING
 * THE NAIVE SCRIPT AT THIS ORG SIZE, what fraction of the intake would fail?
 * The careful agent keeps the org healthy and growing; inside two sampling
 * windows -- around the Act I wall and around the end of Act II -- each fresh
 * ticket is given to the naive strategy first, scored, and then repaired by
 * the careful one so the run continues from a clean state.
 *
 * The windows are headcount, not day numbers, because the acts are headcount
 * (§5) and a run that grew faster or slower would otherwise sample the wrong
 * part of the curve. */
#define GATE_LO_MIN 140
#define GATE_LO_MAX 190
#define GATE_HI_MIN 780
#define GATE_HI_MAX 1000

typedef struct { long seen, failed; } Band;

static void band_report(const char *what, const Band *b, int lo_pct, int hi_pct, int *fails)
{
    if (!b->seen) {
        printf("naive: FAIL  %s: the run never reached this org size\n", what);
        (*fails)++;
        return;
    }
    long pct = (b->failed * 100) / b->seen;
    bool ok = (lo_pct < 0 || pct >= lo_pct) && (hi_pct < 0 || pct <= hi_pct);
    printf("naive: %s  %s: %ld of %ld tickets failed (%ld%%)",
           ok ? "PASS" : "FAIL", what, b->failed, b->seen, pct);
    if (lo_pct >= 0) printf(", wanted at least %d%%", lo_pct);
    if (hi_pct >= 0) printf(", wanted at most %d%%", hi_pct);
    printf("\n");
    if (!ok) (*fails)++;
}

static int play_once(uint64_t seed, const char *specdir, int days, bool naive, bool gate,
                     int users_cap, Band *lo_out, Band *hi_out, const char *out_path)
{
    char serr[RB_ERR_MAX];
    Specs *specs = specs_load(specdir, serr, sizeof serr);
    if (!specs) { printf("play: FAIL  specs do not load: %s\n", serr); return 1; }

    Agent a;
    memset(&a, 0, sizeof a);
    a.w = world_new(seed, specs);
    buf_init(&a.out);
    proto_open(&a.s, a.w);
    a.s.prov = PROV_SCRIPT;

    printf("play: %s, seed %llu, %d days%s\n",
           gate ? "naive gate (careful agent, naive sampled)" : (naive ? "naive agent" : "careful agent"),
           (unsigned long long)seed, days,
           users_cap ? " (stops early at the user cap)" : "");
    printf("play:  act  day  users  raised  closed  open  sla%%  attempted  failed   wall\n");

    Band lo = { 0, 0 }, hi = { 0, 0 };
    const char *act = act_of(a.w->active);
    double act_t0 = now_ms();
    int act_day0 = 0;
    long attempted = 0, failed = 0, act_attempted = 0, act_failed = 0;
    int stalled_day = -1;

    /* THE DAY'S QUEUE, TAKEN ONCE.
     *
     * The first version of this loop asked for the top open ticket, worked
     * it, and asked again. The careful agent never noticed, because it closes
     * what it starts. The naive agent hit one login collision on day three
     * and spent the next nine days failing the same ticket, which reported as
     * a 99% failure rate and told us nothing about the other 200 tickets it
     * never reached.
     *
     * A person does not do that. They put the awkward one aside and work the
     * rest of the queue. So: take the day's open tickets in one go, attempt
     * each once, and let the ones that fail come round again tomorrow -- with
     * their follow-ups attached, which is the compounding pressure doing its
     * job rather than a livelock pretending to be one. */
/* The day's whole queue, not a slice of it. At 4,000 users the intake alone
 * is around 240 tickets a day and the chases sit on top; a batch smaller than
 * that would make the agent look like it was falling behind when it was only
 * ever being handed a quarter of the work. On the heap, because this is four
 * thousand ids and the stack is not the place for it. */
#define PLAY_BATCH 8192
    char (*tids)[RB_ID_MAX] = rb_alloc(PLAY_BATCH * RB_ID_MAX);
    char (*refs)[RB_ID_MAX] = rb_alloc(PLAY_BATCH * RB_ID_MAX);
    char (*types)[RB_NAME_MAX] = rb_alloc(PLAY_BATCH * RB_NAME_MAX);
    bool  *chase = rb_alloc(PLAY_BATCH * sizeof *chase);

    for (int d = 0; d < days; d++) {
        int32_t today = a.w->day;
        int n = 0;

        /* Look at where things are before deciding where to put anything.
         * Once a day is enough: capacity moves slowly, and asking more often
         * would be a script doing work to feel busy. */
        Placement pl;
        place(&a, &pl);
        if (!naive) capacity_ahead(&a, &pl);

        ex(&a, "ticket.list open %d", PLAY_BATCH);
        for (const char *p = a.out.p; p && *p && n < PLAY_BATCH; ) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len && *p == '{') {
                Buf one;
                buf_init(&one);
                buf_put(&one, p, len);
                if (json_str(&one, "id", tids[n], RB_ID_MAX) &&
                    json_str(&one, "ref", refs[n], RB_ID_MAX)) {
                    json_str(&one, "type", types[n], RB_NAME_MAX);
                    /* A chase is the same work asked about again; it counts
                     * toward the queue the agent has to get through, not
                     * toward the work it was measured on. */
                    chase[n] = one.p && strstr(one.p, "\"chasing\"") != NULL;
                    n++;
                }
                buf_free(&one);
            }
            if (!nl) break;
            p = nl + 1;
        }

        for (int i = 0; i < n && a.w->day == today; i++) {
            bool first = !chase[i] && first_try(&a, tids[i]);
            Band *band = NULL;
            if (gate && first) {
                if (a.w->active >= GATE_LO_MIN && a.w->active <= GATE_LO_MAX) band = &lo;
                else if (a.w->active >= GATE_HI_MIN && a.w->active <= GATE_HI_MAX) band = &hi;
            }
            bool ok;
            if (!strcmp(types[i], "service.capacity")) {
                /* Both agents buy hardware. The naive one is naive about
                 * onboarding, not about arithmetic, and a bot that let the
                 * whole estate fill up would be failing for a reason §8 is
                 * not asking about. */
                ok = capacity_careful(&a, tids[i], refs[i]);
                place(&a, &pl);          /* a new appliance changes where work goes */
                band = NULL;
            } else if (band) {
                /* Score the naive strategy, then repair with the careful one
                 * so the org the next sample sees is a healthy one. */
                ok = onboard_naive(&a, &pl, tids[i], refs[i]);
                band->seen++;
                if (!ok) { band->failed++; ok = onboard_careful(&a, &pl, tids[i], refs[i]); }
            } else {
                ok = naive ? onboard_naive(&a, &pl, tids[i], refs[i])
                           : onboard_careful(&a, &pl, tids[i], refs[i]);
            }
            if (first) {
                attempted++; act_attempted++;
                if (!ok) { failed++; act_failed++; }
            }
            if (!naive && (i + 1) % REPLACE_EVERY == 0) { place(&a, &pl); capacity_ahead(&a, &pl); }
        }
        if (a.w->day == today) world_day_advance(a.w);

        if (strcmp(act, act_of(a.w->active))) {
            printf("play:  %-3s  %3d  %5d  %6d  %6d  %4d  %3d%%  %9ld  %6ld  %5.0fms\n",
                   act, a.w->day, a.w->active, a.w->next_tid, a.w->closed_total,
                   a.w->open_count,
                   a.w->closed_total ? ((a.w->closed_total - a.w->breached_total) * 100) / a.w->closed_total : 100,
                   act_attempted, act_failed, now_ms() - act_t0);
            act = act_of(a.w->active);
            act_t0 = now_ms();
            act_day0 = a.w->day;
            act_attempted = act_failed = 0;
        }
        /* WHERE IT STALLED, which is the other half of what §13 asks for.
         *
         * Not "the queue got big" -- the queue is SUPPOSED to get big, and a
         * fixed threshold reported a stall on day 35 of a run that went on to
         * close every ticket it ever saw. The moment the technique stopped
         * being enough is the first time something went past its SLA and was
         * still sitting there. */
        if (stalled_day < 0) {
            Buf st;
            buf_init(&st);
            world_ticket_stats(a.w, &st);
            const char *ob = st.p ? strstr(st.p, "\"open_breached\":") : NULL;
            if (ob && atoi(ob + 16) > 0) stalled_day = a.w->day;
            buf_free(&st);
        }
        if (users_cap && a.w->active >= users_cap) break;
        /* The gate has what it came for once the org is past the second
         * window; running on costs minutes and measures nothing. */
        if (gate && a.w->active > GATE_HI_MAX) break;
    }

    printf("play:  %-3s  %3d  %5d  %6d  %6d  %4d  %3d%%  %9ld  %6ld  %5.0fms\n",
           act, a.w->day, a.w->active, a.w->next_tid, a.w->closed_total, a.w->open_count,
           a.w->closed_total ? ((a.w->closed_total - a.w->breached_total) * 100) / a.w->closed_total : 100,
           act_attempted, act_failed, now_ms() - act_t0);
    (void)act_day0;

    int rc = 0;
    long fail_pct = attempted ? (failed * 100) / attempted : 0;
    printf("play: %ld tickets seen, %ld failed on first attempt (%ld%%), %ld api calls, %ld retries\n",
           attempted, failed, fail_pct, a.calls, a.retries);
    Buf st;
    buf_init(&st);
    world_ticket_stats(a.w, &st);
    printf("play: %s\n", st.p ? st.p : "");
    buf_free(&st);
    if (stalled_day >= 0) printf("play: first missed SLA on day %d\n", stalled_day);
    else                  printf("play: nothing ever went past its SLA\n");

    if (lo_out) { lo_out->seen += lo.seen; lo_out->failed += lo.failed; }
    if (hi_out) { hi_out->seen += hi.seen; hi_out->failed += hi.failed; }

    /* --play IS A GATE TOO, not just a report.
     *
     * §13 asks it to report per-act wall time and where it stalled, and it
     * does. But a reference agent is also a claim -- "a competent script can
     * keep up with this" -- and an unasserted claim decays. If the careful
     * agent starts failing tickets or missing SLAs, either the world got
     * harder than the design says or the agent stopped being competent, and
     * both are worth failing a build over.
     *
     * The thresholds are deliberately loose. This is not measuring the
     * agent's quality; it is noticing when the game stops being winnable by
     * an ordinary good script. */
    int within = a.w->closed_total ? ((a.w->closed_total - a.w->breached_total) * 100) / a.w->closed_total : 100;
    if (!naive && !gate) {
        if (fail_pct > 2) {
            printf("play: FAIL  the reference agent failed %ld%% of tickets; a competent script should not\n", fail_pct);
            rc = 1;
        }
        if (within < 95) {
            printf("play: FAIL  only %d%% of tickets closed within SLA\n", within);
            rc = 1;
        }
        if (!rc) printf("play: PASS  a competent script keeps up\n");
    }

    /* THE WHOLE WORLD, AFTER A WHOLE GAME. This is what the determinism gate
     * compares: not a world that merely grew, but one that was played --
     * every appliance written to, every failure rolled, every retry taken.
     * A replay guarantee over a world nobody touched guarantees very little. */
    if (out_path) {
        Buf dump;
        buf_init(&dump);
        world_dump(a.w, &dump);
        FILE *f = fopen(out_path, "wb");
        if (!f) { fprintf(stderr, "play: cannot write %s\n", out_path); rc = 1; }
        else { fwrite(dump.p, 1, dump.len, f); fclose(f); }
        buf_free(&dump);
    }

    rb_free(tids); rb_free(refs); rb_free(types); rb_free(chase);
    buf_free(&a.out);
    rb_free(a.tried);
    world_free(a.w);
    specs_free(specs);
    return rc;
}

int play_run(uint64_t seed, const char *specdir, int days, bool naive, int users_cap, const char *out_path)
{
    return play_once(seed, specdir, days, naive, false, users_cap, NULL, NULL, out_path);
}

/* One day of unattended running: the systems work the queue, nobody watches. */
/* THE DAY ARRIVES, AND THEN YOU WORK IT -- in that order.
 *
 * The other order looks equivalent and is not. Tickets are raised when the
 * day rolls, so working the queue and then rolling the day leaves every
 * ticket raised that morning sitting open until tomorrow: the queue at any
 * day boundary is exactly one day's intake, and since the company grows 6% a
 * day, that queue GROWS 6% A DAY no matter how good the automation is. The
 * vacation's "queue no deeper at the end than at the start" (§12) would then
 * be unwinnable for a reason that has nothing to do with the player.
 *
 * Day, then work. */
static void vacation_day(Agent *a, Vacation *v)
{
    world_day_advance(a->w);
    int32_t today = a->w->day;

    Placement pl;
    place(a, &pl);
    capacity_ahead(a, &pl);
    ex(a, "ticket.list open %d", 8192);

    /* Take a copy of the day's queue before working it, because working it
     * changes it. */
    Buf queue;
    buf_init(&queue);
    buf_put(&queue, a->out.p ? a->out.p : "", a->out.len);

    int done = 0;
    for (const char *p = queue.p; p && *p && a->w->day == today; ) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && *p == '{') {
            Buf one;
            buf_init(&one);
            buf_put(&one, p, len);
            char tid[RB_ID_MAX] = "", ref[RB_ID_MAX] = "", type[RB_NAME_MAX] = "";
            if (json_str(&one, "id", tid, sizeof tid) && json_str(&one, "ref", ref, sizeof ref)) {
                json_str(&one, "type", type, sizeof type);
                bool ok;
                if (!strcmp(type, "service.capacity")) {
                    ok = capacity_careful(a, tid, ref);
                    place(a, &pl);
                } else if (!strcmp(type, "user.onboard")) {
                    ok = onboard_careful(a, &pl, tid, ref);
                } else {
                    /* A TYPE THE AUTOMATION DOES NOT KNOW ABOUT.
                     * This is the most useful thing the vacation report can
                     * say, and it is why the report names types rather than
                     * counting failures: "your systems have never heard of
                     * offboarding" is actionable, "94%" is not. */
                    vacation_note_unhandled(v, type[0] ? type : "(unknown)");
                    ok = false;
                }
                if (!ok && type[0]) vacation_note_unhandled(v, type);
                if (++done % REPLACE_EVERY == 0) { place(a, &pl); capacity_ahead(a, &pl); }
            }
            buf_free(&one);
        }
        if (!nl) break;
        p = nl + 1;
    }
    buf_free(&queue);

    /* Did anything fall over? Sampled at the day boundary; a service that is
     * over nominal at close of business has been over nominal for a good deal
     * more than the thirty minutes §12 allows. */
    ex(a, "appl.list");
    for (const char *p = a->out.p; p && *p; ) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && *p == '{') {
            Buf one;
            buf_init(&one);
            buf_put(&one, p, len);
            const char *lp = strstr(one.p, "\"load_pct\":");
            int load = lp ? atoi(lp + 11) : 0;
            if (load > 100) {
                v->degraded_minutes += RB_DAY_MINUTES;
                if (load > v->worst_load) {
                    v->worst_load = load;
                    json_str(&one, "id", v->worst_service, RB_ID_MAX);
                }
            }
            buf_free(&one);
        }
        if (!nl) break;
        p = nl + 1;
    }
}

int vacation_run(uint64_t seed, const char *specdir, int days, int at_users)
{
    char serr[RB_ERR_MAX];
    Specs *specs = specs_load(specdir, serr, sizeof serr);
    if (!specs) { printf("vacation: FAIL  specs do not load: %s\n", serr); return 1; }

    Agent a;
    memset(&a, 0, sizeof a);
    a.w = world_new(seed, specs);
    buf_init(&a.out);
    proto_open(&a.s, a.w);
    a.s.prov = PROV_SYSTEM;   /* on vacation, everything is done by a system */

    printf("vacation: growing the org to %d users first\n", at_users);
    for (int d = 0; d < 400 && a.w->active < at_users; d++) {
        Vacation ignore;
        memset(&ignore, 0, sizeof ignore);
        vacation_day(&a, &ignore);
    }
    if (a.w->active < at_users) {
        printf("vacation: FAIL  the org never reached %d users\n", at_users);
        buf_free(&a.out); world_free(a.w); specs_free(specs);
        return 1;
    }

    Vacation v;
    memset(&v, 0, sizeof v);
    v.open_start = a.w->open_count;
    int32_t closed_start = a.w->closed_total, breached_start = a.w->breached_total;
    int32_t day_start = a.w->day;

    printf("vacation: %d days, %d users, nobody watching. Day %d.\n",
           days, a.w->active, a.w->day);
    for (int d = 0; d < days; d++) vacation_day(&a, &v);

    v.open_end = a.w->open_count;
    int closed = a.w->closed_total - closed_start;
    int breached = a.w->breached_total - breached_start;
    v.within_pct = closed ? ((closed - breached) * 100) / closed : 100;

    printf("vacation: day %d to %d — %d users, %d tickets closed\n",
           day_start, a.w->day, a.w->active, closed);

    int fails = 0;
    bool sla_ok   = v.within_pct >= 99;
    bool queue_ok = v.open_end <= v.open_start;
    bool svc_ok   = v.degraded_minutes <= 30;
    if (!sla_ok)   fails++;
    if (!queue_ok) fails++;
    if (!svc_ok)   fails++;

    printf("vacation: %s  %d%% of tickets resolved within SLA (want 99%%)\n",
           sla_ok ? "PASS" : "FAIL", v.within_pct);
    printf("vacation: %s  queue %d deep at the start, %d at the end\n",
           queue_ok ? "PASS" : "FAIL", v.open_start, v.open_end);
    if (svc_ok)
        printf("vacation: PASS  no service went over nominal capacity\n");
    else
        printf("vacation: FAIL  %s ran at %d%% of nominal for about %d in-game minutes (30 allowed)\n",
               v.worst_service[0] ? v.worst_service : "a service", v.worst_load, v.degraded_minutes);

    /* THE DIAGNOSTIC HALF. Failing is not fatal; it is a list of things to go
     * and automate. */
    if (v.nunhandled) {
        printf("vacation: your systems did not handle these ticket types:\n");
        for (int i = 0; i < v.nunhandled; i++) printf("vacation:   %s\n", v.unhandled[i]);
    }
    if (fails)
        printf("vacation: not this time. Fix what the report names and go again — nothing is lost.\n");
    else
        printf("vacation: the company ran itself for %d days. Go on holiday.\n", days);

    buf_free(&a.out);
    world_free(a.w);
    specs_free(specs);
    return fails ? 1 : 0;
}

/* SEVERAL SEEDS, SUMMED, because one run is not a measurement.
 *
 * A single run samples about 140 tickets in each window. A true 42% rate on
 * 140 samples comes out anywhere between 34% and 50% two times in twenty, and
 * a gate that fails one run in ten for no reason is a gate people re-run
 * until it is green -- which is the same as no gate, only slower to notice.
 *
 * Three seeds is the cheapest thing that makes the band a statement about the
 * design rather than about the dice. It costs about a second. */
#define GATE_SEEDS 3

int naive_gate_run(uint64_t seed, const char *specdir, int days)
{
    Band lo = { 0, 0 }, hi = { 0, 0 };
    for (int i = 0; i < GATE_SEEDS; i++)
        play_once(seed + (uint64_t)i, specdir, days, false, true, 0, &lo, &hi, NULL);

    int fails = 0;
    printf("naive: the §8 degeneracy band, over %d seeds\n", GATE_SEEDS);
    band_report("at the Act I wall (~150 users)",     &lo, -1, 5,  &fails);
    band_report("at the end of Act II (~800 users)",  &hi, 35, -1, &fails);
    if (!fails)
        printf("naive: the exceptions do their job -- a script that does not branch keeps up early and does not later\n");
    else
        printf("naive: Act II is the whole bet (§17). If this band is wrong, the game is a form-filling simulator.\n");
    return fails ? 1 : 0;
}
