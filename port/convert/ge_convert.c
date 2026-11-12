/*
 * ge_convert.c: C port of tools_pc/d43_emit.py, d69_emit.py, d88_emit.py and
 * d88_propdefs.py (see ge_convert.h). Every function mirrors the Python one of
 * the same name; comments marked "py:" note Python semantics that the output
 * depends on (dict overwrite order, sort keys, bisect).
 *
 * Not ported: d43's round-trip validation and buffer cross-checks. They only
 * report problems with a conversion of a known ROM (the SHA-1 is checked
 * before this runs) and pass for the supported ROMs; output equality with the
 * Python tools is what tools_pc/check_native_convert.sh verifies.
 */
#include "ge_convert.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

typedef struct { const char *name; uint32_t addr, size; int ns, nt; } rc_model_entry;
enum { RC_SEG = 1, RC_STAN = 2, RC_SETUP = 3 };
typedef struct { const char *name; uint32_t addr, size; int kind; } rc_cg_entry;
#include "ge_convert_tables.h"

const char *ge_convert_region(void) { return RC_TABLE_REGION; }

/* ------------------------------------------------------------ basics --- */

typedef struct {
    char *err;
    size_t err_size;
    int failed;
} rc_ctx;

static void rc_fail(rc_ctx *c, const char *fmt, ...)
{
    if (c->failed)
        return;
    c->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, c->err_size, fmt, ap);
    va_end(ap);
}

typedef struct { uint8_t *p; size_t n, cap; } rc_buf;

static int buf_reserve(rc_buf *b, size_t need)
{
    if (need <= b->cap)
        return 1;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < need)
        cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p)
        return 0;
    b->p = p;
    b->cap = cap;
    return 1;
}

static int buf_append(rc_buf *b, const void *d, size_t n)
{
    if (!buf_reserve(b, b->n + n))
        return 0;
    if (d)
        memcpy(b->p + b->n, d, n);
    else
        memset(b->p + b->n, 0, n);
    b->n += n;
    return 1;
}

/* growable int64 vector */
typedef struct { int64_t *v; size_t n, cap; } vec;

static void vpush(vec *a, int64_t x)
{
    if (a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 64;
        int64_t *v = realloc(a->v, cap * sizeof(*v));
        if (!v)
            abort();
        a->v = v;
        a->cap = cap;
    }
    a->v[a->n++] = x;
}

static int vhas(const vec *a, int64_t x)
{
    for (size_t i = 0; i < a->n; i++)
        if (a->v[i] == x)
            return 1;
    return 0;
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* sorted copy with duplicates removed (py: sorted(set(...))) */
static void vsorted_unique(const vec *in, vec *out)
{
    out->n = 0;
    for (size_t i = 0; i < in->n; i++)
        vpush(out, in->v[i]);
    if (!out->n)
        return;
    qsort(out->v, out->n, sizeof(int64_t), cmp_i64);
    size_t w = 1;
    for (size_t i = 1; i < out->n; i++)
        if (out->v[i] != out->v[w - 1])
            out->v[w++] = out->v[i];
    out->n = w;
}

/* big-endian reads with bounds checks (py: struct.error -> the tool dies) */
typedef struct { const uint8_t *p; int64_t n; rc_ctx *c; const char *name; } rc_src;

static int src_ok(const rc_src *s, int64_t o, int64_t len)
{
    if (o < 0 || o + len > s->n) {
        rc_fail(s->c, "%s: read %lld+%lld out of range (%lld)", s->name, (long long)o,
                (long long)len, (long long)s->n);
        return 0;
    }
    return 1;
}
static uint32_t be32r(const rc_src *s, int64_t o)
{
    if (!src_ok(s, o, 4)) return 0;
    const uint8_t *b = s->p + o;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static uint32_t be32o(const rc_src *s, int64_t o) { return be32r(s, o) & 0xFFFFFF; }
static int32_t bs32(const rc_src *s, int64_t o) { return (int32_t)be32r(s, o); }
static uint16_t bu16(const rc_src *s, int64_t o)
{
    if (!src_ok(s, o, 2)) return 0;
    return (uint16_t)((s->p[o] << 8) | s->p[o + 1]);
}
static int16_t be16(const rc_src *s, int64_t o) { return (int16_t)bu16(s, o); }
static uint8_t bu8(const rc_src *s, int64_t o)
{
    if (!src_ok(s, o, 1)) return 0;
    return s->p[o];
}

/* little-endian writes into a fixed-size output (py would grow the
 * bytearray; never happens for a correct conversion, so it is an error) */
typedef struct { uint8_t *p; int64_t n; rc_ctx *c; const char *name; } rc_out;

static int out_ok(const rc_out *o, int64_t at, int64_t len)
{
    if (at < 0 || at + len > o->n) {
        rc_fail(o->c, "%s: write %lld+%lld out of range (%lld)", o->name, (long long)at,
                (long long)len, (long long)o->n);
        return 0;
    }
    return 1;
}
static void put_le32(rc_out *o, int64_t at, uint32_t v)
{
    if (!out_ok(o, at, 4)) return;
    o->p[at] = v; o->p[at + 1] = v >> 8; o->p[at + 2] = v >> 16; o->p[at + 3] = v >> 24;
}
static void put_le16(rc_out *o, int64_t at, uint32_t v)
{
    if (!out_ok(o, at, 2)) return;
    o->p[at] = v; o->p[at + 1] = v >> 8;
}
static void put_zero(rc_out *o, int64_t at, int64_t len)
{
    if (!out_ok(o, at, len)) return;
    memset(o->p + at, 0, (size_t)len);
}
static void put_bytes(rc_out *o, int64_t at, const rc_src *s, int64_t from, int64_t len)
{
    if (len <= 0) return;
    if (!out_ok(o, at, len) || !src_ok(s, from, len)) return;
    memmove(o->p + at, s->p + from, (size_t)len);
}
/* bswap32 of a source word: write its bytes reversed */
static void put_rev32(rc_out *o, int64_t at, const rc_src *s, int64_t from)
{
    if (!out_ok(o, at, 4) || !src_ok(s, from, 4)) return;
    for (int i = 0; i < 4; i++)
        o->p[at + i] = s->p[from + 3 - i];
}

/* ---------------------------------------------------- RZ (raw deflate) --- */

static int rz_inflate(rc_ctx *c, const char *name, const uint8_t *in, size_t n, rc_buf *out)
{
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -15) != Z_OK) {
        rc_fail(c, "%s: inflateInit failed", name);
        return 0;
    }
    out->n = 0;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)n;
    int rc;
    do {
        if (!buf_reserve(out, out->n + 65536)) { rc = Z_MEM_ERROR; break; }
        z.next_out = out->p + out->n;
        z.avail_out = (uInt)(out->cap - out->n);
        rc = inflate(&z, Z_NO_FLUSH);
        out->n = out->cap - z.avail_out;
    } while (rc == Z_OK);
    inflateEnd(&z);
    if (rc != Z_STREAM_END) {
        rc_fail(c, "%s: decompress failed (%d)", name, rc);
        return 0;
    }
    return 1;
}

/* py: b"\x11\x72" + zlib.compressobj(6, zlib.DEFLATED, -15) */
static int rz_deflate(rc_ctx *c, const char *name, const uint8_t *in, size_t n, rc_buf *out)
{
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        rc_fail(c, "%s: deflateInit failed", name);
        return 0;
    }
    size_t bound = deflateBound(&z, (uLong)n) + 2;
    out->n = 0;
    if (!buf_reserve(out, bound)) {
        deflateEnd(&z);
        rc_fail(c, "%s: out of memory", name);
        return 0;
    }
    out->p[0] = 0x11;
    out->p[1] = 0x72;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)n;
    z.next_out = out->p + 2;
    z.avail_out = (uInt)(bound - 2);
    int rc = deflate(&z, Z_FINISH);
    out->n = 2 + (bound - 2 - z.avail_out);
    deflateEnd(&z);
    if (rc != Z_STREAM_END) {
        rc_fail(c, "%s: compress failed (%d)", name, rc);
        return 0;
    }
    return 1;
}

/* ======================================================== d43 (models) === */

static const int N64_REC[25] = { [1] = 0x1C, [2] = 0x1C, [3] = 0x1C, [4] = 0x14, [8] = 0x10,
    [9] = 0x24, [10] = 0x1C, [12] = 0x28, [13] = 0x20, [15] = 0x1C, [17] = 0x20, [18] = 0x08,
    [21] = 0x14, [22] = 0x10, [23] = 0x02, [24] = 0x20 };
static const int PC_REC[25] = { [1] = 24, [2] = 40, [3] = 40, [4] = 40, [8] = 24, [9] = 48,
    [10] = 28, [12] = 48, [13] = 48, [15] = 28, [18] = 16, [21] = 20, [22] = 32, [23] = 2,
    [24] = 64 };
/* py: op 17 is in N64_REC but not PC_REC -> KeyError (the tool dies) */
static int op_known(int op) { return op >= 0 && op <= 24 && N64_REC[op] != 0; }

typedef struct { int64_t old, osz, nw; } region;

static int cmp_region(const void *a, const void *b)
{
    const region *x = a, *y = b;   /* py: tuple sort (old, osz, new) */
    if (x->old != y->old) return x->old < y->old ? -1 : 1;
    if (x->osz != y->osz) return x->osz < y->osz ? -1 : 1;
    if (x->nw != y->nw) return x->nw < y->nw ? -1 : 1;
    return 0;
}

typedef struct {
    region *v;
    size_t n, cap;
} regions;

static void rpush(regions *r, int64_t old, int64_t osz, int64_t nw)
{
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 256;
        r->v = realloc(r->v, r->cap * sizeof(region));
        if (!r->v) abort();
    }
    r->v[r->n++] = (region){ old, osz, nw };
}

#define NONE INT64_MIN

