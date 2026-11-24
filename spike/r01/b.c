/* mixed: jump table + global arrays indexed by a variable + &__ImageBase */
extern char __ImageBase;
struct Core { char keys[512]; char prev[512]; int q[16]; int count; float f[32]; };
struct Core CORE;
static int sarr[64];
int (*fptab[8])(int);
int g(int);
int mixed(int type, int p) {
    switch (type) {
        case 0: CORE.keys[p] = 0; break;
        case 1:
            CORE.keys[p] = 1;
            if (CORE.prev[p] == 0 && CORE.count < 16) { CORE.q[CORE.count] = p; CORE.count++; }
            break;
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
int *addr_of(int i) { return &CORE.q[i & 15]; }
float fl(int i) { return CORE.f[i & 31] * 2.0f; }
void *base(void) { return (void *)&__ImageBase; }
long long wide(long long i) { return (long long)sarr[i & 63] + (long long)CORE.q[i & 15]; }
