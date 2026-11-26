// Character definition (charmap.h): the roles' tables, the map's bookkeeping, the .tlmap file and the .tlend lines.
#include "charmap.h"

#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "io_json.h"
#include "platform.h"
#include "rig_armature_math.h"

// --- The switch ---------------------------------------------------------------------------------------------------------

static bool s_posemotion_on;
void posemotion_set_enabled(bool on) { s_posemotion_on = on; }
bool posemotion_enabled(void) { return s_posemotion_on; }

// --- Roles --------------------------------------------------------------------------------------------------------------

static const char* const CENTER_CODE[CR_SIDE_FIRST] = { "",      "root",  "hips",  "spine1", "spine2", "spine3", "spine4",
                                                        "chest", "neck1", "neck2", "head",   "jaw",    "eye.L",  "eye.R" };
static const char* const CENTER_LABEL[CR_SIDE_FIRST] = { "",          N_("Root"),      N_("Hips"),   N_("Spine 1"), N_("Spine 2"),
                                                         N_("Spine 3"), N_("Spine 4"),  N_("Chest"),     N_("Neck 1"), N_("Neck 2"),
                                                         N_("Head"),   N_("Jaw"), N_("Eye"),      N_("Eye") };
static const int CENTER_PARENT[CR_SIDE_FIRST] = { CR_NONE,   CR_NONE,  CR_ROOT,  CR_HIPS,  CR_SPINE1, CR_SPINE2, CR_SPINE3,
                                                  CR_SPINE4, CR_CHEST, CR_NECK1, CR_NECK2, CR_HEAD,   CR_HEAD,   CR_HEAD };

#define TL_CHAR_CODE(id, code, label) code,
static const char* const SIDE_CODE[CR_SIDE_COUNT] = { TL_CHAR_SIDE_ROLES(TL_CHAR_CODE) };
#undef TL_CHAR_CODE
#define TL_CHAR_LABEL(id, code, label) N_(label),
static const char* const SIDE_LABEL[CR_SIDE_COUNT] = { TL_CHAR_SIDE_ROLES(TL_CHAR_LABEL) };
#undef TL_CHAR_LABEL

enum { SP_CHEST = -1, SP_HIPS = -2 };
// The parent of each side role: an index into the side block, or the chest / the hips.
static const int SIDE_PARENT[CR_SIDE_COUNT] = {
    SP_CHEST, // clavicle
    0,  1,  2, // upper arm, forearm, hand
    3,  4,  5, // thumb
    3,  7,  8, // index
    3,  10, 11, // middle
    3,  13, 14, // ring
    3,  16, 17, // pinky
    SP_HIPS, 19, 20, 21, // thigh, shin, foot, toe
    1,  2,  19, 20, // the twists hang from their limb bone
};

static char s_codes[CR_COUNT][24];

const char* charrole_code(int role) {
    if (role <= CR_NONE || role >= CR_COUNT) return "";
    if (role < CR_SIDE_FIRST) return CENTER_CODE[role];
    if (!s_codes[role][0]) {
        int k = (role - CR_SIDE_FIRST) % CR_SIDE_COUNT;
        snprintf(s_codes[role], sizeof s_codes[role], "%s.%c", SIDE_CODE[k], role - CR_SIDE_FIRST < CR_SIDE_COUNT ? 'L' : 'R');
    }
    return s_codes[role];
}

int charrole_from_code(const char* code) {
    if (!code || !code[0]) return CR_NONE;
    for (int r = CR_NONE + 1; r < CR_COUNT; r++)
        if (strcmp(charrole_code(r), code) == 0) return r;
    return CR_NONE;
}

int charrole_side(int role) {
    if (role == CR_EYE_L) return 1;
    if (role == CR_EYE_R) return 2;
    if (role < CR_SIDE_FIRST || role >= CR_COUNT) return 0;
    return role - CR_SIDE_FIRST < CR_SIDE_COUNT ? 1 : 2;
}

int charrole_mirror(int role) {
    if (role == CR_EYE_L) return CR_EYE_R;
    if (role == CR_EYE_R) return CR_EYE_L;
    if (role < CR_SIDE_FIRST || role >= CR_COUNT) return role;
    return role - CR_SIDE_FIRST < CR_SIDE_COUNT ? role + CR_SIDE_COUNT : role - CR_SIDE_COUNT;
}