/* py: remap() over regions sorted by tuple; bisect_right on the old offsets */
static int64_t remap(const regions *r, int64_t off)
{
    if (off == 0)
        return 0;
    size_t lo = 0, hi = r->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (off < r->v[mid].old) hi = mid; else lo = mid + 1;
    }
    if (lo == 0)
        return NONE;
    const region *g = &r->v[lo - 1];
    if (g->old <= off && off < g->old + g->osz)
        return g->nw + (off - g->old);
    return NONE;
}

typedef struct {
    uint8_t present;
    uint8_t op;
    uint32_t data, parent, next, prev, child;
} node;

static int64_t gdl_end(const rc_src *s, int64_t D, int64_t g)
{
    int64_t o = g;
    while (o + 8 <= D) {
        if ((be32r(s, o) >> 24) == 0xB8)
            return o + 8;
        o += 8;
    }
    return D;
}

static node *nget(rc_ctx *c, node *nodes, int64_t D, int64_t o, const char *name)
{
    if (o < 0 || o >= D || !nodes[o].present) {
        rc_fail(c, "%s: node %#llx missing", name, (long long)o);
        return NULL;
    }
    return &nodes[o];
}

/* py: process(name) -> compressed PC image in comp */
static int d43_process(rc_ctx *c, const uint8_t *rom, size_t rom_size, const rc_model_entry *e,
                       rc_buf *comp)
{
    const char *name = e->name;
    int ok = 0;
    rc_buf dec = { 0 };
    node *nodes = NULL;
    int64_t *node_newoff = NULL, *rec_newoff = NULL, *gdl_newoff = NULL;
    uint8_t *seen = NULL;
    vec stack = { 0 }, seq = { 0 }, placed = { 0 }, vtx_vo = { 0 }, vtx_nv = { 0 },
        zero_vtx = { 0 }, coll = { 0 }, pu_off = { 0 }, pu_nv = { 0 }, ref_gdls = { 0 },
        unvisited = { 0 }, all_gdls = { 0 }, blob_t = { 0 }, blob_u = { 0 }, tmp = { 0 },
        bl_old = { 0 }, bl_sz = { 0 }, bl_new = { 0 };
    regions R = { 0 };
    uint8_t *buf = NULL;

    if ((uint64_t)e->addr + e->size > rom_size || e->size < 2) {
        rc_fail(c, "%s: ROM range out of bounds", name);
        goto done;
    }
    if (!rz_inflate(c, name, rom + e->addr + 2, e->size - 2, &dec))
        goto done;
    rc_src S = { dec.p, (int64_t)dec.n, c, name };
    const int64_t D = (int64_t)dec.n;
    const int NS = e->ns, NT = e->nt;

    nodes = calloc((size_t)D + 1, sizeof(node));
    node_newoff = malloc(((size_t)D + 1) * sizeof(int64_t));
    rec_newoff = malloc(((size_t)D + 1) * sizeof(int64_t));
    gdl_newoff = malloc(((size_t)D + 1) * sizeof(int64_t));
    seen = calloc((size_t)D + 1, 1);
    if (!nodes || !node_newoff || !rec_newoff || !gdl_newoff || !seen) {
        rc_fail(c, "%s: out of memory", name);
        goto done;
    }
    for (int64_t i = 0; i <= D; i++)
        node_newoff[i] = rec_newoff[i] = gdl_newoff[i] = NONE;

    /* ---- build_nodes ---- */
    const int64_t R0 = 4 * NS + 12 * NT;
    int nnodes = 0;
    vpush(&stack, R0);
    while (stack.n && !c->failed) {
        int64_t o = stack.v[--stack.n];
        if (o >= D || nodes[o].present)
            continue;
        node *n = &nodes[o];
        n->present = 1;
        nnodes++;
        n->op = bu16(&S, o) & 0xff;
        n->data = be32o(&S, o + 4);
        n->parent = be32o(&S, o + 8);
        n->next = be32o(&S, o + 0xC);
        n->prev = be32o(&S, o + 0x10);
        n->child = be32o(&S, o + 0x14);
        if (n->op == 8) {
            uint32_t q = be32o(&S, (int64_t)n->data + 8);
            if (q) vpush(&stack, q);
        } else if (n->op == 18) {
            uint32_t q = be32o(&S, (int64_t)n->data + 0);
            if (q) vpush(&stack, q);
        } else if (n->child) {
            vpush(&stack, n->child);
        }
        if (n->next)
            vpush(&stack, n->next);
    }
    if (c->failed) goto done;
    if (!nnodes) { rc_fail(c, "%s: empty tree", name); goto done; }

    /* ---- visit_seq (mutates the node links, like the game's walk) ---- */
    {
        int64_t nd = R0, prev_node = -1, prev_gdl = 0;
        int guard = 0;
        while (nd && guard < 200000 && !c->failed) {
            guard++;
            node *n = nget(c, nodes, D, nd, name);
            if (!n) break;
            int64_t gdl = 0;
            if (n->op == 4 || n->op == 22 || n->op == 24) {
                int64_t p = 0, s = 0;
                if (n->op == 22) {
                    p = be32o(&S, (int64_t)n->data + 8);
                } else {
                    p = be32o(&S, n->data);
                    s = be32o(&S, (int64_t)n->data + 4);
                }
                if (nd != prev_node)
                    gdl = p;
                else if (s && s != prev_gdl)
                    gdl = s;
            } else if (n->op == 8) {
                n->child = be32o(&S, (int64_t)n->data + 8);
            } else if (n->op == 18) {
                n->child = be32o(&S, n->data);
            } else if (n->op == 9) {
                uint32_t node1 = be32o(&S, (int64_t)n->data + 0x18);
                uint32_t node2 = be32o(&S, (int64_t)n->data + 0x1C);
                if (node1) {
                    n->child = node1;
                    node *n1 = nget(c, nodes, D, node1, name);
                    if (!n1) break;
                    n1->prev = 0;
                    int64_t loop = node1;
                    for (;;) {
                        node *l = nget(c, nodes, D, loop, name);
                        if (!l || !l->next || l->next == node2) break;
                        loop = l->next;
                    }
                    if (c->failed) break;
                    nodes[loop].next = node2;
                    if (node2) {
                        node *n2 = nget(c, nodes, D, node2, name);
                        if (!n2) break;
                        n2->prev = (uint32_t)loop;
                        loop = node2;
                        for (;;) {
                            node *l = nget(c, nodes, D, loop, name);
                            if (!l || !l->next || l->next == node1) break;
                            loop = l->next;
                        }
                        if (c->failed) break;
                        nodes[loop].next = 0;
                    }
                } else {
                    n->child = node2;
                    if (node2) {
                        node *n2 = nget(c, nodes, D, node2, name);
                        if (!n2) break;
                        n2->prev = 0;
                    }
                }
            }
            if (gdl) {
                vpush(&seq, gdl);
                prev_node = nd;
                prev_gdl = gdl;
                continue;
            }
            prev_node = -1;
            if (n->child) {
                nd = n->child;
            } else {
                while (nd) {
                    node *m = nget(c, nodes, D, nd, name);
                    if (!m) break;
                    if (m->next) { nd = m->next; break; }
                    nd = m->parent;
                }
            }
        }
    }
    if (c->failed) goto done;

    /* ---- placement_order (on the mutated links) ---- */
    stack.n = 0;
    vpush(&stack, R0);
    while (stack.n && !c->failed) {
        int64_t o = stack.v[--stack.n];
        if (o >= D || seen[o])
            continue;
        seen[o] = 1;
        node *n = nget(c, nodes, D, o, name);
        if (!n) break;
        vpush(&placed, o);
        if (n->op == 8) {
            uint32_t a = be32o(&S, (int64_t)n->data + 8);
            if (a) vpush(&stack, a);
        } else if (n->op == 18) {
            uint32_t q = be32o(&S, n->data);
            if (q) vpush(&stack, q);
        } else if (n->child) {
            vpush(&stack, n->child);
        }
        if (n->next)
            vpush(&stack, n->next);
    }
    if (c->failed) goto done;

    /* ---- layout pass ---- */
    int64_t dstpos = 0;
#define ADD_REGION(old, osz) (rpush(&R, (old), (osz), dstpos), dstpos += (osz))
    for (int i = 0; i < NS; i++) ADD_REGION(4 * i, 8);
    for (int i = 0; i < NT; i++) ADD_REGION(4 * NS + 12 * i, 12);
    for (size_t k = 0; k < placed.n && !c->failed; k++) {
        int64_t no = placed.v[k];
        int op = nodes[no].op;
        int64_t data = be32o(&S, no + 4);
        ADD_REGION(no, 48);
        node_newoff[no] = dstpos - 48;
        if (!op_known(op) || (op != 17 && !PC_REC[op]) || op == 17) {
            rc_fail(c, "%s: unknown opcode %d at %#llx", name, op, (long long)no);
            break;
        }
        if (data >= D) { rc_fail(c, "%s: record %#llx outside the file", name, (long long)data); break; }
        rec_newoff[data] = dstpos;
        ADD_REGION(data, PC_REC[op]);
        if (op == 4) {
            int64_t nv = bu16(&S, data + 0x10), vo = be32o(&S, data + 0xC);
            if (vo) {
                if (!nv) {
                    vpush(&zero_vtx, vo);
                } else {
                    vpush(&vtx_vo, vo); vpush(&vtx_nv, nv);
                    ADD_REGION(vo, 16 * nv);
                }
            }
        } else if (op == 24) {
            int64_t nv = be16(&S, data + 0xC), ncv = be16(&S, data + 0xE);
            int64_t vo = be32o(&S, data + 8), cvo = be32o(&S, data + 0x10);
            int64_t puo = be32o(&S, data + 0x14);
            if (nv && vo) { vpush(&vtx_vo, vo); vpush(&vtx_nv, nv); ADD_REGION(vo, 16 * nv); }
            if (ncv && cvo) {
                if (!vhas(&coll, cvo)) vpush(&coll, cvo);
                vpush(&vtx_vo, cvo); vpush(&vtx_nv, ncv); ADD_REGION(cvo, 16 * ncv);
            }
            if (nv && puo) {
                ADD_REGION(puo, 2 * nv);
                /* py: dict, first insertion keeps its place, last value wins */
                size_t j;
                for (j = 0; j < pu_off.n; j++)
                    if (pu_off.v[j] == puo) { pu_nv.v[j] = nv; break; }
                if (j == pu_off.n) { vpush(&pu_off, puo); vpush(&pu_nv, nv); }
            }
        } else if (op == 22) {
            int64_t nv = bs32(&S, data), vo = be32o(&S, data + 4);
            if (nv && vo) { vpush(&vtx_vo, vo); vpush(&vtx_nv, 4 * nv); ADD_REGION(vo, 64 * nv); }
        }
    }
    if (c->failed) goto done;

    for (size_t k = 0; k < placed.n; k++) {
        int64_t no = placed.v[k];
        int op = nodes[no].op;
        int64_t data = be32o(&S, no + 4);
        if (op == 4 || op == 24) {
            for (int off = 0; off <= 4; off += 4) {
                int64_t q = be32o(&S, data + off);
                if (q && !vhas(&ref_gdls, q)) vpush(&ref_gdls, q);
            }
        } else if (op == 22) {
            int64_t q = be32o(&S, data + 8);
            if (q && !vhas(&ref_gdls, q)) vpush(&ref_gdls, q);
        }
    }
    for (size_t k = 0; k < ref_gdls.n; k++)
        if (!vhas(&seq, ref_gdls.v[k])) vpush(&tmp, ref_gdls.v[k]);
    vsorted_unique(&tmp, &unvisited);
    for (size_t k = 0; k < unvisited.n; k++) vpush(&all_gdls, unvisited.v[k]);
    for (size_t k = 0; k < seq.n; k++) vpush(&all_gdls, seq.v[k]);

    /* zero-vertex-count arrays run to the next object (obj_offs_now) */
    {
        size_t nvtx_before = vtx_vo.n;
        for (size_t z = 0; z < zero_vtx.n && !c->failed; z++) {
            int64_t vo = zero_vtx.v[z], nxt = D;
#define CAND(o) do { int64_t _o = (o); if (_o > vo && _o < nxt) nxt = _o; } while (0)
            for (size_t k = 0; k < placed.n; k++) {
                CAND(placed.v[k]);
                CAND(be32o(&S, placed.v[k] + 4)); /* rec_newoff keys */
            }
            for (size_t k = 0; k < nvtx_before; k++) CAND(vtx_vo.v[k]);
            for (size_t k = 0; k < all_gdls.n; k++) CAND(all_gdls.v[k]);
#undef CAND
            int64_t sz = nxt - vo;
            if (sz <= 0 || sz % 16) {
                rc_fail(c, "%s: zero-vtx array %#llx bad size %#llx", name, (long long)vo, (long long)sz);
                break;
            }
            vpush(&vtx_vo, vo); vpush(&vtx_nv, sz / 16);
            ADD_REGION(vo, sz);
        }
    }
    if (c->failed) goto done;

    /* embedded image blobs (G_SETTIMG seg-5 targets) */
    for (size_t k = 0; k < all_gdls.n && !c->failed; k++) {
        int64_t o = all_gdls.v[k];
        while (o + 8 <= D) {
            uint32_t w0 = be32r(&S, o), cc = w0 >> 24;
            if (cc == 0xB8) break;
            uint32_t w1 = be32r(&S, o + 4);
            if (cc == 0xFD && (w1 >> 24) == 5) vpush(&blob_t, w1 & 0xFFFFFF);
            o += 8;
        }
    }
    vsorted_unique(&blob_t, &blob_u);
    {
        size_t nplaced = R.n; /* placed_offs = old offsets of the regions so far */
        int64_t mstart = -1, mend = -1;
        for (size_t k = 0; k <= blob_u.n; k++) {
            int64_t t = 0, end = 0;
            if (k < blob_u.n) {
                t = blob_u.v[k];
                end = D;
                for (size_t j = 0; j < nplaced; j++)
                    if (R.v[j].old > t && R.v[j].old < end) end = R.v[j].old;
            }
            if (k < blob_u.n && mstart >= 0 && t <= mend) {
                if (end > mend) mend = end;
                continue;
            }
            if (mstart >= 0) {
                vpush(&bl_old, mstart); vpush(&bl_sz, mend - mstart); vpush(&bl_new, dstpos);
                ADD_REGION(mstart, mend - mstart);
            }
            mstart = t; mend = end;
        }
    }

    /* GDLs last: unvisited first, then visit order, 16 B per N64 slot */
    for (size_t k = 0; k < all_gdls.n && !c->failed; k++) {
        int64_t g = all_gdls.v[k];
        int64_t end = gdl_end(&S, D, g);
        int64_t nslots = (end - g) / 8;
        if ((end - g) % 8) { rc_fail(c, "%s: GDL %#llx not 8B aligned end", name, (long long)g); break; }
        if (g >= D) { rc_fail(c, "%s: GDL %#llx outside the file", name, (long long)g); break; }
        gdl_newoff[g] = dstpos;
        rpush(&R, g, 16 * nslots, dstpos);
        dstpos += 16 * nslots;
    }
#undef ADD_REGION
    if (c->failed) goto done;
    const int64_t D_PC = dstpos;

    qsort(R.v, R.n, sizeof(region), cmp_region);

    /* ---- emit pass ---- */
    buf = calloc((size_t)D_PC + 1, 1);
    if (!buf) { rc_fail(c, "%s: out of memory", name); goto done; }
    rc_out O = { buf, D_PC, c, name };
#define PUT_PTR(at, old_off) do {                                               \
        int64_t _old = (old_off);                                               \
        if (_old) {                                                             \
            int64_t _n = remap(&R, _old);                                       \
            if (_n == NONE) { rc_fail(c, "%s: ptr %#llx not in region map", name, (long long)_old); _n = 0; } \
            put_le32(&O, (at), 0x05000000u | ((uint32_t)_n & 0xFFFFFF));       \
            put_zero(&O, (at) + 4, 4);                                          \
        }                                                                       \
    } while (0)
