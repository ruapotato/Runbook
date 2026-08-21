/* cpu.c — an RV64IM interpreter with no undefined behaviour anywhere.
 *
 * Correctness rules this file follows, all of them for determinism rather
 * than pedantry:
 *
 *   - every arithmetic result is computed in uint64_t and reinterpreted,
 *     because signed overflow in C is undefined and undefined means the
 *     optimiser may produce different answers at -O0 and -O2. That is exactly
 *     the failure the determinism gate exists to catch.
 *   - shifts always mask their amount, so there is no shift-past-width UB.
 *   - division by zero and the signed overflow case have the values the RISC-V
 *     spec defines, rather than trapping the host.
 *   - every load and store is bounds-checked before it happens.
 *   - anything not decoded traps. There is no fallthrough.
 */
#include <string.h>
#include <stdio.h>
#include "nom.h"
#include "cpu.h"

const char *cpu_trap_name(CpuTrap t)
{
    switch (t) {
    case TRAP_NONE:        return "none";
    case TRAP_EXIT:        return "exit";
    case TRAP_ILLEGAL:     return "illegal instruction";
    case TRAP_FETCH_FAULT: return "instruction fetch fault";
    case TRAP_LOAD_FAULT:  return "load fault";
    case TRAP_STORE_FAULT: return "store fault";
    case TRAP_MISALIGNED:  return "misaligned access";
    case TRAP_EBREAK:      return "breakpoint";
    case TRAP_BUDGET:      return "budget";
    case TRAP_HOST:        return "host refused";
    default:               return "?";
    }
}

void cpu_init(Cpu *c)
{
    memset(c, 0, sizeof *c);
    c->memsz = CPU_MEM_BYTES;
    c->mem   = nom_alloc(c->memsz);
    memset(c->mem, 0, c->memsz);       /* reads-as-zero is a guarantee */
    c->x[2]  = CPU_STACK_TOP;          /* sp */
}

void cpu_free(Cpu *c)
{
    nom_free(c->mem);
    c->mem = NULL;
}

bool cpu_read(Cpu *c, uint64_t addr, void *dst, size_t n)
{
    if (addr > c->memsz || n > c->memsz - addr) return false;
    memcpy(dst, c->mem + addr, n);
    return true;
}

bool cpu_write(Cpu *c, uint64_t addr, const void *src, size_t n)
{
    if (addr > c->memsz || n > c->memsz - addr) return false;
    memcpy(c->mem + addr, src, n);
    return true;
}

/* ------------------------------------------------------------- ELF load -- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p)
{ return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

#define EM_RISCV 243

bool cpu_load_elf(Cpu *c, const uint8_t *elf, size_t len, char *err, size_t errsz)
{
    if (len < 64 || memcmp(elf, "\x7f" "ELF", 4) != 0) {
        snprintf(err, errsz, "not an ELF image");
        return false;
    }
    if (elf[4] != 2) { snprintf(err, errsz, "not a 64-bit ELF"); return false; }
    if (elf[5] != 1) { snprintf(err, errsz, "not little-endian");  return false; }
    if (rd16(elf + 18) != EM_RISCV) {
        snprintf(err, errsz, "wrong machine: this cpu executes rv64, image wants %u",
                 rd16(elf + 18));
        return false;
    }

    uint64_t entry  = rd64(elf + 24);
    uint64_t phoff  = rd64(elf + 32);
    uint16_t phsz   = rd16(elf + 54);
    uint16_t phnum  = rd16(elf + 56);
    if (phsz < 56 || phoff > len || (uint64_t)phnum * phsz > len - phoff) {
        snprintf(err, errsz, "program headers are outside the file");
        return false;
    }

    int loaded = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + phoff + (uint64_t)i * phsz;
        if (rd32(ph) != 1) continue;                      /* PT_LOAD only */
        uint64_t off   = rd64(ph + 8);
        uint64_t vaddr = rd64(ph + 16);
        uint64_t fsz   = rd64(ph + 32);
        uint64_t msz   = rd64(ph + 40);
        if (off > len || fsz > len - off) {
            snprintf(err, errsz, "segment %u runs past the end of the file", i);
            return false;
        }
        if (vaddr > c->memsz || msz > c->memsz - vaddr) {
            snprintf(err, errsz,
                     "segment %u wants 0x%llx..0x%llx, machine has %llu bytes",
                     i, (unsigned long long)vaddr,
                     (unsigned long long)(vaddr + msz),
                     (unsigned long long)c->memsz);
            return false;
        }
        memcpy(c->mem + vaddr, elf + off, fsz);
        /* .bss is already zero: cpu_init guarantees it */
        loaded++;
    }
    if (!loaded) { snprintf(err, errsz, "no loadable segments"); return false; }
    if (entry >= c->memsz || (entry & 1)) {
        snprintf(err, errsz, "entry point 0x%llx is not executable",
                 (unsigned long long)entry);
        return false;
    }
    c->pc = entry;
    return true;
}