int charrole_parent(int role) {
    if (role <= CR_NONE || role >= CR_COUNT) return CR_NONE;
    if (role < CR_SIDE_FIRST) return CENTER_PARENT[role];
    int base = role - CR_SIDE_FIRST < CR_SIDE_COUNT ? CR_SIDE_FIRST : CR_SIDE_FIRST + CR_SIDE_COUNT;
    int p = SIDE_PARENT[(role - CR_SIDE_FIRST) % CR_SIDE_COUNT];
    return p == SP_CHEST ? CR_CHEST : p == SP_HIPS ? CR_HIPS : base + p;
}

const char* charrole_label_base(int role) {
    if (role <= CR_NONE || role >= CR_COUNT) return "";
    return role < CR_SIDE_FIRST ? CENTER_LABEL[role] : SIDE_LABEL[(role - CR_SIDE_FIRST) % CR_SIDE_COUNT];
}

void charrole_label(int role, char* out, size_t cap) {
    int side = charrole_side(role);
    const char* base = TR(charrole_label_base(role));
    if (side) snprintf(out, cap, "%s %s", base, side == 1 ? TR("(L)") : TR("(R)"));
    else snprintf(out, cap, "%s", base);
}

static int side_index(int role) { return role < CR_SIDE_FIRST || role >= CR_COUNT ? -1 : (role - CR_SIDE_FIRST) % CR_SIDE_COUNT; }

bool charrole_main(int role) {
    if (role == CR_HIPS || role == CR_SPINE1 || role == CR_HEAD) return true;
    int k = side_index(role);
    return k == 1 || k == 2 || k == 3 || k == 19 || k == 20 || k == 21;
}

bool charrole_is_finger(int role) {
    int k = side_index(role);
    return k >= 4 && k <= 18;
}

bool charrole_is_twist(int role) {
    int k = side_index(role);
    return k >= 23;
}

static const char* const CLASS_CODE[CRC_COUNT] = { "unknown", "humanoid", "near_human", "quadruped", "non_human", "mechanical" };
static const char* const CLASS_NAME[CRC_COUNT] = { N_("Unknown"), N_("Humanoid"),  N_("Near Human"),
                                                   N_("Quadruped"),   N_("Non-human"), N_("Mechanical") };
const char* charmap_class_name(int c) { return c >= 0 && c < CRC_COUNT ? CLASS_NAME[c] : CLASS_NAME[0]; }
const char* charmap_class_code(int c) { return c >= 0 && c < CRC_COUNT ? CLASS_CODE[c] : CLASS_CODE[0]; }
int charmap_class_from_code(const char* code) {
    for (int i = 0; code && i < CRC_COUNT; i++)
        if (strcmp(CLASS_CODE[i], code) == 0) return i;
    return CRC_UNKNOWN;
}

static const char* const SOURCE_CODE[CSRC_COUNT] = { "none", "manual", "autorig", "names", "vrm", "topology" };
static const char* const SOURCE_NAME[CSRC_COUNT] = { N_("None (rig)"),         N_("Picked by Hand"),   N_("Tlender Auto-Rig"),
                                                     N_("Bone Names (rig)"), N_("VRM Humanoid Map"), N_("Skeleton Shape") };
const char* charmap_source_name(int s) { return s >= 0 && s < CSRC_COUNT ? SOURCE_NAME[s] : SOURCE_NAME[0]; }
static int source_from_code(const char* code) {
    for (int i = 0; code && i < CSRC_COUNT; i++)
        if (strcmp(SOURCE_CODE[i], code) == 0) return i;
    return CSRC_NONE;
}

static const char* const CHAIN_CODE[CCK_COUNT] = { "generic", "tail", "leg", "wing", "tentacle", "ear", "arm" };
static const char* const CHAIN_NAME[CCK_COUNT] = { N_("Chain"), N_("Tail"), N_("Leg"), N_("Wing"), N_("Tentacle"), N_("Ear"), N_("Arm") };
const char* charmap_chain_kind_name(int k) { return k >= 0 && k < CCK_COUNT ? CHAIN_NAME[k] : CHAIN_NAME[0]; }
const char* charmap_chain_kind_code(int k) { return k >= 0 && k < CCK_COUNT ? CHAIN_CODE[k] : CHAIN_CODE[0]; }
int charmap_chain_kind_from_code(const char* code) {
    for (int i = 0; code && i < CCK_COUNT; i++)
        if (strcmp(CHAIN_CODE[i], code) == 0) return i;
    return CCK_GENERIC;
}

