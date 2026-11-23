// --- (Q19, notas-dev/importar-tlender.md) The Tlender's drivers at the import (included by tlend_import.c before the
// rig's evaluation): the "rig" block's custom properties ("pr") and drivers ("dr", "drb", "drp", "dre", the curve's
// keys, "dv" variables, "drg" Generator), the expressions compiled once (driver_expr.c, the Tlender's rig_expr.c), then
// evaluated as the Tlender does on every frame the import bakes: a bone's channels and a constraint's influence (so the
// baked animations carry them), a shape key's value (a morph curve of the animation), a custom property (read by the
// others). A driver whose inputs never change is an expression evaluated once (its value kept in the asset); one that
// changes is baked into the animations' curves; the rest is lost with its reason. ---

enum { DRV_AVERAGE = 0, DRV_SUM, DRV_MIN, DRV_MAX, DRV_EXPR };
enum { DV_SINGLE = 0, DV_TRANSFORM, DV_ROT_DIFF, DV_DISTANCE, DV_CONTEXT };
enum { DS_WORLD = 0, DS_TRANSFORM, DS_LOCAL, DS_POSE };
enum { CH_PROP_ = 10 };

typedef struct TDTarget { int obj; char bone[64]; int channel; char prop[192]; int transform, space; } TDTarget;
typedef struct TDVar { char name[64]; int type, rot_mode; TDTarget t[2]; } TDVar;
typedef struct TDriver {
    int object, channel, type, enabled, use_curve, nvars, pre, post, gen_order, gen_fact, gen_add, use_self, fmods;
    char bone[64], prop[192], expr[256];
    TDVar vars[16];
    TCurve curve; // x: the driver's value, y: the result
    float gen_coef[8];
    DriverExpr code;
    bool code_ok;
    char err[160];
    // what it drives: a bone's channel (arm, bone), a constraint's influence (arm, bone, con), a shape key (mesh, key),
    // a custom property (prop), an object's channel (obj)
    int kind, arm, bone_i, con_i, mesh, key, prop_i, obj;
    float value, vmin, vmax;
    int evals;
    bool posed; // a variable reads the posed rig: evaluated again after it
} TDriver;
enum { DK_NONE = 0, DK_BONE, DK_CON, DK_KEY, DK_PROP, DK_OBJ };
typedef struct TProp { int obj, type; char bone[64], name[128]; float value, def, min, max; } TProp;
typedef struct TDrivers {
    TDriver* d;
    int n;
    TProp* p;
    int np;
    float* key_value; // per (mesh, key): this frame's driven value (NaN: not driven)
    int* key_base;    // per mesh: its first key's index in key_value
} TDrivers;

static const char* drv_tail(const char* l, size_t tag) { return strlen(l) > tag ? l + tag + 1 : ""; }