#define GDL_PTR(at, g) do {                                                     \
        int64_t _g = (g);                                                       \
        if (_g) {                                                               \
            if (_g >= D || gdl_newoff[_g] == NONE) { rc_fail(c, "%s: GDL %#llx not packed", name, (long long)_g); } \
            else put_le32(&O, (at), 0x05000000u | ((uint32_t)gdl_newoff[_g] & 0xFFFFFF)); \
        }                                                                       \
    } while (0)
#define F32(at, doff) put_rev32(&O, (at), &S, (doff))
#define U16(at, v) put_le16(&O, (at), (uint32_t)(v))

    for (int i = 0; i < NS; i++) PUT_PTR(8 * i, be32o(&S, 4 * i));
    for (int i = 0; i < NT; i++) {
        int64_t doff = 4 * NS + 12 * i, npos = 8 * NS + 12 * i;
        uint32_t tid = be32r(&S, doff);
        put_le32(&O, npos, tid);
        put_bytes(&O, npos + 4, &S, doff + 4, 8);
        if ((tid >> 24) == 5 && remap(&R, tid & 0xFFFFFF) == NONE)
            rc_fail(c, "%s: texconfig seg-5 target %#x not in a blob region", name, tid & 0xFFFFFF);
    }

    for (size_t k = 0; k < placed.n && !c->failed; k++) {
        int64_t no = placed.v[k];
        int op = nodes[no].op;
        int64_t np = node_newoff[no];
        U16(np, bu16(&S, no));
        PUT_PTR(np + 8, be32o(&S, no + 4));
        PUT_PTR(np + 16, be32o(&S, no + 8));
        PUT_PTR(np + 24, be32o(&S, no + 0xC));
        PUT_PTR(np + 32, be32o(&S, no + 0x10));
        PUT_PTR(np + 40, be32o(&S, no + 0x14));
        int64_t d = be32o(&S, no + 4);
        int64_t r = rec_newoff[d];
        switch (op) {
        case 1:
            U16(r, bu16(&S, d)); U16(r + 2, be16(&S, d + 2));
            PUT_PTR(r + 8, be32o(&S, d + 4));
            U16(r + 16, bu16(&S, d + 8)); U16(r + 18, bu16(&S, d + 0xA));
            U16(r + 20, bu16(&S, d + 0xC));
            break;
        case 2: case 3:
            for (int i = 0; i < 3; i++) F32(r + 4 * i, d + 4 * i);
            U16(r + 12, bu16(&S, d + 0xC));
            for (int i = 0; i < 3; i++) U16(r + 14 + 2 * i, be16(&S, d + 0xE + 2 * i));
            PUT_PTR(r + 24, be32o(&S, d + 0x14));
            F32(r + 32, d + 0x18);
            break;
        case 4:
            GDL_PTR(r, be32o(&S, d));
            GDL_PTR(r + 8, be32o(&S, d + 4));
            PUT_PTR(r + 24, be32o(&S, d + 0xC));
            U16(r + 32, bu16(&S, d + 0x10));
            put_bytes(&O, r + 34, &S, d + 0x12, 1);
            break;
        case 8:
            F32(r, d); F32(r + 4, d + 4);
            PUT_PTR(r + 8, be32o(&S, d + 8));
            U16(r + 16, bu16(&S, d + 0xC));
            break;
        case 9:
            for (int i = 0; i < 3; i++) F32(r + 4 * i, d + 4 * i);
            for (int i = 0; i < 3; i++) F32(r + 12 + 4 * i, d + 0xC + 4 * i);
            PUT_PTR(r + 24, be32o(&S, d + 0x18));
            PUT_PTR(r + 32, be32o(&S, d + 0x1C));
            U16(r + 42, bu16(&S, d + 0x22));
            break;
        case 10:
            put_le32(&O, r, be32r(&S, d));
            for (int i = 0; i < 6; i++) F32(r + 4 + 4 * i, d + 4 + 4 * i);
            break;
        case 12:
            for (int i = 0; i < 3; i++) F32(r + 4 * i, d + 4 * i);
            for (int i = 0; i < 3; i++) F32(r + 12 + 4 * i, d + 0xC + 4 * i);
            PUT_PTR(r + 24, be32o(&S, d + 0x18));
            F32(r + 32, d + 0x1C);
            U16(r + 36, bu16(&S, d + 0x20));
            break;
        case 13:
            for (int i = 0; i < 2; i++) F32(r + 4 * i, d + 4 * i);
            for (int i = 0; i < 2; i++) F32(r + 8 + 4 * i, d + 8 + 4 * i);
            PUT_PTR(r + 16, be32o(&S, d + 0x10));
            PUT_PTR(r + 24, be32o(&S, d + 0x14));
            F32(r + 32, d + 0x18);
            break;
        case 15:
            for (int i = 0; i < 3; i++) F32(r + 4 * i, d + 4 * i);
            for (int i = 0; i < 3; i++) F32(r + 12 + 4 * i, d + 0xC + 4 * i);
            F32(r + 24, d + 0x18);
            break;
        case 18:
            PUT_PTR(r, be32o(&S, d));
            U16(r + 8, bu16(&S, d + 4));
            break;
        case 21:
            for (int i = 0; i < 3; i++) F32(r + 4 * i, d + 4 * i);
            U16(r + 12, be16(&S, d + 0xC)); U16(r + 14, bu16(&S, d + 0xE));
            F32(r + 16, d + 0x10);
            break;
        case 22:
            put_le32(&O, r, be32r(&S, d));
            PUT_PTR(r + 8, be32o(&S, d + 4));
            GDL_PTR(r + 16, be32o(&S, d + 8));
            break;
        case 23:
            U16(r, bu16(&S, d));
            break;
        case 24:
            GDL_PTR(r, be32o(&S, d));
            GDL_PTR(r + 8, be32o(&S, d + 4));
            PUT_PTR(r + 16, be32o(&S, d + 8));
            U16(r + 24, be16(&S, d + 0xC)); U16(r + 26, be16(&S, d + 0xE));
            PUT_PTR(r + 32, be32o(&S, d + 0x10));
            PUT_PTR(r + 40, be32o(&S, d + 0x14));
            U16(r + 48, be16(&S, d + 0x18)); U16(r + 50, bu16(&S, d + 0x1A));
            break;
        }
    }

    /* vertex arrays */
    for (size_t k = 0; k < vtx_vo.n && !c->failed; k++) {
        int64_t vo = vtx_vo.v[k], nv = vtx_nv.v[k];
        int64_t np = remap(&R, vo);
        if (np == NONE) { rc_fail(c, "%s: vtx %#llx not in region map", name, (long long)vo); break; }
        int collision = vhas(&coll, vo);
        for (int64_t i = 0; i < nv && !c->failed; i++) {
            int64_t o = vo + 16 * i, q = np + 16 * i;
            U16(q, be16(&S, o)); U16(q + 2, be16(&S, o + 2));
            U16(q + 4, be16(&S, o + 4)); U16(q + 6, be16(&S, o + 6));
            if (collision) {
                uint32_t v = be32r(&S, o + 8);
                if (v == 0) {
                } else if ((v >> 24) == 5) {
                    int64_t n = remap(&R, v & 0xFFFFFF);
                    if (n == NONE) { rc_fail(c, "%s: LinkedTo %#x not in map", name, v); n = 0; }
                    put_le32(&O, q + 8, 0x05000000u | ((uint32_t)n & 0xFFFFFF));
                } else {
                    rc_fail(c, "%s: LinkedTo %#x neither null nor seg-5", name, v);
                }
                U16(q + 12, be16(&S, o + 0xC)); U16(q + 14, be16(&S, o + 0xE));
            } else {
                U16(q + 8, be16(&S, o + 8)); U16(q + 10, be16(&S, o + 0xA));
                put_bytes(&O, q + 12, &S, o + 0xC, 4);
            }
        }
    }

    /* PointUsage s16 chains */
    for (size_t k = 0; k < pu_off.n && !c->failed; k++) {
        int64_t pn = remap(&R, pu_off.v[k]);
        if (pn == NONE) { rc_fail(c, "%s: PointUsage %#llx not in region map", name, (long long)pu_off.v[k]); break; }
        for (int64_t j = 0; j < pu_nv.v[k]; j++)
            U16(pn + 2 * j, be16(&S, pu_off.v[k] + 2 * j));
    }

    /* GDL slots */
    for (size_t k = 0; k < all_gdls.n && !c->failed; k++) {
        int64_t g = all_gdls.v[k];
        int64_t end = gdl_end(&S, D, g);
        int64_t np = gdl_newoff[g];
        int64_t j = 0, o = g;
        while (o + 8 <= end && !c->failed) {
            uint32_t w0 = be32r(&S, o), w1 = be32r(&S, o + 4);
            int64_t q = np + 16 * j;
            uint32_t cc = w0 >> 24;
            if ((cc == 0x04 || cc == 0xFD) && (w1 >> 24) == 5) {
                int64_t n = remap(&R, w1 & 0xFFFFFF);
                if (n == NONE) { rc_fail(c, "%s: GDL %#llx op%#x w1 %#x not in map", name, (long long)g, cc, w1); n = 0; }
                w1 = 0x05000000u | ((uint32_t)n & 0xFFFFFF);
            }
            put_le32(&O, q, w0); put_zero(&O, q + 4, 4);
            put_le32(&O, q + 8, w1); put_zero(&O, q + 12, 4);
            if (cc == 0xB8) break;
            j++;
            o += 8;
        }
    }

    /* embedded image blobs: raw copy */
    for (size_t k = 0; k < bl_old.n; k++)
        put_bytes(&O, bl_new.v[k], &S, bl_old.v[k], bl_sz.v[k]);
