// SPDX-License-Identifier: GPL-2.0-or-later
// Portable SPIR-V repair for subgroup-uniform wave64 scratch stores.
// nhlport: taken unchanged from the NHL24 Be a Pro fork (github.com/torpedatorpeda/NHL24-Be-a-Pro, R11).
// The existing workgroup barriers remain in place; no shader hashes are used.
#ifndef NHL_WAVE64_SCRATCH_H
#define NHL_WAVE64_SCRATCH_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const uint32_t *words; uint16_t opcode, count; } NhlSpvDef;
typedef struct { const NhlSpvDef *defs; uint32_t bound, local_index; } NhlSpvModule;

static uint32_t nhl_spv_constant(const NhlSpvModule *m, uint32_t id, uint32_t fallback) {
    if (id >= m->bound) return fallback;
    const NhlSpvDef d = m->defs[id];
    if (d.opcode != 43 || d.count != 4) return fallback;
    return d.words[3];
}

static int nhl_spv_uniform(const NhlSpvModule *m, uint32_t id, unsigned depth) {
    if (id >= m->bound || depth > 24) return 0;
    const NhlSpvDef d = m->defs[id];
    const uint32_t *w = d.words;
    if (!w) return 0;
    if (d.opcode == 43 || d.opcode == 46) return 1; // Constant / ConstantNull
    if (d.opcode == 337 || d.opcode == 338 || d.opcode == 339)
        return d.count >= 5 && nhl_spv_constant(m, w[3], 0) == 3;
    if (d.opcode == 124 || d.opcode == 81) // Bitcast / CompositeExtract
        return d.count >= 4 && nhl_spv_uniform(m, w[3], depth+1);
    if (d.opcode == 79) // VectorShuffle: both vectors must be uniform
        return d.count >= 6 && nhl_spv_uniform(m, w[3], depth+1) &&
               nhl_spv_uniform(m, w[4], depth+1);
    return 0;
}

static int nhl_spv_half_index(const NhlSpvModule *m, uint32_t id) {
    if (id >= m->bound) return 0;
    NhlSpvDef d = m->defs[id];
    if (d.opcode != 194 || d.count != 5 || nhl_spv_constant(m, d.words[4], 0) != 5)
        return 0;
    id = d.words[3];
    if (id >= m->bound) return 0;
    d = m->defs[id];
    return d.opcode == 61 && d.count >= 4 && d.words[3] == m->local_index;
}

static int nhl_spv_half_byte_offset(const NhlSpvModule *m, uint32_t id) {
    if (id >= m->bound) return 0;
    const NhlSpvDef d = m->defs[id];
    return d.opcode == 196 && d.count == 5 &&
           nhl_spv_constant(m, d.words[4], 0) == 2 && nhl_spv_half_index(m, d.words[3]);
}

static int nhl_spv_scratch_index(const NhlSpvModule *m, uint32_t id) {
    if (nhl_spv_half_index(m, id)) return 1;
    if (id >= m->bound) return 0;
    NhlSpvDef d = m->defs[id];
    if (d.opcode != 194 || d.count != 5 || nhl_spv_constant(m, d.words[4], 0) != 2)
        return 0;
    id = d.words[3];
    if (nhl_spv_half_byte_offset(m, id)) return 1;
    if (id >= m->bound) return 0;
    d = m->defs[id];
    if (d.opcode != 128 || d.count != 5) return 0;
    uint32_t a = nhl_spv_constant(m, d.words[3], UINT32_MAX);
    uint32_t b = nhl_spv_constant(m, d.words[4], UINT32_MAX);
    return (a != UINT32_MAX && !(a & 7) && nhl_spv_half_byte_offset(m, d.words[4])) ||
           (b != UINT32_MAX && !(b & 7) && nhl_spv_half_byte_offset(m, d.words[3]));
}

static uint32_t nhl_spv_scratch_store(const NhlSpvModule *m, const uint32_t *w,
                                     const uint32_t *next, const uint32_t *end) {
    if ((w[0] >> 16) != 3 || w[1] >= m->bound || !nhl_spv_uniform(m, w[2], 0)) return 0;
    const NhlSpvDef ptr = m->defs[w[1]];
    if (ptr.opcode != 65 || ptr.count != 5 || !nhl_spv_scratch_index(m, ptr.words[4])) return 0;
    if (ptr.words[1] >= m->bound || ptr.words[3] >= m->bound) return 0;
    const NhlSpvDef type = m->defs[ptr.words[1]], variable = m->defs[ptr.words[3]];
    if (type.opcode != 32 || type.count != 4 || type.words[2] != 4 ||
        variable.opcode != 59 || variable.count < 4 || variable.words[3] != 4) return 0;
    const uint32_t value_type = type.words[3];
    if (value_type >= m->bound) return 0;
    const NhlSpvDef integer = m->defs[value_type];
    if (integer.opcode != 21 || integer.count != 4 || integer.words[2] != 32 || integer.words[3]) return 0;
    // Only the lowering's store immediately followed by its full workgroup barrier.
    if (end-next < 4 || (next[0] & 65535) != 224 || (next[0] >> 16) != 4 ||
        nhl_spv_constant(m, next[1], 0) != 2 || nhl_spv_constant(m, next[2], 0) != 2 ||
        nhl_spv_constant(m, next[3], 0) != 0x108) return 0;
    return value_type;
}