// the "rig" block's properties and drivers (the bone constraints' extra lines are read by tlend_import)
static void drivers_parse(TFile* f) {
    // the scene's block: "rig <properties> <drivers> <object constraints> <bone constraints>" (the bones' own "rig" lines
    // in the armatures have 15 numbers)
    size_t at = f->nlines;
    int np = 0, nd = 0, no = 0, nb = 0;
    for (size_t i = find(f, 0, "rig"); i < f->nlines; i = find(f, i + 1, "rig")) {
        char extra[8];
        if (sscanf(f->lines[i] + 4, "%d %d %d %d %7s", &np, &nd, &no, &nb, extra) == 4) {
            at = i;
            break;
        }
    }
    if (at >= f->nlines || (np <= 0 && nd <= 0)) return;
    TDrivers* D = calloc(1, sizeof *D);
    if (!D) return;
    f->drv = D;
    D->p = calloc((size_t)(np > 0 ? np : 1), sizeof(TProp));
    D->d = calloc((size_t)(nd > 0 ? nd : 1), sizeof(TDriver));
    if (!D->p || !D->d) return;
    size_t i = at + 1;
    for (; i < f->nlines && !starts(f->lines[i], "rigend") && !starts(f->lines[i], "oc") && !starts(f->lines[i], "cb"); i++) {
        const char* l = f->lines[i];
        if (starts(l, "pr") && D->np < np) {
            TProp* p = &D->p[D->np++];
            sscanf(l + 3, "%d %d %f %f %f %f", &p->obj, &p->type, &p->value, &p->def, &p->min, &p->max);
        } else if (starts(l, "prb") && D->np) snprintf(D->p[D->np - 1].bone, 64, "%s", drv_tail(l, 3));
        else if (starts(l, "prn") && D->np) snprintf(D->p[D->np - 1].name, 128, "%s", drv_tail(l, 3));
        else if (starts(l, "dr") && D->n < nd) {
            TDriver* d = &D->d[D->n++];
            int nk = 0;
            sscanf(l + 3, "%d %d %d %d %d %d %d %d %d", &d->object, &d->channel, &d->type, &d->enabled, &d->use_curve, &d->nvars, &nk, &d->pre, &d->post);
            if (d->nvars > 16) d->nvars = 16;
            if (d->nvars < 0) d->nvars = 0;
            d->curve.keys = nk > 0 ? calloc((size_t)nk, sizeof(TKey)) : NULL;
            d->kind = DK_NONE, d->arm = d->bone_i = d->con_i = d->mesh = d->key = d->prop_i = d->obj = -1;
            d->vmin = 1e30f, d->vmax = -1e30f;
            int v = -1, tb = 0, tp = 0;
            for (size_t j = i + 1; j < f->nlines; j++) {
                const char* q = f->lines[j];
                if (starts(q, "dr") || starts(q, "oc") || starts(q, "cb") || starts(q, "rigend")) break;
                if (starts(q, "drb")) snprintf(d->bone, 64, "%s", drv_tail(q, 3));
                else if (starts(q, "drp")) snprintf(d->prop, 192, "%s", drv_tail(q, 3));
                else if (starts(q, "dre")) snprintf(d->expr, 256, "%s", drv_tail(q, 3));
                else if (starts(q, "k") && d->curve.keys && d->curve.nkeys < nk) {
                    TKey* k = &d->curve.keys[d->curve.nkeys++];
                    sscanf(q + 2, "%f %f %f %f %f %f %d", &k->f, &k->v, &k->lx, &k->ly, &k->rx, &k->ry, &k->interp);
                } else if (starts(q, "dv") && v + 1 < d->nvars) {
                    TDVar* dv = &d->vars[++v];
                    sscanf(q + 3, "%d %d %d %d %d %d %d %d %d %d", &dv->type, &dv->rot_mode, &dv->t[0].obj, &dv->t[0].channel, &dv->t[0].transform,
                           &dv->t[0].space, &dv->t[1].obj, &dv->t[1].channel, &dv->t[1].transform, &dv->t[1].space);
                    tb = tp = 0;
                } else if (starts(q, "dvn") && v >= 0) snprintf(d->vars[v].name, 64, "%s", drv_tail(q, 3));
                else if (starts(q, "dvb") && v >= 0 && tb < 2) snprintf(d->vars[v].t[tb++].bone, 64, "%s", drv_tail(q, 3));
                else if (starts(q, "dvp") && v >= 0 && tp < 2) snprintf(d->vars[v].t[tp++].prop, 192, "%s", drv_tail(q, 3));
                else if (starts(q, "drg")) {
                    char* p = (char*)q + 4;
                    d->gen_order = (int)strtol(p, &p, 10), d->gen_fact = (int)strtol(p, &p, 10), d->gen_add = (int)strtol(p, &p, 10);
                    for (int c = 0; c < 8; c++) d->gen_coef[c] = strtof(p, &p);
                } else if (starts(q, "drx")) sscanf(q + 4, "%d %d", &d->use_self, &d->fmods);
                i = j;
            }
        }
    }
}