#undef PUT_PTR
#undef GDL_PTR
#undef F32
#undef U16
    if (c->failed) goto done;

    if (!rz_deflate(c, name, buf, (size_t)D_PC, comp))
        goto done;
    ok = 1;

done:
    free(dec.p); free(nodes); free(node_newoff); free(rec_newoff); free(gdl_newoff); free(seen);
    free(stack.v); free(seq.v); free(placed.v); free(vtx_vo.v); free(vtx_nv.v); free(zero_vtx.v);
    free(coll.v); free(pu_off.v); free(pu_nv.v); free(ref_gdls.v); free(unvisited.v);
    free(all_gdls.v); free(blob_t.v); free(blob_u.v); free(tmp.v); free(bl_old.v); free(bl_sz.v);
    free(bl_new.v); free(R.v); free(buf);
    return ok;
}

/* ===================================================== d69 (bg / stan) === */

static int d69_convert_seg(rc_ctx *c, const char *name, const rc_src *S, rc_buf *res)
{
    const int64_t D = S->n;
    if (D < 0x40) { rc_fail(c, "%s: too small (%lld)", name, (long long)D); return 0; }
    uint32_t h0 = be32r(S, 0);
    uint32_t h1 = be32o(S, 4), h2 = be32o(S, 8), h3 = be32o(S, 12), h4 = be32o(S, 16);
    if (h0 != 0) { rc_fail(c, "%s: header word0 != 0 (%#x)", name, h0); return 0; }
    if (h4 != 0) { rc_fail(c, "%s: header word4 != 0 (%#x)", name, h4); return 0; }
    if (h2 == 0 || h2 <= h1) { rc_fail(c, "%s: portal offset %#x <= room offset %#x", name, h2, h1); return 0; }
    int64_t room_end = (h3 && h3 > h1 && h3 < h2) ? h3 : h2;
    if ((room_end - h1) % 24) { rc_fail(c, "%s: room table extent not a multiple of 24", name); return 0; }

    int ok = 0;
    vec targets = { 0 }, tsorted = { 0 };
    uint8_t *out = NULL;

    /* portal table walk (py: while ... else -> no terminator is an error) */
    int64_t o = h2, n_portals = 0;
    int found = 0;
    while (o + 8 <= D) {
        uint32_t v = be32o(S, o);
        if (v == 0) { found = 1; break; }
        vpush(&targets, v);
        n_portals++;
        o += 8;
    }
    if (!found) { rc_fail(c, "%s: portal table terminator not found before EOF", name); goto done; }
    const int64_t portal_table_end = o + 8;
    const int64_t portal_delta = 8 * (n_portals + 1);

    const int ENV_ALT = 100;
    if (h3 && room_end == h3) {
        int64_t o2 = h3;
        while (o2 < h2) {
            uint8_t etype = bu8(S, o2);
            int32_t data = bs32(S, o2 + 4);
            if (etype == ENV_ALT && data) vpush(&targets, (uint32_t)data & 0xFFFFFF);
            o2 += 8;
            if (etype == 0) break;
        }
    }
    if (c->failed) goto done;
    vsorted_unique(&targets, &tsorted);

    out = calloc((size_t)(D + portal_delta), 1);
    if (!out) { rc_fail(c, "%s: out of memory", name); goto done; }
    rc_out O = { out, D + portal_delta, c, name };

    put_le32(&O, 0, h0);
    put_le32(&O, 4, 0x0F000000u | h1);
    put_le32(&O, 8, 0x0F000000u | h2);
    put_le32(&O, 12, h3 ? (0x0F000000u | h3) : 0);
    put_le32(&O, 16, 0);
    put_bytes(&O, 20, S, 20, 0x40 - 20);

    for (int64_t o2 = h1; o2 < room_end; o2 += 24) {
        for (int w = 0; w < 3; w++) {
            uint32_t v = be32o(S, o2 + 4 * w);
            uint32_t nv = v ? (0x0F000000u | ((uint32_t)(v + portal_delta) & 0xFFFFFF)) : 0;
            put_le32(&O, o2 + 4 * w, nv);
        }
        for (int w = 0; w < 3; w++)
            put_le32(&O, o2 + 12 + 4 * w, be32r(S, o2 + 12 + 4 * w));
    }

    if (h3 && room_end == h3) {
        int64_t o2 = h3;
        while (o2 < h2 && !c->failed) {
            uint8_t etype = bu8(S, o2);
            put_bytes(&O, o2, S, o2, 4);
            int64_t data = bs32(S, o2 + 4);
            if (etype == ENV_ALT && data) data += portal_delta;
            if (data > INT32_MAX || data < INT32_MIN) { rc_fail(c, "%s: envdata overflow", name); break; }
            put_le32(&O, o2 + 4, (uint32_t)(int32_t)data);
            o2 += 8;
            if (etype == 0) break;
        }
    }

    {
        int64_t o2 = h2, pc = h2;
        for (int64_t idx = 0; idx <= n_portals && !c->failed; idx++) {
            uint32_t off_val = be32o(S, o2);
            if (off_val)
                put_le32(&O, pc, 0x0F000000u | ((uint32_t)(off_val + portal_delta) & 0xFFFFFF));
            else
                put_zero(&O, pc, 4);
            put_zero(&O, pc + 4, 4);
            put_bytes(&O, pc + 8, S, o2 + 4, 4);
            put_zero(&O, pc + 12, 4);
            o2 += 8;
            pc += 16;
        }
    }

    {
        int64_t cursor = portal_table_end;
        for (size_t k = 0; k < tsorted.n && !c->failed; k++) {
            int64_t t = tsorted.v[k];
            if (t < cursor) { rc_fail(c, "%s: portal target %#llx overlaps previous region", name, (long long)t); break; }
            if (t > cursor) put_bytes(&O, cursor + portal_delta, S, cursor, t - cursor);
            int np = bu8(S, t);
            if (np == 0 || np > 20) { rc_fail(c, "%s: portal target %#llx numPoints=%d implausible", name, (long long)t, np); break; }
            int64_t rs = 4 + 12 * np;
            if (t + rs > D) { rc_fail(c, "%s: portal target overruns EOF", name); break; }
            int64_t dst = t + portal_delta;
            put_bytes(&O, dst, S, t, 4);
            for (int64_t q = 0; q < (rs - 4) / 4; q++)
                put_rev32(&O, dst + 4 + 4 * q, S, t + 4 + 4 * q);
            cursor = t + rs;
        }
        if (!c->failed && cursor < D)
            put_bytes(&O, cursor + portal_delta, S, cursor, D - cursor);
    }
    if (c->failed) goto done;

    res->n = 0;
    if (!buf_append(res, out, (size_t)(D + portal_delta))) { rc_fail(c, "%s: out of memory", name); goto done; }
    ok = 1;
done:
    free(targets.v); free(tsorted.v); free(out);
    return ok;
}