// NULL means no recognized repair (or invalid/unsupported input): forward unchanged.
// Caller owns the returned array. Inputs and cached SPIR-V are never modified.
static uint32_t *nhl_wave64_scratch_atomic(const uint32_t *code, size_t words,
                                          size_t *output_words, uint32_t *store_count) {
    *output_words = words;
    *store_count = 0;
    if (!code || words < 5 || words > 1048576 || code[0] != 0x07230203 ||
        !code[3] || code[3] > 65536) return NULL;
    NhlSpvDef *defs = (NhlSpvDef *)calloc(code[3], sizeof(*defs));
    if (!defs) return NULL;
    NhlSpvModule module = {defs, code[3], 0};
    const uint32_t *end = code+words;
    int compute = 0, malformed = 0;
    for (const uint32_t *w = code+5; w < end;) {
        uint32_t n = w[0] >> 16, op = w[0] & 65535, id = 0;
        if (!n || n > (size_t)(end-w)) { malformed = 1; break; }
        if (op == 15 && n >= 4 && w[1] == 5) compute = 1;
        if (op == 71 && n == 4 && w[2] == 11 && w[3] == 29) module.local_index = w[1];
        if ((op == 21 || op == 32) && n >= 2) id = w[1];
        if ((op == 43 || op == 46 || op == 59 || op == 61 || op == 65 ||
             op == 79 || op == 81 || op == 124 || op == 128 || op == 194 ||
             op == 196 || op == 337 || op == 338 || op == 339) && n >= 3) id = w[2];
        if (id && id < module.bound) { defs[id].words = w; defs[id].opcode = (uint16_t)op; defs[id].count = (uint16_t)n; }
        w += n;
    }
    if (malformed || !compute || !module.local_index) { free(defs); return NULL; }
    uint32_t repairs = 0, zero = 0, scope = 0;
    for (const uint32_t *w = code+5; w < end; w += w[0] >> 16) {
        if ((w[0] & 65535) != 62) continue;
        uint32_t type = nhl_spv_scratch_store(&module, w, w+(w[0] >> 16), end);
        if (!type) continue;
        // All recognized stores must use one unsigned 32-bit type.
        uint32_t z = 0, s = 0;
        for (uint32_t i = 1; i < module.bound; ++i) {
            if (defs[i].opcode != 43 || defs[i].count != 4 || defs[i].words[1] != type) continue;
            if (defs[i].words[3] == 0) z = i;
            if (defs[i].words[3] == 2) s = i;
        }
        if (!z || !s || (repairs && (z != zero || s != scope))) { free(defs); return NULL; }
        zero = z; scope = s; ++repairs;
    }
    if (!repairs || repairs > 4096 || module.bound > UINT32_MAX-repairs) { free(defs); return NULL; }
    uint32_t *out = (uint32_t *)malloc((words+4*(size_t)repairs)*sizeof(uint32_t));
    if (!out) { free(defs); return NULL; }
    memcpy(out, code, 5*sizeof(uint32_t));
    out[3] += repairs;
    size_t at = 5;
    uint32_t result_id = module.bound;
    for (const uint32_t *w = code+5; w < end; w += w[0] >> 16) {
        uint32_t n = w[0] >> 16;
        uint32_t type = (w[0] & 65535) == 62 ? nhl_spv_scratch_store(&module, w, w+n, end) : 0;
        if (type) {
            out[at++] = (7u << 16) | 229u; // OpAtomicExchange
            out[at++] = type; out[at++] = result_id++;
            out[at++] = w[1]; out[at++] = scope; out[at++] = zero; out[at++] = w[2];
        } else { memcpy(out+at, w, n*sizeof(uint32_t)); at += n; }
    }
    free(defs);
    *output_words = at; *store_count = repairs;
    return out;
}

static int nhl_spv_workgroup_pointer(const NhlSpvModule *m, uint32_t id) {
    if (id >= m->bound) return 0;
    const NhlSpvDef d = m->defs[id];
    if ((d.opcode != 65 && d.opcode != 59) || d.count < 4 || d.words[1] >= m->bound) return 0;
    const NhlSpvDef type = m->defs[d.words[1]];
    return type.opcode == 32 && type.count == 4 && type.words[2] == 4;
}

