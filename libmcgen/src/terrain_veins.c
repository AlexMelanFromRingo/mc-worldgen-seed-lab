/* terrain_veins.c — жилы руд 26.3+: правила материала "minecraft:ore_vein" (OreVeinRule) из material_rule настроек шума.
 *
 * В 26.3 жилы — часть системы материалов (стадия поверхности игры). Для стадии TERRAIN libmcgen берём из верхней
 * последовательности material_rule только правила ore_vein (как вариант эталонов «veins»): они применяются к каждому
 * «камню» (не воздух и не жидкость) столбца сверху вниз; первое правило, вернувшее блок, побеждает.
 * Плотность и «богатство» берутся из объёма (prefill = true: sampleVolume по суженному объёму чанка в контексте
 * NoiseChunk), «щель» — точечно; случайность — позиционная фабрика "minecraft:ore" (RandomState).
 */
#include "mcgen_internal.h"
#include <stdlib.h>
#include <stdio.h>

typedef struct { const S *density, *richness, *gap; int ore, raw, filler; float raw_chance; Df *df[3]; } VeinRule;
typedef struct { int n; VeinRule r[8]; } Veins;

static JsDoc *load_rule(const McGen *g, const char *id) {
    char *full = df_full_id(id);
    char *path = xsprintf("%s/data/minecraft/worldgen/material_rule/%s.json", g->pack, strchr(full, ':') + 1);
    JsDoc *d = js_parse_file(path, NULL, 0);
    free(path); free(full);
    return d;
}

static int parse_vein(McWorld *w, const Js *r, VeinRule *v, char *err, size_t errlen) {
    const McGen *g = w->g;
    memset(v, 0, sizeof *v);
    v->ore = gen_state_from_json(g, js_get(r, "ore_block"));
    v->raw = gen_state_from_json(g, js_get(r, "raw_ore_block"));
    v->filler = gen_state_from_json(g, js_get(r, "filler_block"));
    v->raw_chance = js_numf(js_get(r, "raw_ore_chance"), 0.02f);
    if (v->ore < 0 || v->raw < 0 || v->filler < 0) { set_err(err, errlen, "ore_vein: неизвестный блок"); return -1; }
    static const char *K[3] = { "density", "richness", "filler_gap" };
    const S **dst[3] = { &v->density, &v->richness, &v->gap };
    for (int k = 0; k < 3; k++) {
        v->df[k] = df_parse(js_get(r, K[k]), 1, err, errlen);
        if (!v->df[k]) return -1;
        *dst[k] = nc_get(w->nc, v->df[k], err, errlen);
        if (!*dst[k]) return -1;
    }
    return 0;
}

/* вызывается из mcgen_world_new (26.3+) */
int veins_init(McWorld *w, char *err, size_t errlen) {
    const Js *root = js_root(w->ns->doc);
    const char *mr = js_str(js_get(root, "material_rule"), NULL);
    Veins *V = xcalloc(1, sizeof *V);
    w->veins = V;
    if (!mr) return 0;
    JsDoc *d = load_rule(w->g, mr);
    if (!d) return 0;
    const Js *r = js_root(d);
    const char *t = js_str(js_get(r, "type"), "");
    if (strstr(t, "sequence")) {
        Js *seq = js_get(r, "sequence");
        for (int i = 0; seq && i < seq->n && V->n < 8; i++) {
            const Js *it = seq->items[i];
            JsDoc *sub = NULL;
            if (js_is_str(it)) { sub = load_rule(w->g, it->s); it = sub ? js_root(sub) : NULL; }
            if (it && strstr(js_str(js_get(it, "type"), ""), "ore_vein")) {
                if (parse_vein(w, it, &V->r[V->n], err, errlen)) { js_free(sub); js_free(d); return -1; }
                V->n++;
            }
            js_free(sub);
        }
    }
    js_free(d);
    return 0;
}
void veins_free(McWorld *w) {
    Veins *V = w->veins; if (!V) return;
    for (int i = 0; i < V->n; i++) for (int k = 0; k < 3; k++) df_free(V->r[i].df[k]);
    free(V); w->veins = NULL;
}

/* blocks: [y][z][x] мира; блоки шума — строки y0 … y0+ny-1 (относительно min_y мира) */
void veins_apply_new(McWorld *w, SCtx *x, int cx, int cz, int y0, int ny, uint16_t *blocks) {
    const McGen *g = w->g;
    Veins *V = w->veins;
    if (!V || V->n == 0) return;
    /* суженный объём: до верха самой высокой непустой секции */
    int top = -1;
    for (int y = w->height - 1; y >= 0 && top < 0; y--)
        for (int i = 0; i < 256; i++) if (!gen_is_air(g, blocks[(size_t)y * 256 + i])) { top = y; break; }
    if (top < 0) return;
    int max_by = ((w->min_y + top) >> 4) * 16 + 15;
    int nmin = w->min_y + y0;
    int sy = max_by - nmin + 1; if (sy < 1) sy = 1;
    Vol v = { 16, sy, 16, cx * 16, nmin, cz * 16, 1, 1, 1 };
    int n = vol_size(&v);
    float *dens[8], *rich[8];
    for (int i = 0; i < V->n; i++) {   /* порядок как при компиляции правил: density, richness для каждого правила */
        dens[i] = xmalloc(sizeof(float) * (size_t)n); s_volume(x, V->r[i].density, dens[i], &v);
        rich[i] = xmalloc(sizeof(float) * (size_t)n); s_volume(x, V->r[i].richness, rich[i], &v);
    }
    int ylo = w->min_y, yhi = w->min_y + w->height - 1;
    for (int xx = 0; xx < 16; xx++) for (int z = 0; z < 16; z++) {
        int bx = cx * 16 + xx, bz = cz * 16 + z;
        for (int y = w->height - 1; y >= 0; y--) {
            uint16_t *pst = &blocks[((size_t)y * 16 + z) * 16 + xx];
            int st = *pst;
            if (gen_is_air(g, st) || (g->state_cls[st] & 4)) continue;    /* воздух или жидкость */
            int by = w->min_y + y;
            if (by < ylo || by > yhi) continue;
            int idx = vol_index_of_block(&v, bx, by, bz);
            for (int i = 0; i < V->n; i++) {
                const VeinRule *r = &V->r[i];
                float d = idx >= 0 ? dens[i][idx] : s_value(x, r->density, bx, by, bz);
                if (d <= 0.0f) continue;
                Rnd rr = pos_at(&w->ore_pos, bx, by, bz);
                if (rnd_next_float(&rr) > d) continue;
                float rich_v = idx >= 0 ? rich[i][idx] : s_value(x, r->richness, bx, by, bz);
                int out;
                if (rnd_next_float(&rr) < rich_v && s_value(x, r->gap, bx, by, bz) < 0.0f)
                    out = rnd_next_float(&rr) < r->raw_chance ? r->raw : r->ore;
                else out = r->filler;
                *pst = (uint16_t)out;
                break;
            }
        }
    }
    for (int i = 0; i < V->n; i++) { free(dens[i]); free(rich[i]); }
}
