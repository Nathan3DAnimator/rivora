// (2026-11-25, item 12) --i18n-dump FILE: every text the editor shows from its tables rather than from a TR("...") in the
// code (the Blueprint nodes' names, categories, descriptions and pins, the components, the asset kinds, the classes to
// place): one "kind<TAB>text" a line. tools/i18n_audit.py reads it (the i18n_audit test makes it first): each must be in
// the translation tables, in Portuguese and Spanish.
#include <stdio.h>
#include <string.h>

#include "blueprint.h"
#include "components.h"
#include "content.h"
#include "editor.h"
#include "material.h"
#include "i18n.h"

static void line(FILE* f, const char* kind, const char* s, size_t n) {
    bool letters = false;
    for (size_t i = 0; i < n && !letters; i++) letters = (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z');
    if (!letters) return;
    fprintf(f, "%s\t%.*s\n", kind, (int)n, s);
}
static void parts(FILE* f, const char* kind, const char* path) { // "Utilities|Time": each box's name
    for (const char* c = path; c && *c;) {
        const char* bar = strchr(c, '|');
        size_t l = bar ? (size_t)(bar - c) : strlen(c);
        line(f, kind, c, l);
        c = bar ? bar + 1 : c + l;
    }
}

int i18n_dump(const char* path) {
    g_i18n_raw_terms = true; // (the msgids, not the screen's "Graph")
    FILE* f = fopen(path, "wb");
    if (!f) return printf("[RealEngine] i18n dump: can't write %s\n", path), 1;
    for (uint32_t k = 0; k < bp_node_kind_count(); k++) {
        const BpNodeDesc* d = bp_node_desc(k);
        if (!d || !d->name) continue;
        line(f, "node", d->name, strlen(d->name));
        if (d->category) parts(f, "category", d->category);
        if (d->tooltip && d->tooltip[0]) line(f, "node_tip", d->tooltip, strlen(d->tooltip));
        const char* spec = bp_node_pin_spec(k); // "i:type:Name" or "i:type:ref:Name": the pin's name after the last colon
        for (const char* c = spec; *c;) {
            const char* comma = strchr(c, ',');
            size_t l = comma ? (size_t)(comma - c) : strlen(c);
            const char* name = c;
            for (const char* q = c; q < c + l; q++)
                if (*q == ':') name = q + 1;
            if (name > c) line(f, "pin", name, (size_t)(c + l - name));
            c = comma ? comma + 1 : c + l;
        }
    }
    for (int i = 0; i < bp_builtin_enum_count(); i++) { // (the engine's enums' entries: the pins' drop-downs)
        int ne = 0;
        const char* const* en = bp_builtin_enum(bp_builtin_enum_name(i), &ne);
        for (int k = 0; en && k < ne; k++) line(f, "enum", en[k], strlen(en[k]));
    }
    for (int c = 0; c < COMP_COUNT; c++)
        if (g_components[c].name) line(f, "component", g_components[c].name, strlen(g_components[c].name));
    for (int k = 0; k < ASSET_KIND_COUNT; k++) line(f, "asset_kind", asset_kind_name((AssetKind)k), strlen(asset_kind_name((AssetKind)k)));
    for (int i = 0; i < editor_place_count(); i++) {
        const char* cat = NULL;
        const char* nm = editor_place_name(i, &cat);
        if (nm) line(f, "place", nm, strlen(nm));
        if (cat) parts(f, "place_category", cat);
    }
    // (the Material Editor's nodes: their English names and the Tlender's Portuguese labels, sockets, options)
    const MatNodeDef* defs[256];
    int nd = mat_node_defs(defs, 256);
    for (int i = 0; i < nd; i++) {
        const MatNodeDef* d = defs[i];
        fprintf(f, "mat_node\t%s\t%s\n", d->name, d->label ? d->label : "");
        for (int k = 0; k < d->nin; k++) fprintf(f, "mat_sock\t%s\t%s\n", d->in[k].name, d->in[k].label ? d->in[k].label : "");
        for (int k = 0; k < d->nout; k++) fprintf(f, "mat_sock\t%s\t%s\n", d->out[k].name, d->out[k].label ? d->out[k].label : "");
        for (int k = 0; k < d->nprops; k++) {
            fprintf(f, "mat_prop\t%s\n", d->props[k].label ? d->props[k].label : "");
            for (int j = 0; d->props[k].items && j < d->props[k].count; j++) fprintf(f, "mat_item\t%s\n", d->props[k].items[j]);
        }
    }
    for (int c = 0; c < MCAT_COUNT; c++) fprintf(f, "mat_cat\t%s\n", mat_category_name(c));
    fclose(f);
    printf("[RealEngine] i18n dump: %s\n", path);
    return 0;
}