static const int TILESIZES[12] = { 0x20, 0x20, 0x20, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0 };

static int d69_convert_stan(rc_ctx *c, const char *name, const rc_src *S, rc_buf *res)
{
    const int64_t D = S->n;
    if (D < 8) { rc_fail(c, "%s: too small", name); return 0; }
    int32_t stanfile = bs32(S, 0);
    vec offs = { 0 };
    int64_t o = 4;
    int found = 0;
    while (o + 4 <= D) {
        uint32_t v = be32r(S, o);
        vpush(&offs, v);
        o += 4;
        if (v == 0) { found = 1; break; }
    }
    if (!found) { free(offs.v); rc_fail(c, "%s: room-offset array terminator not found", name); return 0; }
    const int64_t N = (int64_t)offs.n - 1;
    const int64_t old_array_end = o, array_delta = 4 * (N + 2);
    const int64_t new_tiledata_start = old_array_end + array_delta;
    uint8_t *out = calloc((size_t)(D + array_delta), 1);
    if (!out) { free(offs.v); rc_fail(c, "%s: out of memory", name); return 0; }
    rc_out O = { out, D + array_delta, c, name };
    int ok = 0;

    put_le32(&O, 0, (uint32_t)stanfile);
    put_zero(&O, 4, 4);
    int64_t p = 8;
    for (size_t k = 0; k < offs.n; k++) {
        uint32_t v = (uint32_t)offs.v[k];
        put_le32(&O, p, v ? (uint32_t)(v + array_delta) : 0);
        put_zero(&O, p + 4, 4);
        p += 8;
    }
    (void)new_tiledata_start;

    int64_t so = old_array_end, dso = new_tiledata_start;
    found = 0;
    while (so + 4 <= D && !c->failed) {
        if (be32r(S, so) == 0) { put_zero(&O, dso, 4); found = 1; break; }
        uint8_t w0 = bu8(S, so), w1 = bu8(S, so + 1);
        if (out_ok(&O, dso, 4)) { out[dso] = w1; out[dso + 1] = w0; }
        put_bytes(&O, dso + 2, S, so + 2, 2);
        uint16_t mid = bu16(S, so + 4), tail = bu16(S, so + 6);
        put_le16(&O, dso + 4, mid);
        put_le16(&O, dso + 6, tail);
        int pointCount = (tail >> 12) & 0xF;
        if (pointCount >= 12 || TILESIZES[pointCount] == 0) {
            rc_fail(c, "%s: bad pointCount %d at src %#llx", name, pointCount, (long long)so);
            break;
        }
        int sz = TILESIZES[pointCount];
        if (so + sz > D) { rc_fail(c, "%s: tile at %#llx overruns EOF", name, (long long)so); break; }
        for (int i = 0; i < pointCount; i++) {
            int64_t po = so + 8 + 8 * i, qo = dso + 8 + 8 * i;
            for (int f = 0; f < 4; f++)
                put_le16(&O, qo + 2 * f, bu16(S, po + 2 * f));
        }
        so += sz;
        dso += sz;
    }
    if (!c->failed && !found)
        rc_fail(c, "%s: ran off EOF without an all-zero EOF sentinel", name);
    if (!c->failed && so + 4 < D)
        put_bytes(&O, dso + 4, S, so + 4, D - (so + 4));
    if (!c->failed) {
        res->n = 0;
        if (buf_append(res, out, (size_t)(D + array_delta))) ok = 1;
        else rc_fail(c, "%s: out of memory", name);
    }
    free(offs.v);
    free(out);
    return ok;
}

/* =================================================== d88 (stage setup) === */

static const int PROPDEF_N64_WORDS[49] = { [1] = 64, [2] = 2, [3] = 32, [4] = 33, [5] = 32,
    [6] = 59, [7] = 33, [8] = 34, [9] = 7, [10] = 64, [11] = 149, [12] = 32, [13] = 54, [14] = 3,
    [17] = 32, [18] = 3, [19] = 4, [20] = 45, [21] = 34, [22] = 4, [23] = 4, [24] = 1, [25] = 2,
    [26] = 2, [27] = 2, [28] = 2, [29] = 2, [30] = 4, [31] = 1, [32] = 4, [33] = 5, [34] = 3,
    [35] = 4, [36] = 32, [37] = 10, [38] = 4, [39] = 44, [40] = 45, [42] = 32, [43] = 32,
    [44] = 5, [45] = 56, [46] = 7, [47] = 37, [48] = 1 };
static const int PROPDEF_PC_BYTES[49] = { [1] = 296, [2] = 8, [3] = 144, [5] = 144, [12] = 144,
    [17] = 144, [36] = 144, [42] = 144, [43] = 144, [4] = 152, [6] = 272, [7] = 152, [8] = 160,
    [9] = 32, [10] = 288, [11] = 664, [13] = 248, [14] = 32, [18] = 12, [19] = 32, [20] = 200,
    [21] = 152, [22] = 24, [23] = 16, [24] = 4, [25] = 8, [26] = 8, [27] = 8, [28] = 8, [29] = 8,
    [30] = 24, [31] = 4, [32] = 24, [33] = 24, [34] = 12, [35] = 24, [37] = 48, [38] = 32,
    [39] = 208, [40] = 208, [44] = 40, [45] = 248, [46] = 28, [47] = 168, [48] = 4 };

/* word transforms (py: _bswap32 / _hdr_word / _hh_word / custom) */
enum { W_SWAP32, W_HDR, W_HH, W_RAW, W_0132, W_1023 };

static void put_word(uint8_t *out, int64_t pc, const uint8_t *w, int kind)
{
    static const int perm[6][4] = {
        { 3, 2, 1, 0 }, { 1, 0, 2, 3 }, { 1, 0, 3, 2 }, { 0, 1, 2, 3 }, { 0, 1, 3, 2 }, { 1, 0, 2, 3 },
    };
    for (int i = 0; i < 4; i++)
        out[pc + i] = w[perm[kind][i]];
}

static int64_t emit_object_prefix(uint8_t *out, const uint8_t *src)
{
    int64_t pc = 0;
    for (int i = 0; i < 32; i++) {
        const uint8_t *w = src + 4 * i;
        if (i == 4 || i == 5 || i == 26 || i == 27) {
            pc = (pc + 7) & ~7LL;
            pc += 8;
        } else if (i == 0) {
            put_word(out, pc, w, W_HDR); pc += 4;
        } else if (i == 1) {
            put_word(out, pc, w, W_HH); pc += 4;
        } else if (i == 30 || i == 31) {
            put_word(out, pc, w, W_RAW); pc += 4;
        } else {
            put_word(out, pc, w, W_SWAP32); pc += 4;
        }
    }
    return 144;
}

