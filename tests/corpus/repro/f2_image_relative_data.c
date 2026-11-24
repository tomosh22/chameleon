/* F-2 (docs/wp/WP-26.md; fixed by WP-36, ADR-0042). Replaces f2_global_array_index.c, which did NOT reproduce the finding:
 * for one global array indexed by a variable cl emits `lea rax,[table]` (REL32), which always worked.
 *
 * cl addresses data image-relatively only in a function that keeps __ImageBase in a register:
 *   - a function with a switch jump table that also touches globals (raylib 5.5's PlayAutomationEvent in rcore.c), or
 *   - a function that indexes two or more global arrays.
 * With cl 19.4x /O2 /Ob0 /Gy (and /O1) `mixed` below compiles to (spike run 45402685481):
 *     lea  rdi,[__ImageBase]                          ; REL32 __ImageBase
 *     mov  eax,dword ptr $LN16[rdi+rax*4]             ; 8B 84 87 disp32      ADDR32NB $LN16 (the jump table, patch-local)
 *     add  rax,rdi / jmp rax
 *     mov  byte ptr CORE[rbx+rdi],0                   ; C6 84 3B disp32 00   ADDR32NB CORE
 *     cmp  byte ptr CORE[rbx+rdi+200h],0              ; 80 BC 3B disp32 00   ADDR32NB CORE (addend 200h in place)
 *     mov  dword ptr CORE[rdi+rax*4+400h],ebx         ; 89 9C 87 disp32      ADDR32NB CORE
 *     mov  dword ptr sarr[rdi+rcx*4],eax              ; 89 84 8F disp32      ADDR32NB sarr (a static)
 *     mov  rax,qword ptr fptab[rdi+rax*8]             ; 48 8B 84 C7 disp32   ADDR32NB fptab
 *     cvttss2si eax,dword ptr CORE[rdi+rax*4+444h]    ; F3 0F 2C 84 87 disp32  ADDR32NB CORE
 *   $LN16: the jump table, in the function's own .text$mn COMDAT (in .rdata with /jumptablerdata), entries ADDR32NB $LNn
 * With /Od there is no ADDR32NB to data, and clang-cl emits none (its jump tables are table-relative).
 *
 * Before WP-36 any edit of this translation unit was refused with CML3101 ("references 'CORE' in <module> image-relatively
 * (ADDR32NB); this cannot be retargeted"), whichever function was edited. Now each such site is proved to be the
 * sign-extended displacement of a memory operand and retargeted to the running image's data (fix-up kind Addr32NbDisp);
 * the broker log shows `WP36-IMGREL sites=N verified=N`. The fixture tests/fixtures/imgrel is this file with a driver, and
 * tests/harness/scenarios/imgrel.toml is the acceptance. Edit anything in this file and `reload`. */
struct Core { char keys[512]; char prev[512]; int q[16]; int count; float f[32]; };
struct Core CORE;
static int sarr[64];
int (*fptab[8])(int);
int g(int);

int mixed(int type, int p) {
    switch (type) {
        case 0: CORE.keys[p] = 0; break;
        case 1: CORE.keys[p] = 1; if (CORE.prev[p] == 0 && CORE.count < 16) { CORE.q[CORE.count] = p; CORE.count++; } break;
        case 2: sarr[p & 63] = g(p); break;
        case 3: return g(p + 1);
        case 4: return g(p + 2);
        case 5: return g(p * 3);
        case 6: return sarr[p & 63] + 1;
        case 7: return fptab[p & 7](p);
        case 8: return (int)CORE.f[p & 31];
        default: break;
    }
    return 0;
}

/* Two global arrays, no jump table: image-relative as well. */
long long wide(long long i) { return (long long)sarr[i & 63] + (long long)CORE.q[i & 15]; }