static const char* const MASS_SOURCE_CODE[CMS_COUNT] = { "none", "preset", "mesh" };
static int mass_source_from_code(const char* code) {
    for (int i = 0; code && i < CMS_COUNT; i++)
        if (strcmp(MASS_SOURCE_CODE[i], code) == 0) return i;
    return CMS_NONE;
}
static const char* const REST_POSE_CODE[CRP_COUNT] = { "unknown", "T", "A", "other" };
static int rest_pose_from_code(const char* code) {
    for (int i = 0; code && i < CRP_COUNT; i++)
        if (strcmp(REST_POSE_CODE[i], code) == 0) return i;
    return CRP_UNKNOWN;
}

// --- Maps ---------------------------------------------------------------------------------------------------------------

CharMap* charmap_new(void) { return calloc(1, sizeof(CharMap)); }
void charmap_free(CharMap* m) { free(m); }

CharMap* charmap_copy(const CharMap* m) {
    if (!m) return NULL;
    CharMap* c = malloc(sizeof *c);
    if (c) *c = *m;
    return c;
}

void charmap_clear(CharMap* m) { memset(m, 0, sizeof *m); }

CharMap* armature_charmap_ensure(Armature* a) {
    if (!a->charmap) a->charmap = charmap_new();
    return a->charmap;
}

void armature_charmap_remove(Armature* a) {
    charmap_free(a->charmap);
    a->charmap = NULL;
}

void charmap_set_role(CharMap* m, int role, const char* bone, const char* control, float confidence, int source) {
    if (role <= CR_NONE || role >= CR_COUNT) return;
    CharRoleMap* r = &m->roles[role];
    if (!bone || !bone[0]) {
        memset(r, 0, sizeof *r);
        return;
    }
    for (int i = CR_NONE + 1; i < CR_COUNT; i++)
        if (i != role && strcmp(m->roles[i].bone, bone) == 0) memset(&m->roles[i], 0, sizeof m->roles[i]);
    memset(r, 0, sizeof *r);
    snprintf(r->bone, sizeof r->bone, "%s", bone);
    if (control && strcmp(control, bone) != 0) snprintf(r->control, sizeof r->control, "%s", control);
    r->confidence = confidence < 0.0f ? 0.0f : confidence > 1.0f ? 1.0f : confidence;
    r->source = (uint8_t)source;
}

int charmap_role_of_bone(const CharMap* m, const char* bone) {
    if (!m || !bone || !bone[0]) return CR_NONE;
    for (int i = CR_NONE + 1; i < CR_COUNT; i++)
        if (strcmp(m->roles[i].bone, bone) == 0) return i;
    for (int i = CR_NONE + 1; i < CR_COUNT; i++)
        if (m->roles[i].control[0] && strcmp(m->roles[i].control, bone) == 0) return i;
    return CR_NONE;
}

int charmap_bone(const Armature* a, const CharMap* m, int role) {
    if (!a || !m || role <= CR_NONE || role >= CR_COUNT || !m->roles[role].bone[0]) return -1;
    return armature_find_bone(a, m->roles[role].bone);
}

int charmap_control(const Armature* a, const CharMap* m, int role) {
    if (!a || !m || role <= CR_NONE || role >= CR_COUNT) return -1;
    if (m->roles[role].control[0]) {
        int c = armature_find_bone(a, m->roles[role].control);
        if (c >= 0) return c;
    }
    return charmap_bone(a, m, role);
}

int charmap_mapped_count(const CharMap* m) {
    int n = 0;
    for (int i = CR_NONE + 1; m && i < CR_COUNT; i++) n += m->roles[i].bone[0] != 0;
    return n;
}

static bool has_spine(const CharMap* m) {
    for (int r = CR_SPINE1; r <= CR_CHEST; r++)
        if (m->roles[r].bone[0]) return true;
    return false;
}