static int in_set(int x, const int *set, int n)
{
    for (int i = 0; i < n; i++)
        if (set[i] == x) return 1;
    return 0;
}

/* py: convert_record(); out has PROPDEF_PC_BYTES[t] zeroed bytes */
static int convert_record(rc_ctx *c, const char *name, const uint8_t *src, int t, uint8_t *out)
{
    const int n64w = PROPDEF_N64_WORDS[t], pcb = PROPDEF_PC_BYTES[t];
#define W(i) (src + 4 * (i))
#define CHECK_PC(pc) do { if ((pc) > pcb) { rc_fail(c, "%s: propdef %d overrun", name, t); return 0; } } while (0)
    switch (t) {
    case 3: case 5: case 12: case 17: case 36: case 42: case 43:
        emit_object_prefix(out, src);
        return 1;
    case 1: {
        emit_object_prefix(out, src);
        int64_t pc = 144;
        for (int i = 32; i < n64w; i++) {
            if (i == 50 || i == 51 || i == 61 || i == 62) { pc = (pc + 7) & ~7LL; pc += 8; }
            else if (i == 38) { CHECK_PC(pc + 4); put_word(out, pc, W(i), W_HH); pc += 4; }
            else if (i == 47) { CHECK_PC(pc + 4); put_word(out, pc, W(i), W_0132); pc += 4; }
            else if (i == 49) { CHECK_PC(pc + 4); put_word(out, pc, W(i), W_1023); pc += 4; }
            else { CHECK_PC(pc + 4); put_word(out, pc, W(i), W_SWAP32); pc += 4; }
        }
        CHECK_PC(pc);
        return 1;
    }
    case 4: case 21: case 7: {
        emit_object_prefix(out, src);
        int64_t pc = 144;
        for (int i = 32; i < n64w; i++) { CHECK_PC(pc + 4); put_word(out, pc, W(i), W_SWAP32); pc += 4; }
        return 1;
    }
    case 8:
        emit_object_prefix(out, src);
        put_word(out, 144, W(32), W_0132);
        return 1;
    case 10: case 11: {
        emit_object_prefix(out, src);
        int nmon = t == 10 ? 1 : 4;
        int64_t pc = 144 + 128 * nmon;
        for (int i = 32 + 29 * nmon; i < n64w; i++) {
            CHECK_PC(pc + 4);
            put_word(out, pc, W(i), t == 11 ? W_RAW : W_SWAP32);
            pc += 4;
        }
        return 1;
    }
    case 6: {
        emit_object_prefix(out, src);
        put_word(out, 144, W(32), W_SWAP32);
        int64_t pc = 212;
        for (int i = 49; i < n64w; i++) { CHECK_PC(pc + 4); put_word(out, pc, W(i), W_SWAP32); pc += 4; }
        return 1;
    }
    case 22:
        put_word(out, 0, W(0), W_HDR);
        put_word(out, 4, W(1), W_HH);
        return 1;
    case 37:
        put_word(out, 0, W(0), W_HDR);
        for (int i = 1; i < 9; i++) put_word(out, 4 * i, W(i), W_SWAP32);
        return 1;
    case 14: case 19: case 38: case 44: {
        int nidx = t == 44 ? 3 : 2;
        put_word(out, 0, W(0), W_HDR);
        for (int k = 0; k < nidx; k++) put_word(out, 8 + 8 * k, W(k + 1), W_SWAP32);
        return 1;
    }
    case 18:
        put_word(out, 0, W(0), W_HDR);
        put_word(out, 4, W(1), W_SWAP32);
        put_word(out, 8, W(2), W_1023);
        return 1;
    case 9:
        put_word(out, 0, W(0), W_HDR);
        for (int i = 1; i < 6; i++) put_word(out, 4 * i, W(i), W_HH);
        return 1;
    case 47: case 39: case 40: case 45: case 13: case 20: {
        static const int p39[] = { 41, 43 }, p40[] = { 43, 44 }, p45[] = { 32 }, p13[] = { 49, 50, 51 };
        static const int h39[] = { 33 };
        const int *ptr = NULL, *hh = NULL;
        int nptr = 0, nhh = 0, id_word = -1;
        if (t == 39) { ptr = p39; nptr = 2; hh = h39; nhh = 1; id_word = 32; }
        if (t == 40) { ptr = p40; nptr = 2; hh = h39; nhh = 1; id_word = 32; }
        if (t == 45) { ptr = p45; nptr = 1; }
        if (t == 13) { ptr = p13; nptr = 3; }
        emit_object_prefix(out, src);
        int64_t pc = 144;
        for (int i = 32; i < n64w; i++) {
            if (i == id_word) {
                pc = (pc + 7) & ~7LL;
                CHECK_PC(pc + 4);
                put_word(out, pc, W(i), W_SWAP32);
                pc += 8;
            } else if (in_set(i, ptr, nptr)) {
                pc = (pc + 7) & ~7LL;
                pc += 8;
            } else if ((t == 20 && i >= 32 && i < 45) || in_set(i, hh, nhh)) {
                CHECK_PC(pc + 4); put_word(out, pc, W(i), W_HH); pc += 4;
            } else {
                CHECK_PC(pc + 4); put_word(out, pc, W(i), W_SWAP32); pc += 4;
            }
        }
        CHECK_PC(pc);
        return 1;
    }
    case 30: case 32: case 33: case 35:
        put_word(out, 0, W(0), W_HDR);
        for (int i = 1; i < n64w - 1; i++) put_word(out, 4 * i, W(i), W_SWAP32);
        return 1;
    default:
        put_word(out, 0, W(0), W_HDR);
        for (int i = 1; i < n64w; i++) {
            CHECK_PC(4 * i + 4);
            put_word(out, 4 * i, W(i), W_SWAP32);
        }
        return 1;
    }
#undef W
#undef CHECK_PC
}

/* py: convert_stream(src, start, end) */
static int convert_propdefs(rc_ctx *c, const char *name, const rc_src *S, int64_t start, int64_t end,
                            rc_buf *pc_out)
{
    pc_out->n = 0;
    int64_t o = start;
    while (o < end) {
        if (o + 4 > end) { rc_fail(c, "%s: propDefs: truncated header at %#llx", name, (long long)o); return 0; }
        int t = bu8(S, o + 3);
        if (t > 48 || !PROPDEF_N64_WORDS[t]) { rc_fail(c, "%s: propDefs: unknown record type %d at %#llx", name, t, (long long)o); return 0; }
        int n64w = PROPDEF_N64_WORDS[t];
        if (o + 4 * n64w > end || !src_ok(S, o, 4 * n64w)) { rc_fail(c, "%s: propDefs: record overruns region end", name); return 0; }
        size_t at = pc_out->n;
        if (!buf_append(pc_out, NULL, (size_t)PROPDEF_PC_BYTES[t])) { rc_fail(c, "%s: out of memory", name); return 0; }
        if (!convert_record(c, name, S->p + o, t, pc_out->p + at))
            return 0;
        o += 4 * n64w;
        if (t == 48) break;
    }
    if (o != end) {
        int64_t pad = end - o;
        int nonzero = 0;
        for (int64_t i = o; i < end && i < S->n; i++)
            if (S->p[i]) nonzero = 1;
        if (pad < 0 || pad >= 16 || nonzero) {
            rc_fail(c, "%s: propDefs: walk ended at %#llx, region end %#llx", name, (long long)o, (long long)end);
            return 0;
        }
        if (!buf_append(pc_out, S->p + o, (size_t)pad)) { rc_fail(c, "%s: out of memory", name); return 0; }
    }
    return 1;
}

enum { K_HEADER, K_GROW, K_INTRO, K_PROPDEFS, K_S32ARR, K_CSTR, K_AISTREAM, K_GAP };
typedef struct {
    int64_t start, end; /* end < 0: unresolved */
    int kind, grow_idx;
    size_t order;       /* insertion order (py: stable sort) */
} sregion;