// a custom property's index by its owner (object, bone) and name ("name", "[\"name\"]" or a path ending in ["name"])
static int drv_prop(const TDrivers* D, int obj, const char* bone, const char* path) {
    char nm[192];
    const char* b = strrchr(path, '[');
    if (b && b[1] == '"') {
        snprintf(nm, sizeof nm, "%s", b + 2);
        char* e = strchr(nm, '"');
        if (e) *e = 0;
    } else snprintf(nm, sizeof nm, "%s", path);
    for (int i = 0; i < D->np; i++)
        if (D->p[i].obj == obj && !strcmp(D->p[i].name, nm) && (!bone || !strcmp(D->p[i].bone, bone))) return i;
    for (int i = 0; i < D->np; i++) // (the owner's bone unknown: by its name)
        if (D->p[i].obj == obj && !strcmp(D->p[i].name, nm)) return i;
    return -1;
}

// a quoted name in a path (key_blocks["Smile"].value, constraints["IK"].influence): into out
static bool drv_quoted(const char* path, const char* tag, char* out, size_t cap) {
    const char* p = strstr(path, tag);
    if (!p) return false;
    p += strlen(tag);
    const char* e = strchr(p, '"');
    if (!e) return false;
    snprintf(out, cap, "%.*s", (int)(e - p), p);
    return true;
}

static void drivers_resolve(TFile* f) {
    TDrivers* D = f->drv;
    if (!D) return;
    int nkeys = 0;
    D->key_base = calloc((size_t)(f->nmeshes ? f->nmeshes : 1), sizeof(int));
    for (int m = 0; D->key_base && m < f->nmeshes; m++) D->key_base[m] = nkeys, nkeys += (int)f->meshes[m].nkeys;
    D->key_value = calloc((size_t)(nkeys ? nkeys : 1), sizeof(float));
    for (int k = 0; D->key_value && k < nkeys; k++) D->key_value[k] = NAN;
    for (int i = 0; i < D->n; i++) {
        TDriver* d = &D->d[i];
        // its expression
        if (d->type == DRV_EXPR) {
            const char* names[16];
            for (int v = 0; v < d->nvars; v++) names[v] = d->vars[v].name;
            d->code_ok = driver_expr_compile(d->expr, names, d->nvars, &d->code, d->err, sizeof d->err);
        } else d->code_ok = true;
        for (int v = 0; v < d->nvars; v++) {
            int t = d->vars[v].type;
            d->posed |= t == DV_ROT_DIFF || t == DV_DISTANCE || (t == DV_TRANSFORM && d->vars[v].t[0].space != DS_TRANSFORM);
        }
        // what it drives
        if (d->object < 0 || d->object >= f->nobjs) continue;
        const TObj* o = &f->objs[d->object];
        if (o->type == 3 && o->arm >= 0 && o->arm < f->narms && d->bone[0]) {
            const TArm* a = &f->arms[o->arm];
            int b = bone_index(a, d->bone);
            if (b < 0) continue;
            if (d->channel < CH_PROP_) d->kind = DK_BONE, d->arm = o->arm, d->bone_i = b;
            else {
                char cn[64];
                int ci = -1;
                if (!strncmp(d->prop, "con", 3) && d->prop[3] >= '0' && d->prop[3] <= '9') ci = atoi(d->prop + 3);
                else if (drv_quoted(d->prop, "constraints[\"", cn, sizeof cn) && strstr(d->prop, "influence"))
                    for (int k = 0; k < a->bones[b].ncons; k++)
                        if (!strcmp(a->bones[b].cons[k].name, cn)) ci = k;
                if (ci >= 0 && ci < a->bones[b].ncons) d->kind = DK_CON, d->arm = o->arm, d->bone_i = b, d->con_i = ci;
                else if ((d->prop_i = drv_prop(D, d->object, d->bone, d->prop)) >= 0) d->kind = DK_PROP;
            }
        } else if (d->channel == CH_PROP_) {
            char kn[64];
            if (o->type == 0 && o->mesh >= 0 && o->mesh < f->nmeshes && drv_quoted(d->prop, "key_blocks[\"", kn, sizeof kn)) {
                const TMesh* me = &f->meshes[o->mesh];
                for (uint32_t k = 0; k < me->nkeys; k++)
                    if (!strcmp(me->kname[k], kn)) d->kind = DK_KEY, d->mesh = o->mesh, d->key = (int)k;
            } else if ((d->prop_i = drv_prop(D, d->object, NULL, d->prop)) >= 0) d->kind = DK_PROP;
        } else if (d->channel >= 0 && d->channel < CH_PROP_)
            d->kind = DK_OBJ, d->obj = d->object;
    }
}