/* Read the .nomneed section. Real section-header parsing: the loader has to
 * find it in the file the same way anything else would, so a truncated or
 * mangled binary simply has no readable dependency list. */
bool cpu_elf_needs(const uint8_t *elf, size_t len, char *out, size_t outsz)
{
    if (out && outsz) out[0] = '\0';
    if (len < 64 || memcmp(elf, "\x7f" "ELF", 4) != 0) return false;

    uint64_t shoff = rd64(elf + 40);
    uint16_t shent = rd16(elf + 58);
    uint16_t shnum = rd16(elf + 60);
    uint16_t shstr = rd16(elf + 62);
    if (shent < 64 || shoff >= len || shnum == 0) return false;
    if ((uint64_t)shnum * shent > len - shoff) return false;
    if (shstr >= shnum) return false;

    const uint8_t *sh = elf + shoff;
    const uint8_t *strsec = sh + (uint64_t)shstr * shent;
    uint64_t stroff = rd64(strsec + 24), strsz = rd64(strsec + 32);
    if (stroff >= len || strsz > len - stroff) return false;

    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *s = sh + (uint64_t)i * shent;
        uint32_t nameoff = rd32(s);
        if (nameoff >= strsz) continue;
        const char *nm = (const char *)(elf + stroff + nameoff);
        if (strncmp(nm, ".nomneed", 9) != 0) continue;
        uint64_t off = rd64(s + 24), sz = rd64(s + 32);
        if (off >= len || sz > len - off) return false;
        if (sz >= outsz) sz = outsz - 1;
        memcpy(out, elf + off, sz);
        out[sz] = '\0';
        return true;
    }
    return false;
}

/* ----------------------------------------------------------- execution -- */

/* Sign-extend the low `bits` of v. Done in unsigned arithmetic throughout. */
static int64_t sext(uint64_t v, int bits)
{
    uint64_t m = 1ull << (bits - 1);
    v &= (bits == 64) ? ~0ull : ((1ull << bits) - 1);
    return (int64_t)((v ^ m) - m);
}

#define RD(i)     ((int)(((i) >> 7)  & 0x1f))
#define RS1(i)    ((int)(((i) >> 15) & 0x1f))
#define RS2(i)    ((int)(((i) >> 20) & 0x1f))
#define F3(i)     ((int)(((i) >> 12) & 0x7))
#define F7(i)     ((int)(((i) >> 25) & 0x7f))

static void setreg(Cpu *c, int r, uint64_t v) { if (r) c->x[r] = v; }

static bool load_mem(Cpu *c, uint64_t addr, int bytes, uint64_t *out)
{
    if (addr > c->memsz || (uint64_t)bytes > c->memsz - addr) {
        c->trap = TRAP_LOAD_FAULT; c->trap_addr = addr; return false;
    }
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) v |= (uint64_t)c->mem[addr + i] << (8 * i);
    *out = v;
    return true;
}