static int cmp_sregion(const void *a, const void *b)
{
    const sregion *x = a, *y = b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

typedef struct { int64_t field_off; int recsize, term_off, s32neg, newsz; } grow_desc;

static int d88_convert_usetup(rc_ctx *c, const char *name, const rc_src *S, rc_buf *res)
{
    const int64_t D = S->n;
    if (D < 0x28) { rc_fail(c, "%s: too small", name); return 0; }
    /* FIELD_NAMES order */
    enum { F_WP, F_WG, F_INTRO, F_PROPDEFS, F_PP, F_AL, F_PADS, F_BPADS, F_PN, F_BPN };
    uint32_t H[10];
    for (int i = 0; i < 10; i++) {
        H[i] = be32r(S, 4 * i);
        if (H[i] != 0 && H[i] >= D) { rc_fail(c, "%s: header field %d offset >= filesize", name, i); return 0; }
    }

    /* GROW_TABLES order: pads, boundpads, waypointgroups, pathwaypoints,
     * patrolpaths, ailists, padnames, boundpadnames */
    static const struct { int field, recsize, term_off, s32neg, newsz; } GT[8] = {
        { F_PADS, 44, 0x24, 0, 56 }, { F_BPADS, 68, 0x24, 0, 80 }, { F_WG, 12, 0, 0, 24 },
        { F_WP, 16, 0, 1, 24 }, { F_PP, 8, 0, 0, 16 }, { F_AL, 8, 0, 0, 16 },
        { F_PN, 4, 0, 0, 8 }, { F_BPN, 4, 0, 0, 8 },
    };
    vec starts[8] = { { 0 } };
    int64_t term[8];
    int ok = 0;
    vec leaf_start = { 0 }, leaf_end = { 0 }, leaf_kind = { 0 }, ai = { 0 }, ai_sorted = { 0 },
        intro_o = { 0 }, intro_t = { 0 };
    sregion *rg = NULL;
    size_t nrg = 0, caprg = 0;
    sregion *tiled = NULL;
    size_t ntiled = 0;
    int64_t *delta_at = NULL;
    uint8_t *has_delta = NULL;
    uint8_t *out = NULL;
    rc_buf propdefs_pc = { 0 };
    int have_propdefs = 0;

    /* walk_fixed */
    for (int g = 0; g < 8 && !c->failed; g++) {
        term[g] = -1;
        int64_t off = H[GT[g].field];
        if (!off) continue;
        int64_t o = off;
        int guard = 0;
        while (guard < 100000 && !c->failed) {
            guard++;
            uint32_t v = be32r(S, o + GT[g].term_off);
            int is_term = GT[g].s32neg ? ((int32_t)v < 0) : (v == 0);
            if (is_term) { term[g] = o; break; }
            vpush(&starts[g], o);
            o += GT[g].recsize;
        }
        if (term[g] < 0 && !c->failed) rc_fail(c, "%s: runaway walk_fixed at %#llx", name, (long long)off);
    }
    if (c->failed) goto done;

    /* intro */
    static const int INTRO_SZ[10] = { 12, 16, 16, 32, 8, 8, 40, 12, 8, 4 };
    int64_t intro_end = H[F_INTRO];
    if (H[F_INTRO]) {
        int64_t o = H[F_INTRO];
        int guard = 0, ended = 0;
        while (guard < 5000 && !c->failed) {
            guard++;
            int32_t t = bs32(S, o);
            if (t < 0 || t > 9) { rc_fail(c, "%s: unknown intro type %d at %#llx", name, t, (long long)o); break; }
            vpush(&intro_o, o); vpush(&intro_t, t);
            o += INTRO_SZ[t];
            if (t == 9) { ended = 1; break; }
        }
        if (!c->failed && !ended) rc_fail(c, "%s: intro walk runaway", name);
        intro_end = o;
    }
    if (c->failed) goto done;

    /* leaves */
#define ADD_LEAF(st, kind) do {                                                  \
        int64_t _s = (st);                                                       \
        if (_s != 0 && !vhas(&leaf_start, _s)) {                                 \
            int64_t _e = -1;                                                     \
            if ((kind) == K_S32ARR) {                                            \
                int64_t _o = _s; int _g = 0;                                     \
                while (_g < 100000 && !c->failed) {                              \
                    _g++;                                                        \
                    if (bs32(S, _o) < 0) { _e = _o + 4; break; }                 \
                    _o += 4; _g++;                                               \
                }                                                                \
                if (_e < 0 && !c->failed) rc_fail(c, "%s: runaway s32 array at %#llx", name, (long long)_s); \
            } else {                                                             \
                if (_s < D) {                                                    \
                    const void *_z = memchr(S->p + _s, 0, (size_t)(D - _s));      \
                    if (_z) _e = (const uint8_t *)_z - S->p + 1;                 \
                }                                                                \
                if (_e < 0) rc_fail(c, "%s: unterminated string at %#llx", name, (long long)_s); \
            }                                                                    \
            if (_e >= 0) { vpush(&leaf_start, _s); vpush(&leaf_end, _e); vpush(&leaf_kind, (kind)); } \
        }                                                                        \
    } while (0)
    /* GT index: 0 pads, 1 boundpads, 2 waygroups, 3 waypoints, 4 patrol, 5 ai, 6 pn, 7 bpn */
    for (size_t k = 0; k < starts[3].n; k++) ADD_LEAF(be32r(S, starts[3].v[k] + 4), K_S32ARR);
    for (size_t k = 0; k < starts[2].n; k++) {
        ADD_LEAF(be32r(S, starts[2].v[k] + 0), K_S32ARR);
        ADD_LEAF(be32r(S, starts[2].v[k] + 4), K_S32ARR);
    }
    for (size_t k = 0; k < starts[4].n; k++) ADD_LEAF(be32r(S, starts[4].v[k]), K_S32ARR);
    for (size_t k = 0; k < starts[0].n; k++) ADD_LEAF(be32r(S, starts[0].v[k] + 0x24), K_CSTR);
    for (size_t k = 0; k < starts[1].n; k++) ADD_LEAF(be32r(S, starts[1].v[k] + 0x24), K_CSTR);
    for (size_t k = 0; k < starts[6].n; k++) ADD_LEAF(be32r(S, starts[6].v[k]), K_CSTR);
    for (size_t k = 0; k < starts[7].n; k++) ADD_LEAF(be32r(S, starts[7].v[k]), K_CSTR);
#undef ADD_LEAF
    if (c->failed) goto done;
    for (size_t k = 0; k < starts[5].n; k++) {
        int64_t s = be32r(S, starts[5].v[k]);
        if (s && !vhas(&leaf_start, s) && !vhas(&ai, s)) vpush(&ai, s);
    }
    /* py: a set of ints; only its order among equal region starts could
     * matter, and the stable sort below orders by start first */
    vsorted_unique(&ai, &ai_sorted);

    /* assemble regions */
#define RPUSH(st, en, kd, gi) do {                                               \
        if (nrg == caprg) { caprg = caprg ? caprg * 2 : 64; rg = realloc(rg, caprg * sizeof(*rg)); if (!rg) abort(); } \
        rg[nrg] = (sregion){ (st), (en), (kd), (gi), nrg }; nrg++;               \
    } while (0)
    RPUSH(0, 0x28, K_HEADER, -1);
    for (int g = 0; g < 8; g++)
        if (term[g] >= 0) RPUSH((int64_t)H[GT[g].field], term[g] + GT[g].recsize, K_GROW, g);
    if (H[F_INTRO]) RPUSH((int64_t)H[F_INTRO], intro_end, K_INTRO, -1);
    if (H[F_PROPDEFS]) RPUSH((int64_t)H[F_PROPDEFS], -1, K_PROPDEFS, -1);
    for (size_t k = 0; k < leaf_start.n; k++) RPUSH(leaf_start.v[k], leaf_end.v[k], (int)leaf_kind.v[k], -1);
    for (size_t k = 0; k < ai_sorted.n; k++) RPUSH(ai_sorted.v[k], -1, K_AISTREAM, -1);
#undef RPUSH
    qsort(rg, nrg, sizeof(*rg), cmp_sregion);
    for (size_t i = 0; i < nrg; i++)
        if (rg[i].end < 0) rg[i].end = i + 1 < nrg ? rg[i + 1].start : D;

    /* tiling */
    tiled = calloc(2 * nrg + 2, sizeof(*tiled));
    if (!tiled) { rc_fail(c, "%s: out of memory", name); goto done; }
    {
        int64_t cursor = 0;
        for (size_t i = 0; i < nrg; i++) {
            if (rg[i].start < cursor) { rc_fail(c, "%s: region overlap at %#llx (cursor %#llx)", name, (long long)rg[i].start, (long long)cursor); goto done; }
            if (rg[i].start > cursor) tiled[ntiled++] = (sregion){ cursor, rg[i].start, K_GAP, -1, 0 };
            if (rg[i].end < rg[i].start) { rc_fail(c, "%s: region with negative length", name); goto done; }
            tiled[ntiled++] = rg[i];
            cursor = rg[i].end;
        }
        if (cursor < D) tiled[ntiled++] = (sregion){ cursor, D, K_GAP, -1, 0 };
        else if (cursor > D) { rc_fail(c, "%s: regions overrun EOF", name); goto done; }
    }

    if (H[F_PROPDEFS]) {
        int64_t pd_end = -1;
        for (size_t i = 0; i < ntiled; i++)
            if (tiled[i].kind == K_PROPDEFS) { pd_end = tiled[i].end; break; }
        if (!convert_propdefs(c, name, S, H[F_PROPDEFS], pd_end, &propdefs_pc)) goto done;
        have_propdefs = 1;
    }

    /* pass 1: cumulative delta at each region start (py: dict, last wins) */
    delta_at = calloc((size_t)D + 1, sizeof(int64_t));
    has_delta = calloc((size_t)D + 1, 1);
    if (!delta_at || !has_delta) { rc_fail(c, "%s: out of memory", name); goto done; }
    int64_t cum = 0x28;
    for (size_t i = 0; i < ntiled; i++) {
        sregion *r = &tiled[i];
        if (r->kind == K_HEADER) { delta_at[r->start] = 0; has_delta[r->start] = 1; continue; }
        if (r->start <= D) { delta_at[r->start] = cum; has_delta[r->start] = 1; }
        if (r->kind == K_GROW) {
            int g = r->grow_idx;
            int64_t n = (int64_t)starts[g].n;      /* records before the terminator */
            cum += (n + 1) * (GT[g].newsz - GT[g].recsize);
        } else if (r->kind == K_PROPDEFS && have_propdefs) {
            cum += (int64_t)propdefs_pc.n - (r->end - r->start);
        }
    }