// a bone's channel as keyed at this frame (the action's curve, else the file's pose): Tlender bones keep a quaternion
static float drv_bone_channel(const RigCtx* x, const TArm* a, int bi, int ch) {
    const TBone* b = &a->bones[bi];
    float v = ch <= 2 ? b->ploc[ch] : ch <= 5 ? b->pquat[ch - 2] : ch <= 8 ? b->pscale[ch - 6] : ch == 9 ? b->pquat[0] : 0.0f;
    if (x && x->act && a->curve_bone)
        for (int c = 0; c < x->act->ncurves; c++)
            if (a->curve_bone[c] == bi && x->act->curves[c].channel == ch) v = curve_eval(&x->act->curves[c], x->frame);
    return v;
}

// a target's matrix in a space (Blender's: world, the armature's pose, local to the parent's rest)
static M4 drv_target_matrix(TFile* f, const TDTarget* t, int space, bool* ok) {
    *ok = false;
    if (t->obj < 0 || t->obj >= f->nobjs) return m4_id();
    const TObj* o = &f->objs[t->obj];
    M4 ow = obj_world(f, t->obj, 0);
    if (o->type == 3 && t->bone[0] && o->arm >= 0 && o->arm < f->narms) {
        const TArm* a = &f->arms[o->arm];
        int b = bone_index(a, t->bone);
        if (b < 0 || !a->pose) return m4_id();
        *ok = true;
        if (space == DS_POSE) return a->pose[b];
        if (space == DS_LOCAL) return pose_to_local(a, b, &a->pose[b]);
        return m4_mul(&a->world, &a->pose[b]);
    }
    *ok = true;
    return ow;
}

static float drv_transform(const M4* m, int tr, int rot_mode) {
    float loc[3], sz[3];
    M4 rot;
    m4_decomp(m, loc, &rot, sz);
    if (tr <= 2) return loc[tr];
    if (tr >= 7 && tr <= 9) return sz[tr - 7];
    if (tr == 10) return (sz[0] + sz[1] + sz[2]) / 3.0f;
    if (rot_mode == 1) { // quaternion: w, x, y, z
        float q[4];
        quat_of(&rot, q);
        return tr == 6 ? q[0] : q[tr - 2];
    }
    float e[3];
    euler_of(&rot, e);
    return tr <= 5 ? e[tr - 3] : 0.0f;
}

