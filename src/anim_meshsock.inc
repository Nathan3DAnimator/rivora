// (2026-11-23, the user: "on meshes, add sockets") A Static Mesh's sockets (Unreal's Static Mesh sockets), included by
// anim.c: the same AnimSocket the Skeletal Meshes have (their bone 0: the mesh's own space), in a hidden file beside the
// mesh (".<file>.sockets", JSON), cached by path. anim_socket_world finds them on an actor with a Static Mesh and no
// skeleton (Get Socket Transform, attaching to a socket); the Static Mesh viewer edits them with the Skeletal Mesh's
// socket panel and gizmo.

#define MESH_SOCK_CACHE 64
static struct MeshSocks { char vpath[512]; uint32_t n; AnimSocket s[ANIM_MAX_SOCKETS]; bool loaded; } g_msock[MESH_SOCK_CACHE];
static uint32_t g_nmsock;

static void msock_disk(const char* vpath, char* out, size_t cap) { // "<root>/<dir>/.<file>.sockets"
    char disk[1100];
    const char* root = mat_root();
    if (!strncmp(vpath, "/Content", 8) && root && root[0]) snprintf(disk, sizeof disk, "%s%s", root, vpath + 8);
    else snprintf(disk, sizeof disk, "%s", vpath);
    const char* bn = disk + strlen(disk);
    while (bn > disk && bn[-1] != '/' && bn[-1] != '\\') bn--;
    snprintf(out, cap, "%.*s.%s.sockets", (int)(bn - disk), disk, bn);
}

// a mesh's sockets (read once; none: an empty set). reload: read its file again
uint32_t anim_mesh_sockets(const char* vpath, bool reload, AnimSocket* out, uint32_t cap) {
    if (!vpath || !vpath[0]) return 0;
    struct MeshSocks* m = NULL;
    for (uint32_t i = 0; i < g_nmsock && !m; i++)
        if (!strcmp(g_msock[i].vpath, vpath)) m = &g_msock[i];
    if (!m) {
        m = &g_msock[g_nmsock < MESH_SOCK_CACHE ? g_nmsock++ : (g_nmsock = 1, 0)]; // (full: the oldest reused)
        memset(m, 0, sizeof *m);
        snprintf(m->vpath, sizeof m->vpath, "%s", vpath);
    }
    if (!m->loaded || reload) {
        m->loaded = true, m->n = 0;
        char side[1200];
        msock_disk(vpath, side, sizeof side);
        char* text = NULL;
        FILE* f = re_fopen(side, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            text = len > 0 && len < (1 << 20) ? malloc((size_t)len + 1) : NULL;
            if (text) text[fread(text, 1, (size_t)len, f)] = 0;
            fclose(f);
        }
        JsonDoc doc;
        if (text && json_parse(text, strlen(text), &doc, NULL, 0)) {
            const JsonValue* arr = json_get(&doc.root, "sockets");
            for (size_t i = 0; i < json_len(arr) && m->n < ANIM_MAX_SOCKETS; i++) {
                const JsonValue* o = json_at(arr, i);
                AnimSocket* k = &m->s[m->n++];
                memset(k, 0, sizeof *k);
                snprintf(k->name, sizeof k->name, "%s", json_get_str(o, "name", "Socket"));
                const JsonValue* t = json_get(o, "t");
                const JsonValue* q = json_get(o, "q");
                const JsonValue* s = json_get(o, "s");
                for (int a = 0; a < 3; a++) k->local.t[a] = (float)json_num(json_at(t, (size_t)a), 0.0), k->local.s[a] = (float)json_num(json_at(s, (size_t)a), 1.0);
                for (int a = 0; a < 4; a++) k->local.q[a] = (float)json_num(json_at(q, (size_t)a), a == 3 ? 1.0 : 0.0);
                k->has_preview = (uint8_t)json_get_num(o, "preview", 0) != 0, k->preview_mesh = (uint32_t)json_get_num(o, "preview_mesh", 0);
            }
            json_free(&doc);
        }
        free(text);
    }
    uint32_t n = m->n < cap ? m->n : cap;
    if (out) memcpy(out, m->s, sizeof(AnimSocket) * n);
    return m->n;
}

bool anim_mesh_sockets_save(const char* vpath, const AnimSocket* s, uint32_t n) {
    char side[1200];
    msock_disk(vpath, side, sizeof side);
    if (!n) { // (none left: no file)
        remove(side);
        anim_mesh_sockets(vpath, true, NULL, 0);
        return true;
    }
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    jw_key(&w, "sockets");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < n && i < ANIM_MAX_SOCKETS; i++) {
        jw_obj_begin(&w);
        jw_kstr(&w, "name", s[i].name);
        jw_key(&w, "t"), jw_arr_begin(&w);
        for (int a = 0; a < 3; a++) jw_float(&w, s[i].local.t[a]);
        jw_arr_end(&w);
        jw_key(&w, "q"), jw_arr_begin(&w);
        for (int a = 0; a < 4; a++) jw_float(&w, s[i].local.q[a]);
        jw_arr_end(&w);
        jw_key(&w, "s"), jw_arr_begin(&w);
        for (int a = 0; a < 3; a++) jw_float(&w, s[i].local.s[a]);
        jw_arr_end(&w);
        if (s[i].has_preview) jw_kint(&w, "preview", 1), jw_kint(&w, "preview_mesh", (int)s[i].preview_mesh);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    bool ok = write_text(side, &w);
    jw_free(&w);
    anim_mesh_sockets(vpath, true, NULL, 0);
    return ok;
}

// a Static Mesh actor's socket in the world (its own transform times the socket's place)
static bool static_socket_world(World* w, Entity actor, const char* name, float out[3][4]) {
    const CStaticMesh* sm = ecs_get(w, actor, COMP_STATIC_MESH);
    const CTransform* t = ecs_get(w, actor, COMP_TRANSFORM);
    if (!sm || !t || !name || !name[0]) return false;
    const char* vp = render_mesh_vpath(render_mesh_slot(sm->mesh));
    if (!vp || !vp[0]) return false;
    static AnimSocket s[ANIM_MAX_SOCKETS]; // (the main thread's)
    uint32_t n = anim_mesh_sockets(vp, false, s, ANIM_MAX_SOCKETS);
    for (uint32_t i = 0; i < n; i++)
        if (!_stricmp(s[i].name, name)) {
            float rows[3][4];
            render_transform_rows(t, rows);
            xf_rows(&s[i].local, rows, out);
            return true;
        }
    return false;
}
