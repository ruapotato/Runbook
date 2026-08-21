/* kernel.h — running guest programs against a machine's filesystem. */
#ifndef NOM_KERNEL_H
#define NOM_KERNEL_H

typedef struct Proc Proc;

/* Load and run the program at `path` to completion, on its own CPU, with the
 * machine's disk as its filesystem and `console` as its stdout. Returns the
 * program's exit code, or one of the negative SPAWN_* codes from abi.h.
 * `err` receives a human-readable reason when the program could not be run or
 * faulted -- that string is boot console output, so it must read like a
 * machine talking, not like a debugger. */
int64_t kernel_spawn(Machine *m, const char *path, const char *arg,
                     Buf *console, int depth, char *err, size_t errsz);

/* Same, but with a parent process, so the child inherits its namespace and
 * working directory. */
int64_t kernel_spawn_p(Machine *m, const char *path, const char *arg,
                       Buf *console, int depth, Proc *parent,
                       char *err, size_t errsz);

/* Run one shell command line as the persistent session, so cd and bind stick.
 * This is the ONLY entry point the terminal, the socket and the desktop use,
 * so none of them can diverge from the others. */
int64_t kernel_run(Machine *m, const char *line, Buf *console);
/* What the drives are, read out of band. The one question a service processor
 * can answer about a machine that never reached a shell -- see the comment on
 * the definition for why this one and nothing else. */
void kernel_sp_blkid(Machine *m, Buf *out);
void kernel_no_shell(Buf *out);
/* One stage of a pipeline: `in` becomes the child's stdin, its stdout is
 * captured into `out`. */
int64_t kernel_spawn_piped(Machine *m, const char *path, const char *arg,
                           Buf *console, int depth, Proc *parent,
                           Buf *in, Buf *out);
ProcInfo *kernel_session(Machine *m);

/* THE HOST'S ANSWER TO SYS_rbapi. RUNBOOK sets this to something that runs
 * one line of the game's protocol; anything else leaves it NULL and guest
 * programs that ask get an honest failure. */
extern void (*rb_api_hook)(Machine *m, const char *line, Buf *out);

/* The TCP bench: the entire game, with no GUI in the process at all. */
int bench_serve(int port, bool verbose, uint64_t seed0);

#endif /* NOM_KERNEL_H */