// one driver's value this frame (false: it can't be evaluated: Blender leaves the channel as it was)
static bool drv_eval_one(TFile* f, TDrivers* D, TDriver* d, const RigCtx* x, float frame) {
    if (!d->enabled || !d->code_ok) return false;
    float vals[16] = { 0 };
    for (int v = 0; v < d->nvars; v++) {
        const TDVar* dv = &d->vars[v];
        const TDTarget* t = &dv->t[0];
        float val = 0.0f;
        if (dv->type == DV_SINGLE) {
            const TObj* o = t->obj >= 0 && t->obj < f->nobjs ? &f->objs[t->obj] : NULL;
            if (o && t->channel == CH_PROP_) {
                char kn[64];
                int pi = drv_prop(D, t->obj, t->bone[0] ? t->bone : NULL, t->prop);
                if (pi >= 0) val = D->p[pi].value;
                else if (o->type == 0 && o->mesh >= 0 && o->mesh < f->nmeshes && drv_quoted(t->prop, "key_blocks[\"", kn, sizeof kn)) {
                    const TMesh* me = &f->meshes[o->mesh];
                    for (uint32_t k = 0; k < me->nkeys; k++)
                        if (!strcmp(me->kname[k], kn)) val = me->kvalue[k];
                }
            } else if (o && o->type == 3 && t->bone[0] && o->arm >= 0 && o->arm < f->narms) {
                int bi = bone_index(&f->arms[o->arm], t->bone);
                if (bi >= 0) val = drv_bone_channel(x, &f->arms[o->arm], bi, t->channel);
            } else if (o) val = t->channel <= 2 ? o->loc[t->channel] : t->channel <= 5 ? o->rot[t->channel - 3] : t->channel <= 8 ? o->scale[t->channel - 6] : 0.0f;
        } else if (dv->type == DV_TRANSFORM) {
            const TObj* o = t->obj >= 0 && t->obj < f->nobjs ? &f->objs[t->obj] : NULL;
            if (o && t->space == DS_TRANSFORM) { // the channels as keyed
                if (o->type == 3 && t->bone[0] && o->arm >= 0 && o->arm < f->narms) {
                    const TArm* a = &f->arms[o->arm];
                    int bi = bone_index(a, t->bone);
                    if (bi >= 0) {
                        float loc[3], q[4], s[3];
                        for (int k = 0; k < 3; k++) loc[k] = drv_bone_channel(x, a, bi, k), s[k] = drv_bone_channel(x, a, bi, 6 + k);
                        q[0] = drv_bone_channel(x, a, bi, 9);
                        for (int k = 0; k < 3; k++) q[k + 1] = drv_bone_channel(x, a, bi, 3 + k);
                        M4 m = chan_matrix(loc, q, s);
                        val = drv_transform(&m, t->transform, dv->rot_mode);
                    }
                } else {
                    float e[3] = { o->rot[0], o->rot[1], o->rot[2] };
                    val = t->transform <= 2 ? o->loc[t->transform] : t->transform <= 5 ? e[t->transform - 3] : t->transform <= 9 && t->transform >= 7 ? o->scale[t->transform - 7]
                          : t->transform == 10 ? (o->scale[0] + o->scale[1] + o->scale[2]) / 3.0f : 1.0f;
                }
            } else {
                bool ok;
                M4 m = drv_target_matrix(f, t, t->space, &ok);
                if (ok) val = drv_transform(&m, t->transform, dv->rot_mode);
            }
        } else if (dv->type == DV_ROT_DIFF || dv->type == DV_DISTANCE) {
            bool ok0, ok1;
            M4 a = drv_target_matrix(f, &dv->t[0], DS_WORLD, &ok0), b = drv_target_matrix(f, &dv->t[1], DS_WORLD, &ok1);
            if (ok0 && ok1) {
                if (dv->type == DV_DISTANCE) {
                    float d3[3] = { a.m[12] - b.m[12], a.m[13] - b.m[13], a.m[14] - b.m[14] };
                    val = v3len(d3);
                } else {
                    float qa[4], qb[4];
                    M4 ra = rot_of(&a), rb = rot_of(&b);
                    quat_of(&ra, qa), quat_of(&rb, qb);
                    float dt = fabsf(qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]);
                    val = 2.0f * acosf(fminf(dt, 1.0f));
                }
            }
        }
        vals[v] = val;
    }
    float raw = 0.0f;
    if (d->type == DRV_AVERAGE || d->type == DRV_SUM) {
        for (int v = 0; v < d->nvars; v++) raw += vals[v];
        if (d->type == DRV_AVERAGE && d->nvars) raw /= (float)d->nvars;
    } else if (d->type == DRV_MIN || d->type == DRV_MAX) {
        raw = d->nvars ? vals[0] : 0.0f;
        for (int v = 1; v < d->nvars; v++) raw = d->type == DRV_MIN ? fminf(raw, vals[v]) : fmaxf(raw, vals[v]);
    } else {
        bool ok = true;
        raw = driver_expr_eval(&d->code, vals, frame, &ok);
        if (!ok) return false;
    }
    float value = raw;
    if (d->gen_order > 0) { // Blender's Generator: expanded c0 + c1 x + ..., or factorised (c0 x + c1)(c2 x + c3)...
        float g = 0.0f;
        if (d->gen_fact) {
            g = 1.0f;
            for (int k = 0; k < d->gen_order && k < 4; k++) g *= d->gen_coef[k * 2] * raw + d->gen_coef[k * 2 + 1];
        } else {
            float pw = 1.0f;
            for (int k = 0; k <= d->gen_order && k < 8; k++) g += d->gen_coef[k] * pw, pw *= raw;
        }
        value = d->gen_add ? value + g : g;
    }
    if (d->use_curve && d->curve.nkeys) value = curve_eval(&d->curve, value);
    if (d->kind == DK_CON) value = fminf(fmaxf(value, 0.0f), 1.0f);
    else if (d->kind == DK_KEY && f->meshes[d->mesh].krange) // (the Tlender holds a shape key's value in its slider's range)
        value = fminf(fmaxf(value, f->meshes[d->mesh].krange[d->key][0]), f->meshes[d->mesh].krange[d->key][1]);
    d->value = value, d->evals++;
    d->vmin = fminf(d->vmin, value), d->vmax = fmaxf(d->vmax, value);
    return true;
}