int charmap_main_total(void) {
    int n = 0;
    for (int r = CR_NONE + 1; r < CR_COUNT; r++) n += charrole_main(r);
    return n;
}

int charmap_missing_main(const CharMap* m, int* out, int cap) {
    int n = 0;
    for (int r = CR_NONE + 1; r < CR_COUNT; r++) {
        if (!charrole_main(r)) continue;
        bool have = r == CR_SPINE1 ? has_spine(m) : m->roles[r].bone[0] != 0;
        if (have) continue;
        if (out && n < cap) out[n] = r;
        n++;
    }
    return n;
}

float charmap_confidence(const CharMap* m) {
    float sum = 0.0f;
    int n = 0;
    for (int r = CR_NONE + 1; r < CR_COUNT; r++) {
        if (!charrole_main(r) || !m->roles[r].bone[0]) continue;
        sum += m->roles[r].confidence;
        n++;
    }
    return n ? sum / (float)n : 0.0f;
}

int charmap_parent_role(const CharMap* m, int role) {
    for (int p = charrole_parent(role); p != CR_NONE; p = charrole_parent(p))
        if (m->roles[p].bone[0]) return p;
    return CR_NONE;
}

void charmap_update_class(CharMap* m) {
    if (m->rig_class == CRC_QUADRUPED || m->rig_class == CRC_NON_HUMAN || m->rig_class == CRC_MECHANICAL) return;
    if (charmap_missing_main(m, NULL, 0) == 0) {
        m->rig_class = CRC_HUMANOID;
        return;
    }
    bool legs = m->roles[CR_THIGH_L].bone[0] && m->roles[CR_THIGH_R].bone[0] && m->roles[CR_SHIN_L].bone[0] && m->roles[CR_SHIN_R].bone[0];
    if (legs && m->roles[CR_HIPS].bone[0] && has_spine(m)) m->rig_class = CRC_NEAR_HUMAN;
    else if (charmap_mapped_count(m) == 0 && m->chain_count > 0) m->rig_class = CRC_NON_HUMAN;
    else m->rig_class = CRC_UNKNOWN;
}

int charmap_chain_add(CharMap* m, const char* name, int kind, int side, const char* first, const char* last) {
    if (m->chain_count >= TL_CHARMAP_CHAINS || !first || !first[0]) return -1;
    CharChain* c = &m->chains[m->chain_count];
    memset(c, 0, sizeof *c);
    char base[TL_CHARMAP_CHAIN_NAME];
    snprintf(base, sizeof base, "%s", name && name[0] ? name : charmap_chain_kind_code(kind));
    for (char* p = base; *p; p++)
        if (*p == ' ' || *p == '\t') *p = '_'; // one word: the .tlend line splits on spaces
    snprintf(c->name, sizeof c->name, "%s", base);
    for (int n = 2; n < 1000; n++) {
        bool taken = false;
        for (int i = 0; i < m->chain_count; i++) taken = taken || strcmp(m->chains[i].name, c->name) == 0;
        if (!taken) break;
        snprintf(c->name, sizeof c->name, "%.24s_%d", base, n);
    }
    c->kind = (uint8_t)kind;
    c->side = (uint8_t)side;
    snprintf(c->first, sizeof c->first, "%s", first);
    snprintf(c->last, sizeof c->last, "%s", last && last[0] ? last : first);
    return m->chain_count++;
}

void charmap_chain_remove(CharMap* m, int chain) {
    if (chain < 0 || chain >= m->chain_count) return;
    memmove(&m->chains[chain], &m->chains[chain + 1], (size_t)(m->chain_count - chain - 1) * sizeof m->chains[0]);
    m->chain_count--;
}

int charmap_chain_bones(const Armature* a, const CharChain* c, int* out, int cap) {
    int first = armature_find_bone(a, c->first), last = armature_find_bone(a, c->last);
    if (first < 0 || last < 0) return 0;
    int n = 0;
    for (int b = last; b >= 0; b = a->bones[b].parent) {
        n++;
        if (b == first) break;
        if (a->bones[b].parent < 0) return 0; // first is not above last
    }
    int k = n;
    for (int b = last; k > 0; b = a->bones[b].parent) {
        k--;
        if (out && k < cap) out[k] = b;
    }
    return n;
}