#define RELOC(off) ((off) == 0 ? 0 : (int64_t)(off) + (((int64_t)(off) >= 0 && (int64_t)(off) <= D && has_delta[(off)]) ? delta_at[(off)] : 0))

    const int64_t total_new = D + cum;
    out = calloc((size_t)total_new + 1, 1);
    if (!out) { rc_fail(c, "%s: out of memory", name); goto done; }
    rc_out O = { out, total_new, c, name };

    for (int i = 0; i < 10; i++) {
        put_le32(&O, 8 * i, (uint32_t)RELOC(H[i]));
        put_zero(&O, 8 * i + 4, 4);
    }

    for (int g = 0; g < 8 && !c->failed; g++) {
        if (term[g] < 0) continue;
        int64_t base = RELOC(H[GT[g].field]);
        for (size_t idx = 0; idx <= starts[g].n && !c->failed; idx++) {
            int64_t so = idx < starts[g].n ? starts[g].v[idx] : term[g];
            int64_t d = base + (int64_t)idx * GT[g].newsz;
            switch (g) {
            case 0: case 1:
                for (int k = 0; k < 9; k++) put_le32(&O, d + 4 * k, be32r(S, so + 4 * k));
                put_le32(&O, d + 0x28, (uint32_t)RELOC(be32r(S, so + 0x24)));
                put_zero(&O, d + 0x2c, 4);
                put_zero(&O, d + 0x30, 8);
                if (g == 1)
                    for (int k = 0; k < 6; k++) put_le32(&O, d + 0x38 + 4 * k, be32r(S, so + 0x2c + 4 * k));
                break;
            case 2: {
                uint32_t nb = be32r(S, so), wpv = be32r(S, so + 4), dist = be32r(S, so + 8);
                put_le32(&O, d, nb ? (uint32_t)RELOC(nb) : 0); put_zero(&O, d + 4, 4);
                put_le32(&O, d + 8, wpv ? (uint32_t)RELOC(wpv) : 0); put_zero(&O, d + 12, 4);
                put_le32(&O, d + 16, dist); put_zero(&O, d + 20, 4);
                break;
            }
            case 3: {
                uint32_t padID = be32r(S, so), nb = be32r(S, so + 4);
                uint32_t gn = be32r(S, so + 8), dist = be32r(S, so + 12);
                put_le32(&O, d, padID); put_zero(&O, d + 4, 4);
                put_le32(&O, d + 8, nb ? (uint32_t)RELOC(nb) : 0); put_zero(&O, d + 12, 4);
                put_le32(&O, d + 16, gn); put_le32(&O, d + 20, dist);
                break;
            }
            case 4: {
                uint32_t wpv = be32r(S, so);
                put_le32(&O, d, wpv ? (uint32_t)RELOC(wpv) : 0); put_zero(&O, d + 4, 4);
                put_bytes(&O, d + 8, S, so + 4, 2);
                put_le16(&O, d + 10, bu16(S, so + 6));
                put_zero(&O, d + 12, 4);
                break;
            }
            case 5: {
                uint32_t al = be32r(S, so), id = be32r(S, so + 4);
                put_le32(&O, d, al ? (uint32_t)RELOC(al) : 0); put_zero(&O, d + 4, 4);
                put_le32(&O, d + 8, id); put_zero(&O, d + 12, 4);
                break;
            }
            case 6: case 7: {
                uint32_t p = be32r(S, so);
                put_le32(&O, d, p ? (uint32_t)RELOC(p) : 0); put_zero(&O, d + 4, 4);
                break;
            }
            }
        }
    }

    if (H[F_INTRO]) {
        int64_t idelta = has_delta[H[F_INTRO]] ? delta_at[H[F_INTRO]] : 0;
        for (size_t k = 0; k < intro_o.n && !c->failed; k++) {
            int64_t o = intro_o.v[k], d = o + idelta;
            int t = (int)intro_t.v[k];
            if (t == 6) {
                for (int q = 0; q < 10; q++) {
                    if (q == 7) {
                        put_le16(&O, d + 4 * q, bu16(S, o + 4 * q));
                        put_le16(&O, d + 4 * q + 2, bu16(S, o + 4 * q + 2));
                    } else {
                        put_le32(&O, d + 4 * q, be32r(S, o + 4 * q));
                    }
                }
            } else {
                for (int q = 0; q < INTRO_SZ[t] / 4; q++) put_le32(&O, d + 4 * q, be32r(S, o + 4 * q));
            }
        }
    }

    for (size_t k = 0; k < leaf_start.n && !c->failed; k++) {
        int64_t st = leaf_start.v[k], en = leaf_end.v[k], d = RELOC(st);
        if (leaf_kind.v[k] == K_S32ARR) {
            for (int64_t o = st; o < en; o += 4) put_le32(&O, d + (o - st), be32r(S, o));
        } else {
            put_bytes(&O, d, S, st, en - st);
        }
    }

    for (size_t i = 0; i < ntiled && !c->failed; i++) {
        sregion *r = &tiled[i];
        if (r->kind == K_PROPDEFS && have_propdefs) {
            int64_t d = RELOC(r->start);
            if (out_ok(&O, d, (int64_t)propdefs_pc.n)) memcpy(out + d, propdefs_pc.p, propdefs_pc.n);
        } else if (r->kind == K_AISTREAM || r->kind == K_GAP) {
            put_bytes(&O, RELOC(r->start), S, r->start, r->end - r->start);
        }
    }
#undef RELOC
    if (c->failed) goto done;
    res->n = 0;
    if (!buf_append(res, out, (size_t)total_new)) { rc_fail(c, "%s: out of memory", name); goto done; }
    ok = 1;
done:
    for (int g = 0; g < 8; g++) free(starts[g].v);
    free(leaf_start.v); free(leaf_end.v); free(leaf_kind.v); free(ai.v); free(ai_sorted.v);
    free(intro_o.v); free(intro_t.v); free(rg); free(tiled); free(delta_at); free(has_delta);
    free(out); free(propdefs_pc.p);
    return ok;
}

/* ============================================================ output === */

static int mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int write_file(rc_ctx *c, const char *path, const void *data, size_t n)
{
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { rc_fail(c, "can't write %s (%s)", tmp, strerror(errno)); return 0; }
    int ok = fwrite(data, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        rc_fail(c, "can't write %s", path);
        return 0;
    }
    return 1;
}

/* py: emit() into a concatenated image (16-aligned starts) + csv rows */
typedef struct { rc_buf bin, csv; } sidecar;

static int sc_emit(sidecar *sc, const char *name, const uint8_t *data, size_t n)
{
    size_t start = (sc->bin.n + 15) & ~(size_t)15;
    if (!buf_append(&sc->bin, NULL, start - sc->bin.n)) return 0;
    char line[512];
    int l = snprintf(line, sizeof(line), "%s,%zu,%zu\r\n", name, start, n);
    if (!buf_append(&sc->csv, line, (size_t)l)) return 0;
    return buf_append(&sc->bin, data, n);
}

static int sc_write(rc_ctx *c, sidecar *sc, const char *dir, const char *binname)
{
    char path[1100];
    if (mkdirs(dir) != 0) { rc_fail(c, "can't create %s (%s)", dir, strerror(errno)); return 0; }
    snprintf(path, sizeof(path), "%s/manifest.csv", dir);
    if (!write_file(c, path, sc->csv.p, sc->csv.n)) return 0;
    snprintf(path, sizeof(path), "%s/%s", dir, binname);
    return write_file(c, path, sc->bin.p, sc->bin.n);
}

int ge_convert_sidecars(const uint8_t *rom, size_t rom_size, const char *out_root,
                        void (*progress)(float done, void *user), void *user,
                        char *err, size_t err_size)
{
    static char dummy[1];
    rc_ctx C = { err ? err : dummy, err ? err_size : 1, 0 };
    if (err && err_size) err[0] = 0;
    const size_t nm = sizeof(rc_models) / sizeof(rc_models[0]);
    const size_t ncg = sizeof(rc_cg) / sizeof(rc_cg[0]);
    const size_t total = nm + ncg;
    size_t done_n = 0;
    sidecar models = { { 0 } }, cg = { { 0 } };
    rc_buf comp = { 0 }, dec = { 0 }, conv = { 0 };
    char dir[1100];

    static const char hdr[] = "name,offset,size\r\n";
    buf_append(&models.csv, hdr, sizeof(hdr) - 1);
    buf_append(&cg.csv, hdr, sizeof(hdr) - 1);

    for (size_t i = 0; i < nm && !C.failed; i++) {
        if (d43_process(&C, rom, rom_size, &rc_models[i], &comp))
            if (!sc_emit(&models, rc_models[i].name, comp.p, comp.n)) rc_fail(&C, "out of memory");
        if (progress) progress((float)++done_n / (float)total, user);
    }
    if (!C.failed) {
        /* pcmodels.bin is padded to a multiple of 16 */
        size_t pad = (16 - (models.bin.n % 16)) % 16;
        buf_append(&models.bin, NULL, pad);
    }

    for (size_t i = 0; i < ncg && !C.failed; i++) {
        const rc_cg_entry *e = &rc_cg[i];
        if ((uint64_t)e->addr + e->size > rom_size) { rc_fail(&C, "%s: ROM range out of bounds", e->name); break; }
        const uint8_t *raw = rom + e->addr;
        if (e->kind == RC_SEG) {
            rc_src S = { raw, e->size, &C, e->name };
            if (d69_convert_seg(&C, e->name, &S, &conv))
                if (!sc_emit(&cg, e->name, conv.p, conv.n)) rc_fail(&C, "out of memory");
        } else {
            if (e->size < 2 || raw[0] != 0x11 || raw[1] != 0x72) { rc_fail(&C, "%s: bad RZ magic", e->name); break; }
            if (!rz_inflate(&C, e->name, raw + 2, e->size - 2, &dec)) break;
            rc_src S = { dec.p, (int64_t)dec.n, &C, e->name };
            int r = e->kind == RC_STAN ? d69_convert_stan(&C, e->name, &S, &conv)
                                       : d88_convert_usetup(&C, e->name, &S, &conv);
            if (r && rz_deflate(&C, e->name, conv.p, conv.n, &comp))
                if (!sc_emit(&cg, e->name, comp.p, comp.n)) rc_fail(&C, "out of memory");
        }
        if (progress) progress((float)++done_n / (float)total, user);
    }

    if (!C.failed) {
        snprintf(dir, sizeof(dir), "%s/pcmodels-%s", out_root, RC_TABLE_REGION);
        sc_write(&C, &models, dir, "pcmodels.bin");
    }
    if (!C.failed) {
        snprintf(dir, sizeof(dir), "%s/pccg-%s", out_root, RC_TABLE_REGION);
        sc_write(&C, &cg, dir, "pccg.bin");
    }
    free(models.bin.p); free(models.csv.p); free(cg.bin.p); free(cg.csv.p);
    free(comp.p); free(dec.p); free(conv.p);
    return C.failed ? -1 : 0;
}
