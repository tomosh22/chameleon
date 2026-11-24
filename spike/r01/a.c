/* F-2 repro shape */
int table[1024];
int lookup(int i) { return table[i & 1023]; }