void charmap_bone_renamed(CharMap* m, const char* from, const char* to) {
    if (!m || !from || !to) return;
    for (int i = CR_NONE + 1; i < CR_COUNT; i++) {
        if (strcmp(m->roles[i].bone, from) == 0) snprintf(m->roles[i].bone, sizeof m->roles[i].bone, "%s", to);
        if (strcmp(m->roles[i].control, from) == 0) snprintf(m->roles[i].control, sizeof m->roles[i].control, "%s", to);
    }
    for (int i = 0; i < m->chain_count; i++) {
        if (strcmp(m->chains[i].first, from) == 0) snprintf(m->chains[i].first, sizeof m->chains[i].first, "%s", to);
        if (strcmp(m->chains[i].last, from) == 0) snprintf(m->chains[i].last, sizeof m->chains[i].last, "%s", to);
    }
}

int charmap_validate(CharMap* m, const Armature* a) {
    int dropped = 0;
    for (int i = CR_NONE + 1; i < CR_COUNT; i++) {
        CharRoleMap* r = &m->roles[i];
        if (r->bone[0] && armature_find_bone(a, r->bone) < 0) {
            memset(r, 0, sizeof *r);
            dropped++;
        } else if (r->control[0] && armature_find_bone(a, r->control) < 0) r->control[0] = 0;
    }
    for (int i = m->chain_count - 1; i >= 0; i--)
        if (charmap_chain_bones(a, &m->chains[i], NULL, 0) == 0) {
            charmap_chain_remove(m, i);
            dropped++;
        }
    return dropped;
}

// --- The .tlmap file ----------------------------------------------------------------------------------------------------

char* charmap_to_json(const CharMap* m, size_t* len) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "tlmap", 1);
    jw_kstr(&w, "classe", charmap_class_code(m->rig_class));
    jw_kstr(&w, "origem", SOURCE_CODE[m->source < CSRC_COUNT ? m->source : 0]);
    jw_kstr(&w, "convencao", m->convention);
    jw_key(&w, "papeis");
    jw_obj_begin(&w);
    for (int r = CR_NONE + 1; r < CR_COUNT; r++) {
        const CharRoleMap* rm = &m->roles[r];
        if (!rm->bone[0]) continue;
        jw_key(&w, charrole_code(r));
        jw_obj_begin(&w);
        jw_kstr(&w, "osso", rm->bone);
        if (rm->control[0]) jw_kstr(&w, "controle", rm->control);
        jw_kfloat(&w, "confianca", rm->confidence);
        jw_kstr(&w, "origem", SOURCE_CODE[rm->source < CSRC_COUNT ? rm->source : 0]);
        if (rm->has_ref) jw_kfloats(&w, "referencia", rm->ref_quat, 4);
        if (rm->has_limit) {
            float lim[6] = { rm->lim_min[0], rm->lim_max[0], rm->lim_min[1], rm->lim_max[1], rm->lim_min[2], rm->lim_max[2] };
            jw_kfloats(&w, "limites", lim, 6);
        }
        if (rm->mass > 0.0f) {
            jw_kfloat(&w, "massa", rm->mass);
            jw_kfloat(&w, "centro", rm->mass_center);
        }
        jw_obj_end(&w);
    }
    jw_obj_end(&w);
    if (m->rest_pose != CRP_UNKNOWN) {
        jw_key(&w, "pose");
        jw_obj_begin(&w);
        jw_kstr(&w, "descanso", REST_POSE_CODE[m->rest_pose < CRP_COUNT ? m->rest_pose : 0]);
        jw_kfloats(&w, "bracos", m->rest_arm_angle, 2);
        jw_obj_end(&w);
    }
    if (m->mass_source != CMS_NONE) jw_kstr(&w, "massas", MASS_SOURCE_CODE[m->mass_source < CMS_COUNT ? m->mass_source : 0]);
    jw_key(&w, "cadeias");
    jw_arr_begin(&w);
    for (int i = 0; i < m->chain_count; i++) {
        const CharChain* c = &m->chains[i];
        jw_obj_begin(&w);
        jw_kstr(&w, "nome", c->name);
        jw_kstr(&w, "tipo", charmap_chain_kind_code(c->kind));
        jw_kint(&w, "lado", c->side);
        jw_kstr(&w, "primeiro", c->first);
        jw_kstr(&w, "ultimo", c->last);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    if (w.fail || !w.buf) {
        jw_free(&w);
        return NULL;
    }
    if (len) *len = w.len;
    char* out = malloc(w.len + 1);
    if (out) {
        memcpy(out, w.buf, w.len);
        out[w.len] = 0;
    }
    jw_free(&w);
    return out;
}

