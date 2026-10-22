/* x86 "game" code for the translation harness: compiled with gcc -m32 for
 * x87, linked flat, lifted by xboxrecomp's translator. */
typedef struct { float x, y, z; } vec3;
typedef struct node { struct node *next; int key; float w; } node;
typedef struct { vec3 pos, vel; float mass; int flags; } body;

body bodies[64];
node nodes[128];
float mat[16];
unsigned hist[17];
int (*ops[4])(int, int);

static int op_add(int a, int b) { return a + b; }
static int op_xor(int a, int b) { return a ^ (b << 3); }
static int op_mul(int a, int b) { return a * (b | 1); }
static int op_sub(int a, int b) { return a - (b >> 2); }

__attribute__((noinline)) void vtransform(vec3 *o, const vec3 *v, const float *m)
{
    o->x = m[0]*v->x + m[4]*v->y + m[8]*v->z + m[12];
    o->y = m[1]*v->x + m[5]*v->y + m[9]*v->z + m[13];
    o->z = m[2]*v->x + m[6]*v->y + m[10]*v->z + m[14];
}

__attribute__((noinline)) float vlen2(const vec3 *v) { return v->x*v->x + v->y*v->y + v->z*v->z; }

__attribute__((noinline)) void step(float dt)
{
    for (int i = 0; i < 64; i++) {
        body *b = &bodies[i];
        vec3 t;
        b->vel.y -= 9.81f * dt;
        b->pos.x += b->vel.x * dt; b->pos.y += b->vel.y * dt; b->pos.z += b->vel.z * dt;
        if (b->pos.y < 0.0f) { b->pos.y = -b->pos.y; b->vel.y = -b->vel.y * 0.5f; b->flags |= 1; }
        vtransform(&t, &b->pos, mat);
        if (vlen2(&t) > 10000.0f) { b->pos.x *= 0.5f; b->pos.z *= 0.5f; b->flags ^= 2; }
    }
}

__attribute__((noinline)) unsigned hash(unsigned h, const unsigned char *p, int n)
{
    while (n--) { h ^= *p++; h *= 16777619u; h = (h << 5) | (h >> 27); }
    return h;
}

__attribute__((noinline)) int classify(int v)
{
    switch (v & 15) {
    case 0: return v * 3;
    case 1: return v + 7;
    case 2: return v ^ 0x55;
    case 3: return -v;
    case 4: return v >> 1;
    case 5: return v << 2;
    case 6: return v | 0x100;
    case 7: return v & 0xFF0;
    case 8: return v - 99;
    default: return v;
    }
}

__attribute__((noinline)) int walk(node *n, int key)
{
    int c = 0; float w = 0;
    for (; n; n = n->next) { if (n->key == key) c++; w += n->w; if (w > 1000.0f) w *= 0.25f; }
    return c * 1000 + (int)w;
}

int run(int iters)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < 16; i++) mat[i] = (i % 5 == 0) ? 1.0f : 0.01f * i;
    for (int i = 0; i < 64; i++) {
        bodies[i].pos.x = i; bodies[i].pos.y = 10 + (i & 7); bodies[i].pos.z = -i;
        bodies[i].vel.x = 0.5f * (i & 3); bodies[i].vel.y = 0; bodies[i].vel.z = 1.0f;
        bodies[i].mass = 1 + i; bodies[i].flags = 0;
    }
    for (int i = 0; i < 128; i++) { nodes[i].next = i < 127 ? &nodes[i + 1] : 0; nodes[i].key = (i * 7) & 31; nodes[i].w = i * 0.75f; }
    ops[0] = op_add; ops[1] = op_xor; ops[2] = op_mul; ops[3] = op_sub;
    int acc = 1;
    for (int it = 0; it < iters; it++) {
        step(1.0f / 60.0f);
        h = hash(h, (const unsigned char *)bodies, sizeof bodies);
        acc = ops[it & 3](acc, classify(acc + it));
        acc += walk(nodes, it & 31);
        hist[h % 17]++;
    }
    int fl = 0;
    for (int i = 0; i < 64; i++) fl += bodies[i].flags + (int)(bodies[i].pos.y * 16.0f);
    return (int)(h ^ (unsigned)acc ^ (unsigned)fl ^ hist[3]);
}
