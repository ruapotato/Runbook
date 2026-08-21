/* cpu.h — the machine NOMINAL actually runs on.
 *
 * D18. This is our CPU. We define the platform: the memory map, the syscall
 * ABI, the trap behaviour, and — the part that matters most — the determinism
 * guarantees. The instruction encoding is RV64IM, which is a published,
 * stable, deliberately small standard. Using it is not a compromise on "our
 * own CPU"; it means an existing compiler can target our machine today, so
 * the emulator can be validated against a reference instead of against my
 * opinion, and Adder's third backend has a documented target to lower to.
 *
 * WHAT IS DELIBERATELY ABSENT, and why:
 *
 *   no floating point   — the F/D extensions are the single largest source of
 *                         cross-platform result divergence. Integer-only means
 *                         determinism is structural rather than maintained.
 *                         Code that needs reals uses soft-float, which is
 *                         exact and reproducible everywhere.
 *   no cycle counter    — rdtsc-alikes leak host timing into guest results.
 *                         The instruction count IS readable, because it is a
 *                         deterministic function of the program.
 *   no randomness       — there is no instruction that produces entropy.
 *   no uninitialised    — all memory reads as zero until written.
 *   no undefined        — every encoding either executes or traps. There is
 *                         no "unpredictable" in this machine.
 *
 * Two runs of the same image with the same input produce the same trace, the
 * same output and the same instruction count, on every platform we ship to.
 * That is a testable claim and it is tested.
 */
#ifndef NOM_CPU_H
#define NOM_CPU_H

/* Guest physical memory is one flat span. There is no MMU yet: a program is
 * loaded where its ELF asks to be loaded and addresses are physical. Paging is
 * a later concern and does not change the ISA. */
/* 4 MB. The largest guest program is about 14 KB and the stack is tiny, so
 * this is generous -- and it has to be, because every LONG-LIVED daemon keeps
 * its own machine alive for the whole boot. At 16 MB a dozen daemons cost
 * 192 MB of host memory for nothing. */
#define CPU_MEM_BYTES  (1024u * 1024u)
#define CPU_STACK_TOP  (CPU_MEM_BYTES - 64u)

typedef enum {
    TRAP_NONE = 0,
    TRAP_EXIT,            /* the program called exit                    */
    TRAP_ILLEGAL,         /* an encoding this machine does not define   */
    TRAP_FETCH_FAULT,     /* pc outside memory, or misaligned           */
    TRAP_LOAD_FAULT,      /* data read outside memory                   */
    TRAP_STORE_FAULT,     /* data write outside memory                  */
    TRAP_MISALIGNED,      /* unaligned data access                      */
    TRAP_EBREAK,          /* breakpoint                                 */
    TRAP_BUDGET,          /* instruction budget exhausted (not an error)*/
    TRAP_HOST,            /* the host refused a syscall                 */
} CpuTrap;

const char *cpu_trap_name(CpuTrap t);

typedef struct Cpu Cpu;

/* A syscall. Returns the value to place in a0. The host decides everything
 * about the outside world; the CPU has no other channel to it, which is what
 * makes the sandbox structural rather than a policy. */
typedef int64_t (*CpuSyscall)(Cpu *c, int64_t num, int64_t a0, int64_t a1,
                              int64_t a2, void *ctx);

struct Cpu {
    uint64_t   x[32];          /* x0 is hardwired to zero */
    uint64_t   pc;
    uint8_t   *mem;
    uint64_t   memsz;

    uint64_t   icount;         /* instructions retired — deterministic */
    /* RUNBOOK: WHAT A SYSCALL COSTS, in instructions.
     *
     * A host syscall does real work and the guest pays four instructions to
     * ask for it, so an expensive one is free from the scheduler's point of
     * view -- and a `while True` polling loop ran three and a half thousand
     * times inside a single tick, which is a script with the brakes off.
     *
     * A syscall handler adds to this and cpu_run charges it against the
     * budget, so a call into the game costs what it is worth. Deterministic,
     * like everything else here: the same script pays the same price on every
     * platform, every run. */
    uint64_t   charge;
    CpuTrap    trap;
    uint64_t   trap_addr;      /* the address or encoding that caused it */
    int64_t    exit_code;

    CpuSyscall syscall;
    void      *ctx;

    Buf       *out;            /* where guest writes to fd 1 and 2 land */
};

void cpu_init(Cpu *c);
void cpu_free(Cpu *c);

/* Load a static RV64 ELF into memory and set pc to its entry. Returns false
 * and fills `err` if the file is not something this machine can run — which is
 * itself a diagnosable failure, since a corrupted binary lands here. */
bool cpu_load_elf(Cpu *c, const uint8_t *elf, size_t len, char *err, size_t errsz);

/* What a binary was linked against, read out of its .nomneed section: one
 * "<soname> <version>" line each. Returns false if the image has no such
 * section, which means it declares no dependencies. */
bool cpu_elf_needs(const uint8_t *elf, size_t len, char *out, size_t outsz);

/* Run at most `budget` instructions. Returns the trap that stopped it;
 * TRAP_BUDGET means it is still live and can be resumed. */
CpuTrap cpu_run(Cpu *c, uint64_t budget);

/* Guest memory access, bounds-checked, for the host side of syscalls. */
bool cpu_read(Cpu *c, uint64_t addr, void *dst, size_t n);
bool cpu_write(Cpu *c, uint64_t addr, const void *src, size_t n);

#endif /* NOM_CPU_H */