bool charmap_from_json(CharMap* m, const char* text, size_t len, char* err, size_t err_cap) {
    JsonDoc doc;
    char jerr[160] = "";
    if (!json_parse(text, len, &doc, jerr, sizeof jerr)) {
        if (err) snprintf(err, err_cap, TR("Unreadable character map: %s"), jerr);
        return false;
    }
    const JsonValue* root = &doc.root;
    if (json_get_int(root, "tlmap", 0) < 1 || !json_get(root, "papeis")) {
        if (err) snprintf(err, err_cap, "%s", TR("The file isn't a character map (.tlmap)."));
        json_free(&doc);
        return false;
    }
    charmap_clear(m);
    m->rig_class = (uint8_t)charmap_class_from_code(json_get_str(root, "classe", ""));
    m->source = (uint8_t)source_from_code(json_get_str(root, "origem", ""));
    snprintf(m->convention, sizeof m->convention, "%s", json_get_str(root, "convencao", ""));
    const JsonValue* roles = json_get(root, "papeis");
    for (int r = CR_NONE + 1; r < CR_COUNT; r++) {
        const JsonValue* v = json_get(roles, charrole_code(r));
        const char* bone = v ? json_get_str(v, "osso", "") : "";
        if (!bone[0]) continue;
        charmap_set_role(m, r, bone, json_get_str(v, "controle", ""), (float)json_get_num(v, "confianca", 1.0),
                         source_from_code(json_get_str(v, "origem", "")));
        m->roles[r].has_ref = json_get_floats(v, "referencia", m->roles[r].ref_quat, 4) == 4;
        float lim[6];
        if (json_get_floats(v, "limites", lim, 6) == 6) {
            m->roles[r].has_limit = true;
            for (int k = 0; k < 3; k++) m->roles[r].lim_min[k] = lim[2 * k], m->roles[r].lim_max[k] = lim[2 * k + 1];
        }
        m->roles[r].mass = (float)json_get_num(v, "massa", 0.0);
        m->roles[r].mass_center = (float)json_get_num(v, "centro", 0.5);
    }
    m->mass_source = (uint8_t)mass_source_from_code(json_get_str(root, "massas", ""));
    const JsonValue* pose = json_get(root, "pose");
    if (pose) {
        m->rest_pose = (uint8_t)rest_pose_from_code(json_get_str(pose, "descanso", ""));
        json_get_floats(pose, "bracos", m->rest_arm_angle, 2);
    }
    const JsonValue* chains = json_get(root, "cadeias");
    for (size_t i = 0; i < json_len(chains); i++) {
        const JsonValue* c = json_at(chains, i);
        charmap_chain_add(m, json_get_str(c, "nome", ""), charmap_chain_kind_from_code(json_get_str(c, "tipo", "")), json_get_int(c, "lado", 0),
                          json_get_str(c, "primeiro", ""), json_get_str(c, "ultimo", ""));
    }
    json_free(&doc);
    return true;
}

bool charmap_save_file(const CharMap* m, const char* path, char* err, size_t err_cap) {
    size_t len = 0;
    char* text = charmap_to_json(m, &len);
    FILE* f = text ? tl_fopen(path, "wb") : NULL;
    bool ok = f && fwrite(text, 1, len, f) == len;
    if (f) ok = fclose(f) == 0 && ok;
    free(text);
    if (!ok && err) snprintf(err, err_cap, TR("Couldn't write %s"), path);
    return ok;
}

