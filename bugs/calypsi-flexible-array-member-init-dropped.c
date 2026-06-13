typedef struct { int n; const void *slots[]; } fam_t;
extern int f1(int), f2(int);
const int k = 7;
const fam_t a = { .n = 1, .slots = { (const void*)f1 } };
const fam_t b = { .n = 2, .slots = { (const void*)f1, (const void*)f2 } };
const fam_t c = { .n = 1, .slots = { &k } };