// Every driver at a frame (the action act, if any, keys the bones): the properties first (others read them), then the
// rest; their results into the rig (constraints' influences) and the shape keys' values; the bones' channels are read
// by bone_channels (drv_bone_override)
static void drivers_eval(TFile* f, const TAction* act, float frame, bool posed_pass) {
    TDrivers* D = f->drv;
    if (!D || !D->n) return;
    RigCtx x = { f, act, frame };
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < D->n; i++) {
            TDriver* d = &D->d[i];
            if (posed_pass && !d->posed) continue;
            if ((pass == 0) != (d->kind == DK_PROP)) continue;
            if (!drv_eval_one(f, D, d, &x, frame)) continue;
            if (d->kind == DK_PROP) D->p[d->prop_i].value = d->value;
            else if (d->kind == DK_CON) f->arms[d->arm].bones[d->bone_i].cons[d->con_i].influence = d->value;
            else if (d->kind == DK_KEY && D->key_value && D->key_base) D->key_value[D->key_base[d->mesh] + d->key] = d->value;
        }
}

// The driven channels of bone i of armature ai over its keyed ones (bone_channels): loc 0-2, quaternion x y z 3-5 / w 9, scale 6-8
static void drv_bone_override(const TFile* f, int ai, int bi, float loc[3], float q[4], float s[3]) {
    const TDrivers* D = f->drv;
    if (!D) return;
    for (int i = 0; i < D->n; i++) {
        const TDriver* d = &D->d[i];
        if (d->kind != DK_BONE || d->arm != ai || d->bone_i != bi || !d->evals) continue;
        int ch = d->channel;
        if (ch <= 2) loc[ch] = d->value;
        else if (ch <= 5) q[ch - 2] = d->value;
        else if (ch <= 8) s[ch - 6] = d->value;
        else if (ch == 9) q[0] = d->value;
    }
}