bool charmap_load_file(CharMap* m, const char* path, char* err, size_t err_cap) {
    FILE* f = tl_fopen(path, "rb");
    if (!f) {
        if (err) snprintf(err, err_cap, TR("Couldn't open %s"), path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* text = size >= 0 ? malloc((size_t)size + 1) : NULL;
    bool ok = text && fread(text, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) {
        if (err) snprintf(err, err_cap, TR("Couldn't open %s"), path);
        free(text);
        return false;
    }
    ok = charmap_from_json(m, text, (size_t)size, err, err_cap);
    free(text);
    return ok;
}

// --- The .tlend lines ---------------------------------------------------------------------------------------------------
// charmap <lines that follow> <class> <source> <convention or ->
// cmr <role> <confidence> <source> <bone>                 one per mapped role (the name runs to the end of the line)
// cmk <role> <control>                                    after the cmr of a role with a control
// cmq <role> <w> <x> <y> <z>                             the reference orientation of a role that has one (AM2)
// cmc <kind> <side> <name> <first>\t<last>                one per chain
// cmp <rest pose> <left arm angle> <right arm angle>      the rest pose, when it was measured (AM2)
// cml <role> <min max> x 3                                the joint limits of a role that has them (AM3)
// cmm <role> <mass> <center>                              the mass of a role that has one (AM3)
// cmw <mass source>                                       where the masses came from, when there are any (AM3)

void charmap_write_lines(FILE* f, const CharMap* m) {
    int lines = m->chain_count + (m->rest_pose != CRP_UNKNOWN ? 1 : 0) + (m->mass_source != CMS_NONE ? 1 : 0);
    for (int r = CR_NONE + 1; r < CR_COUNT; r++)
        if (m->roles[r].bone[0])
            lines += 1 + (m->roles[r].control[0] ? 1 : 0) + (m->roles[r].has_ref ? 1 : 0) + (m->roles[r].has_limit ? 1 : 0) + (m->roles[r].mass > 0.0f ? 1 : 0);
    fprintf(f, "charmap %d %s %s %s\n", lines, charmap_class_code(m->rig_class), SOURCE_CODE[m->source < CSRC_COUNT ? m->source : 0],
            m->convention[0] ? m->convention : "-");
    for (int r = CR_NONE + 1; r < CR_COUNT; r++) {
        const CharRoleMap* rm = &m->roles[r];
        if (!rm->bone[0]) continue;
        fprintf(f, "cmr %s %.9g %s %s\n", charrole_code(r), rm->confidence, SOURCE_CODE[rm->source < CSRC_COUNT ? rm->source : 0], rm->bone);
        if (rm->control[0]) fprintf(f, "cmk %s %s\n", charrole_code(r), rm->control);
        if (rm->has_ref) fprintf(f, "cmq %s %.9g %.9g %.9g %.9g\n", charrole_code(r), rm->ref_quat[0], rm->ref_quat[1], rm->ref_quat[2], rm->ref_quat[3]);
        if (rm->has_limit)
            fprintf(f, "cml %s %.9g %.9g %.9g %.9g %.9g %.9g\n", charrole_code(r), rm->lim_min[0], rm->lim_max[0], rm->lim_min[1], rm->lim_max[1],
                    rm->lim_min[2], rm->lim_max[2]);
        if (rm->mass > 0.0f) fprintf(f, "cmm %s %.9g %.9g\n", charrole_code(r), rm->mass, rm->mass_center);
    }
    if (m->mass_source != CMS_NONE) fprintf(f, "cmw %s\n", MASS_SOURCE_CODE[m->mass_source < CMS_COUNT ? m->mass_source : 0]);
    if (m->rest_pose != CRP_UNKNOWN)
        fprintf(f, "cmp %s %.9g %.9g\n", REST_POSE_CODE[m->rest_pose < CRP_COUNT ? m->rest_pose : 0], m->rest_arm_angle[0], m->rest_arm_angle[1]);
    for (int i = 0; i < m->chain_count; i++) {
        const CharChain* c = &m->chains[i];
        fprintf(f, "cmc %s %d %s %s\t%s\n", charmap_chain_kind_code(c->kind), c->side, c->name, c->first, c->last);
    }
}

bool charmap_parse_header(CharMap* m, const char* line, int* lines) {
    char cls[32] = "", src[32] = "", conv[32] = "";
    if (sscanf(line, "charmap %d %31s %31s %31s", lines, cls, src, conv) < 3 || *lines < 0) return false;
    charmap_clear(m);
    m->rig_class = (uint8_t)charmap_class_from_code(cls);
    m->source = (uint8_t)source_from_code(src);
    if (strcmp(conv, "-") != 0) snprintf(m->convention, sizeof m->convention, "%s", conv);
    return true;
}

static void rest_of_line(const char* s, char* out, size_t cap) {
    snprintf(out, cap, "%s", s);
    out[strcspn(out, "\r\n")] = 0;
}

bool charmap_parse_line(CharMap* m, const char* line) {
    char code[32] = "", src[32] = "", name[TL_BONE_NAME_LEN];
    int at = 0;
    if (strncmp(line, "cmr ", 4) == 0) {
        float conf = 0.0f;
        if (sscanf(line, "cmr %31s %f %31s %n", code, &conf, src, &at) < 3 || !at) return false;
        int role = charrole_from_code(code);
        rest_of_line(line + at, name, sizeof name);
        if (role != CR_NONE && name[0]) charmap_set_role(m, role, name, "", conf, source_from_code(src));
        return true; // a role this version does not know is skipped, not an error
    }
    if (strncmp(line, "cmk ", 4) == 0) {
        if (sscanf(line, "cmk %31s %n", code, &at) < 1 || !at) return false;
        int role = charrole_from_code(code);
        rest_of_line(line + at, name, sizeof name);
        if (role != CR_NONE && m->roles[role].bone[0]) snprintf(m->roles[role].control, sizeof m->roles[role].control, "%s", name);
        return true;
    }
    if (strncmp(line, "cmc ", 4) == 0) {
        char kind[32] = "", cname[TL_CHARMAP_CHAIN_NAME] = "", rest[2 * TL_BONE_NAME_LEN + 8];
        int side = 0;
        if (sscanf(line, "cmc %31s %d %31s %n", kind, &side, cname, &at) < 3 || !at) return false;
        rest_of_line(line + at, rest, sizeof rest);
        char* tab = strchr(rest, '\t');
        if (!tab) return false;
        *tab = 0;
        charmap_chain_add(m, cname, charmap_chain_kind_from_code(kind), side, rest, tab + 1);
        return true;
    }
    if (strncmp(line, "cmq ", 4) == 0) {
        float q[4];
        if (sscanf(line, "cmq %31s %f %f %f %f", code, &q[0], &q[1], &q[2], &q[3]) != 5) return false;
        int role = charrole_from_code(code);
        if (role != CR_NONE && m->roles[role].bone[0]) {
            memcpy(m->roles[role].ref_quat, q, sizeof q);
            m->roles[role].has_ref = true;
        }
        return true;
    }
    if (strncmp(line, "cmp ", 4) == 0) {
        char pose[32] = "";
        if (sscanf(line, "cmp %31s %f %f", pose, &m->rest_arm_angle[0], &m->rest_arm_angle[1]) != 3) return false;
        m->rest_pose = (uint8_t)rest_pose_from_code(pose);
        return true;
    }
    if (strncmp(line, "cml ", 4) == 0) {
        float v[6];
        if (sscanf(line, "cml %31s %f %f %f %f %f %f", code, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 7) return false;
        int role = charrole_from_code(code);
        if (role != CR_NONE && m->roles[role].bone[0]) {
            m->roles[role].has_limit = true;
            for (int k = 0; k < 3; k++) m->roles[role].lim_min[k] = v[2 * k], m->roles[role].lim_max[k] = v[2 * k + 1];
        }
        return true;
    }
    if (strncmp(line, "cmm ", 4) == 0) {
        float mass = 0.0f, center = 0.5f;
        if (sscanf(line, "cmm %31s %f %f", code, &mass, &center) != 3) return false;
        int role = charrole_from_code(code);
        if (role != CR_NONE && m->roles[role].bone[0]) {
            m->roles[role].mass = mass;
            m->roles[role].mass_center = center;
        }
        return true;
    }
    if (strncmp(line, "cmw ", 4) == 0) {
        if (sscanf(line, "cmw %31s", src) != 1) return false;
        m->mass_source = (uint8_t)mass_source_from_code(src);
        return true;
    }
    return strncmp(line, "cm", 2) == 0; // a kind of line of a newer version: skipped
}