static bool store_mem(Cpu *c, uint64_t addr, int bytes, uint64_t v)
{
    if (addr > c->memsz || (uint64_t)bytes > c->memsz - addr) {
        c->trap = TRAP_STORE_FAULT; c->trap_addr = addr; return false;
    }
    for (int i = 0; i < bytes; i++) c->mem[addr + i] = (uint8_t)(v >> (8 * i));
    return true;
}

CpuTrap cpu_run(Cpu *c, uint64_t budget)
{
    c->trap = TRAP_NONE;

    for (uint64_t n = 0; n < budget; n++) {
        /* ---- fetch ---- */
        if (c->pc + 4 > c->memsz || (c->pc & 3)) {
            c->trap = TRAP_FETCH_FAULT; c->trap_addr = c->pc; return c->trap;
        }
        uint32_t ins = rd32(c->mem + c->pc);
        uint64_t next = c->pc + 4;
        int op = (int)(ins & 0x7f);
        c->icount++;

        switch (op) {

        case 0x37:  /* lui */
            setreg(c, RD(ins), (uint64_t)sext((uint64_t)(ins & 0xfffff000), 32));
            break;

        case 0x17:  /* auipc */
            setreg(c, RD(ins), c->pc + (uint64_t)sext((uint64_t)(ins & 0xfffff000), 32));
            break;

        case 0x6f: { /* jal */
            uint64_t imm = ((uint64_t)((ins >> 21) & 0x3ff) << 1)
                         | ((uint64_t)((ins >> 20) & 0x1) << 11)
                         | ((uint64_t)((ins >> 12) & 0xff) << 12)
                         | ((uint64_t)((ins >> 31) & 0x1) << 20);
            setreg(c, RD(ins), next);
            next = c->pc + (uint64_t)sext(imm, 21);
            break;
        }

        case 0x67: { /* jalr */
            if (F3(ins) != 0) { c->trap = TRAP_ILLEGAL; c->trap_addr = ins; return c->trap; }
            uint64_t t = (c->x[RS1(ins)] + (uint64_t)sext(ins >> 20, 12)) & ~1ull;
            setreg(c, RD(ins), next);
            next = t;
            break;
        }

        case 0x63: { /* branches */
            uint64_t imm = ((uint64_t)((ins >> 8)  & 0xf) << 1)
                         | ((uint64_t)((ins >> 25) & 0x3f) << 5)
                         | ((uint64_t)((ins >> 7)  & 0x1) << 11)
                         | ((uint64_t)((ins >> 31) & 0x1) << 12);
            int64_t  a = (int64_t)c->x[RS1(ins)], b = (int64_t)c->x[RS2(ins)];
            uint64_t ua = c->x[RS1(ins)], ub = c->x[RS2(ins)];
            bool take;
            switch (F3(ins)) {
            case 0: take = (ua == ub); break;             /* beq  */
            case 1: take = (ua != ub); break;             /* bne  */
            case 4: take = (a  <  b);  break;             /* blt  */
            case 5: take = (a  >= b);  break;             /* bge  */
            case 6: take = (ua <  ub); break;             /* bltu */
            case 7: take = (ua >= ub); break;             /* bgeu */
            default: c->trap = TRAP_ILLEGAL; c->trap_addr = ins; return c->trap;
            }
            if (take) next = c->pc + (uint64_t)sext(imm, 13);
            break;
        }

        case 0x03: { /* loads */
            uint64_t addr = c->x[RS1(ins)] + (uint64_t)sext(ins >> 20, 12);
            uint64_t v;
            switch (F3(ins)) {
            case 0: if (!load_mem(c, addr, 1, &v)) return c->trap;
                    setreg(c, RD(ins), (uint64_t)sext(v, 8));  break;   /* lb  */
            case 1: if (addr & 1) { c->trap = TRAP_MISALIGNED; c->trap_addr = addr; return c->trap; }
                    if (!load_mem(c, addr, 2, &v)) return c->trap;
                    setreg(c, RD(ins), (uint64_t)sext(v, 16)); break;   /* lh  */
            case 2: if (addr & 3) { c->trap = TRAP_MISALIGNED; c->trap_addr = addr; return c->trap; }
                    if (!load_mem(c, addr, 4, &v)) return c->trap;
                    setreg(c, RD(ins), (uint64_t)sext(v, 32)); break;   /* lw  */
            case 3: if (addr & 7) { c->trap = TRAP_MISALIGNED; c->trap_addr = addr; return c->trap; }
                    if (!load_mem(c, addr, 8, &v)) return c->trap;
                    setreg(c, RD(ins), v);                     break;   /* ld  */
            case 4: if (!load_mem(c, addr, 1, &v)) return c->trap;
                    setreg(c, RD(ins), v);                     break;   /* lbu */
            case 5: if (addr & 1) { c->trap = TRAP_MISALIGNED; c->trap_addr = addr; return c->trap; }
                    if (!load_mem(c, addr, 2, &v)) return c->trap;
                    setreg(c, RD(ins), v);                     break;   /* lhu */
            case 6: if (addr & 3) { c->trap = TRAP_MISALIGNED; c->trap_addr = addr; return c->trap; }
                    if (!load_mem(c, addr, 4, &v)) return c->trap;
                    setreg(c, RD(ins), v);                     break;   /* lwu */
            default: c->trap = TRAP_ILLEGAL; c->trap_addr = ins; return c->trap;
            }
            break;
        }

        case 0x23: { /* stores */
            uint64_t imm = ((uint64_t)F7(ins) << 5) | (uint64_t)RD(ins);
            uint64_t addr = c->x[RS1(ins)] + (uint64_t)sext(imm, 12);
            uint64_t v = c->x[RS2(ins)];
            int bytes;
            switch (F3(ins)) {
            case 0: bytes = 1; break;
            case 1: bytes = 2; break;
            case 2: bytes = 4; break;
            case 3: bytes = 8; break;
            default: c->trap = TRAP_ILLEGAL; c->trap_addr = ins; return c->trap;
            }
            if (bytes > 1 && (addr & (uint64_t)(bytes - 1))) {
                c->trap = TRAP_MISALIGNED; c->trap_addr = addr; return c->trap;
            }
            if (!store_mem(c, addr, bytes, v)) return c->trap;
            break;
        }

        case 0x13: { /* op-imm */
            uint64_t a = c->x[RS1(ins)];
            int64_t  imm = sext(ins >> 20, 12);
            uint64_t r;
            switch (F3(ins)) {
            case 0: r = a + (uint64_t)imm; break;                        /* addi  */
            case 2: r = ((int64_t)a < imm) ? 1 : 0; break;               /* slti  */
            case 3: r = (a < (uint64_t)imm) ? 1 : 0; break;              /* sltiu */
            case 4: r = a ^ (uint64_t)imm; break;                        /* xori  */
            case 6: r = a | (uint64_t)imm; break;                        /* ori   */
            case 7: r = a & (uint64_t)imm; break;                        /* andi  */
            case 1: if ((ins >> 26) != 0) { c->trap = TRAP_ILLEGAL; c->trap_addr = ins; return c->trap; }
                    r = a << (ins >> 20 & 63); break;                    /* slli  */
            case 5: {
                unsigned sh = (ins >> 20) & 63;
                unsigned top = ins >> 26;
                if (top == 0)       r = a >> sh;                          /* srli */
                else if (top == 0x10) r = (uint64_t)((int64_t)a >> sh);   /* srai */
                else { c->trap = TRAP_ILLEGAL; c->trap_addr = ins; return c->trap; }
                break;
            }
            default: c->trap = TRAP_ILLEGAL; c->trap_addr = ins; return c->trap;
            }
            setreg(c, RD(ins), r);
            break;
        }

        case 0x1b: { /* op-imm-32 */
            uint32_t a = (uint32_t)c->x[RS1(ins)];
            int64_t  imm = sext(ins >> 20, 12);
            uint32_t r;
            switch (F3(ins)) {
            case 0: r = a + (uint32_t)imm; break;                        /* addiw */
            case 1: if ((ins >> 25) != 0) { c->trap = TRAP_ILLEGAL; c->trap_addr = ins; return c->trap; }
                    r = a << ((ins >> 20) & 31); break;                  /* slliw */
            case 5: {
                unsigned sh = (ins >> 20) & 31;
                unsigned top = ins >> 25;
                if (top == 0)         r = a >> sh;                        /* srliw */
                else if (top == 0x20) r = (uint32_t)((int32_t)a >> sh);   /* sraiw */
                else { c->trap = TRAP_ILLEGAL; c->trap_addr = ins; return c->trap; }
                break;
            }
            default: c->trap = TRAP_ILLEGAL; c->trap_addr = ins; return c->trap;
            }
            setreg(c, RD(ins), (uint64_t)sext(r, 32));
            break;
        }

        case 0x33: { /* op */
            uint64_t a = c->x[RS1(ins)], b = c->x[RS2(ins)];
            int64_t sa = (int64_t)a, sb = (int64_t)b;
            uint64_t r;
            if (F7(ins) == 1) {                       /* M extension */
                switch (F3(ins)) {
                /* MULTIPLY IN UNSIGNED, BECAUSE SIGNED OVERFLOW IS UNDEFINED
                 * AND RISC-V `mul` IS NOT. The spec says mul returns the low
                 * 64 bits of the product and never traps -- wrapping is the
                 * defined behaviour. Written as `sa * sb` that is C undefined
                 * behaviour the moment a guest multiplies two large numbers,
                 * which UBSan duly reports and a sufficiently clever compiler
                 * is entitled to act on. Unsigned wrapping IS defined, and
                 * the low 64 bits are identical in two's complement. */
                case 0: r = a * b; break;                                /* mul */
                case 1: { /* mulh: signed high 64 of a 128-bit product */
                    __int128 p = (__int128)sa * (__int128)sb;
                    r = (uint64_t)((unsigned __int128)p >> 64); break;
                }
                case 2: { /* mulhsu */
                    __int128 p = (__int128)sa * (__int128)(unsigned __int128)b;
                    r = (uint64_t)((unsigned __int128)p >> 64); break;
                }
                case 3: { /* mulhu */
                    unsigned __int128 p = (unsigned __int128)a * (unsigned __int128)b;
                    r = (uint64_t)(p >> 64); break;
                }
                /* The spec defines these edge cases; defining them here is
                 * what keeps a divide-by-zero in guest code from being a
                 * host-dependent event. */
                case 4: r = (b == 0) ? ~0ull
                          : (sa == INT64_MIN && sb == -1) ? (uint64_t)sa
                          : (uint64_t)(sa / sb); break;                  /* div  */
                case 5: r = (b == 0) ? ~0ull : (uint64_t)(a / b); break; /* divu */
                case 6: r = (b == 0) ? (uint64_t)sa
                          : (sa == INT64_MIN && sb == -1) ? 0
                          : (uint64_t)(sa % sb); break;                  /* rem  */
                case 7: r = (b == 0) ? a : (uint64_t)(a % b); break;     /* remu */
                default: c->trap = TRAP_ILLEGAL; c->trap_addr = ins; return c->trap;
                }
            } else if (F7(ins) == 0 || F7(ins) == 0x20) {
                bool alt = (F7(ins) == 0x20);
                switch (F3(ins)) {
                case 0: r = alt ? a - b : a + b; break;                  /* add/sub */
                case 1: if (alt) goto illegal; r = a << (b & 63); break; /* sll */
                case 2: if (alt) goto illegal; r = (sa < sb) ? 1 : 0; break;
                case 3: if (alt) goto illegal; r = (a < b) ? 1 : 0; break;
                case 4: if (alt) goto illegal; r = a ^ b; break;
                case 5: r = alt ? (uint64_t)(sa >> (b & 63)) : (a >> (b & 63)); break;
                case 6: if (alt) goto illegal; r = a | b; break;
                case 7: if (alt) goto illegal; r = a & b; break;
                default: goto illegal;
                }
            } else {
                goto illegal;
            }
            setreg(c, RD(ins), r);
            break;
        }

        case 0x3b: { /* op-32 */
            uint32_t a = (uint32_t)c->x[RS1(ins)], b = (uint32_t)c->x[RS2(ins)];
            int32_t sa = (int32_t)a, sb = (int32_t)b;
            uint32_t r;
            if (F7(ins) == 1) {
                switch (F3(ins)) {
                /* Same as `mul` above: wrapping is defined for mulw and
                 * undefined for the C multiply that was implementing it. */
                case 0: r = (uint32_t)((uint32_t)a * (uint32_t)b); break; /* mulw  */
                case 4: r = (b == 0) ? 0xffffffffu
                          : (sa == INT32_MIN && sb == -1) ? (uint32_t)sa
                          : (uint32_t)(sa / sb); break;                  /* divw  */
                case 5: r = (b == 0) ? 0xffffffffu : a / b; break;       /* divuw */
                case 6: r = (b == 0) ? (uint32_t)sa
                          : (sa == INT32_MIN && sb == -1) ? 0
                          : (uint32_t)(sa % sb); break;                  /* remw  */
                case 7: r = (b == 0) ? a : a % b; break;                 /* remuw */
                default: goto illegal;
                }
            } else if (F7(ins) == 0 || F7(ins) == 0x20) {
                bool alt = (F7(ins) == 0x20);
                switch (F3(ins)) {
                case 0: r = alt ? a - b : a + b; break;                  /* addw/subw */
                case 1: if (alt) goto illegal; r = a << (b & 31); break;
                case 5: r = alt ? (uint32_t)(sa >> (b & 31)) : (a >> (b & 31)); break;
                default: goto illegal;
                }
            } else {
                goto illegal;
            }
            setreg(c, RD(ins), (uint64_t)sext(r, 32));
            break;
        }

        case 0x0f:  /* fence: this machine has one hart and no store buffer */
            break;

        case 0x73: { /* system */
            if (ins == 0x00000073) {          /* ecall */
                if (!c->syscall) { c->trap = TRAP_HOST; return c->trap; }
                /* see Cpu.charge in cpu.h */
                c->charge = 0;
                int64_t r = c->syscall(c, (int64_t)c->x[17], (int64_t)c->x[10],
                                       (int64_t)c->x[11], (int64_t)c->x[12], c->ctx);
                if (c->trap != TRAP_NONE) { c->pc = next; return c->trap; }
                c->x[10] = (uint64_t)r;
                /* And what it cost. Charged against this run's budget, so an
                 * expensive syscall ends the slice early rather than being
                 * free. */
                if (c->charge) {
                    if (c->charge >= budget - n) { n = budget - 1; }
                    else n += c->charge;
                    c->icount += c->charge;
                    c->charge = 0;
                }
            } else if (ins == 0x00100073) {   /* ebreak */
                c->trap = TRAP_EBREAK; c->pc = next; return c->trap;
            } else {
                goto illegal;
            }
            break;
        }

        default:
        illegal:
            c->trap = TRAP_ILLEGAL;
            c->trap_addr = ins;
            return c->trap;
        }

        c->x[0] = 0;      /* x0 stays zero no matter what wrote to it */
        c->pc = next;
    }

    c->trap = TRAP_BUDGET;
    return c->trap;
}