// At the file's frame: the drivers once, their constant results kept (objects' channels, shape keys' values: the defaults)
static void drivers_static(TFile* f) {
    TDrivers* D = f->drv;
    if (!D) return;
    drivers_eval(f, NULL, 0.0f, false);
    for (int i = 0; i < D->n; i++) {
        TDriver* d = &D->d[i];
        if (!d->evals) continue;
        if (d->kind == DK_OBJ && d->obj >= 0) {
            TObj* o = &f->objs[d->obj];
            if (d->channel <= 2) o->loc[d->channel] = d->value;
            else if (d->channel <= 5) o->rot[d->channel - 3] = d->value;
            else if (d->channel <= 8) o->scale[d->channel - 6] = d->value;
            o->world_done = false;
        } else if (d->kind == DK_KEY) f->meshes[d->mesh].kvalue[d->key] = d->value;
    }
}

// The conversion report: what each driver became
static void drivers_report(const TFile* f) {
    const TDrivers* D = f->drv;
    if (!D) return;
    for (int i = 0; i < D->n; i++) {
        const TDriver* d = &D->d[i];
        char item[200], owner[64];
        snprintf(owner, sizeof owner, "%s", d->object >= 0 && d->object < f->nobjs ? f->objs[d->object].name : d->object == -2 ? "Scene" : "?");
        snprintf(item, sizeof item, "%s%s%s", d->bone, d->bone[0] ? " " : "", d->channel == CH_PROP_ ? d->prop : (const char*[]){ "loc X", "loc Y", "loc Z", "rot X", "rot Y", "rot Z", "scale X", "scale Y", "scale Z", "rot W" }[d->channel >= 0 && d->channel < 10 ? d->channel : 0]);
        if (!d->enabled) imprep_add(IMPREP_SAME, IMPREP_CAT_DRIVER, owner, item, "%s", TR("off in the Tlender: not evaluated"));
        else if (!d->code_ok) imprep_add(IMPREP_LOST, IMPREP_CAT_DRIVER, owner, item, TR("its expression doesn't compile: %s"), d->err);
        else if (d->object == -2) imprep_add(IMPREP_LOST, IMPREP_CAT_DRIVER, owner, item, "%s", TR("drives a property of the scene (render, world): the Engine's are set in its own panels"));
        else if (d->kind == DK_NONE && strstr(d->prop, "key_blocks"))
            imprep_add(IMPREP_LOST, IMPREP_CAT_DRIVER, owner, item, "%s", TR("drives a shape key that is no morph target here (the Basis, an Absolute key, or a mesh that is not drawn)"));
        else if (d->kind == DK_NONE) imprep_add(IMPREP_LOST, IMPREP_CAT_DRIVER, owner, item, "%s", TR("drives a property the Engine doesn't have (only bones, constraints' influences, shape keys, custom properties and objects' transforms)"));
        else if (d->use_self || d->fmods) imprep_add(IMPREP_CONVERTED, IMPREP_CAT_DRIVER, owner, item, "%s", TR("evaluated without its Use Self / F-modifiers (the Generator is kept)"));
        else if (!d->evals) imprep_add(IMPREP_LOST, IMPREP_CAT_DRIVER, owner, item, "%s", TR("its value could not be evaluated (a division by zero, a missing target)"));
        else if (d->vmax - d->vmin <= 1e-6f) imprep_add(IMPREP_CONVERTED, IMPREP_CAT_DRIVER, owner, item, TR("an expression evaluated: the constant %g kept in the asset"), (double)d->value);
        else imprep_add(IMPREP_BAKED, IMPREP_CAT_DRIVER, owner, item, TR("baked into the animations (from %g to %g): the bones' poses and the morph curves"), (double)d->vmin, (double)d->vmax);
    }
}

static void drivers_free(TFile* f) {
    TDrivers* D = f->drv;
    if (!D) return;
    for (int i = 0; i < D->n; i++) free(D->d[i].curve.keys);
    free(D->d), free(D->p), free(D->key_value), free(D->key_base), free(D);
    f->drv = NULL;
}