// A subgroup memory fence does not stop another invocation from starting a
// shared-memory write. Promote the fence at a uniform selection header when
// that block read shared data and the following selection writes shared data.
// Never place an execution barrier inside the divergent selection itself.
static uint32_t *nhl_shared_read_write_barriers(const uint32_t *code, size_t words,
                                                size_t *output_words, uint32_t *barrier_count) {
    *output_words = words; *barrier_count = 0;
    if (!code || words < 5 || words > 1048576 || code[0] != 0x07230203 ||
        !code[3] || code[3] > 65536) return NULL;
    NhlSpvDef *defs = (NhlSpvDef *)calloc(code[3], sizeof(*defs));
    unsigned char *patch = (unsigned char *)calloc(words, 1);
    if (!defs || !patch) { free(defs); free(patch); return NULL; }
    NhlSpvModule module = {defs, code[3], 0};
    const uint32_t *end = code+words;
    int compute = 0, malformed = 0; unsigned functions = 0;
    for (const uint32_t *w = code+5; w < end;) {
        uint32_t n = w[0] >> 16, op = w[0] & 65535, id = 0;
        if (!n || n > (size_t)(end-w)) { malformed = 1; break; }
        if (op == 15 && n >= 4 && w[1] == 5) compute = 1;
        if (op == 54) ++functions;
        // Loops, switches and terminating/discarding branches need a more
        // complete convergence analysis; leave these modules unchanged.
        if (op == 246 || op == 251 || op == 252 || op == 255 || op == 4416) malformed = 1;
        if ((op == 21 || op == 32) && n >= 2) id = w[1];
        if ((op == 43 || op == 59 || op == 65) && n >= 3) id = w[2];
        if (id && id < module.bound) { defs[id].words=w; defs[id].opcode=(uint16_t)op; defs[id].count=(uint16_t)n; }
        w += n;
    }
    if (malformed || !compute || functions != 1) { free(defs); free(patch); return NULL; }
    uint32_t merges[64], pending_merge = 0, depth = 0, workgroup_scope = 0, repairs = 0;
    int read_shared = 0;
    for (const uint32_t *w = code+5; w < end; w += w[0] >> 16) {
        uint32_t n = w[0] >> 16, op = w[0] & 65535;
        if (op == 248 && n == 2) { // OpLabel
            if (depth && merges[depth-1] == w[1]) --depth;
            read_shared = 0;
        }
        if ((op == 253 || op == 254) && depth) { malformed = 1; break; }
        if (op == 61 && n >= 4 && nhl_spv_workgroup_pointer(&module, w[3])) read_shared = 1;
        if (op == 224) read_shared = 0;
        if (op == 225 && n == 3 && !depth && read_shared &&
            nhl_spv_constant(&module,w[1],0) == 3 && nhl_spv_constant(&module,w[2],0) == 0x108 &&
            end-(w+n) >= 7 && ((w+n)[0] & 65535) == 247 && ((w+n)[0] >> 16) == 3 &&
            ((w+n+3)[0] & 65535) == 250 && ((w+n+3)[0] >> 16) == 4) {
            const uint32_t merge = (w+n)[1];
            int writes_shared = 0, found_merge = 0;
            for (const uint32_t *q = w+n+7; q < end; q += q[0] >> 16) {
                uint32_t qn = q[0] >> 16, qo = q[0] & 65535;
                if (qo == 248 && qn == 2 && q[1] == merge) { found_merge = 1; break; }
                if (qo == 62 && qn >= 3 && nhl_spv_workgroup_pointer(&module,q[1])) writes_shared = 1;
            }
            if (found_merge && writes_shared) {
                uint32_t scope_type = w[1] < module.bound && defs[w[1]].opcode == 43 ? defs[w[1]].words[1] : 0;
                uint32_t scope = 0;
                for (uint32_t i = 1; i < module.bound; ++i)
                    if (defs[i].opcode == 43 && defs[i].count == 4 &&
                        defs[i].words[1] == scope_type && defs[i].words[3] == 2) scope = i;
                if (scope && (!workgroup_scope || scope == workgroup_scope)) {
                    workgroup_scope = scope; patch[w-code] = 1; ++repairs; read_shared = 0;
                }
            }
        }
        if (op == 247 && n == 3) pending_merge = w[1];
        if (op == 250) {
            if (!pending_merge || depth == 64) { malformed = 1; break; }
            merges[depth++] = pending_merge; pending_merge = 0;
        }
    }
    if (malformed || depth || !repairs || repairs > 4096) { free(defs); free(patch); return NULL; }
    uint32_t *out = (uint32_t *)malloc((words+repairs)*sizeof(uint32_t));
    if (!out) { free(defs); free(patch); return NULL; }
    memcpy(out,code,5*sizeof(uint32_t));size_t at=5;
    for (const uint32_t *w=code+5;w<end;w+=w[0]>>16) {
        uint32_t n=w[0]>>16;
        if (patch[w-code]) {
            out[at++]=(4u<<16)|224u;out[at++]=workgroup_scope;out[at++]=workgroup_scope;out[at++]=w[2];
        } else { memcpy(out+at,w,n*sizeof(uint32_t));at+=n; }
    }
    free(defs);free(patch);*output_words=at;*barrier_count=repairs;return out;
}
#endif
