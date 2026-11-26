// (2026-11-24, item 8: "visualizadores com as opções da Unreal") Included by anim_editor.c: the Skeletal Mesh, Skeleton
// and Animation editors' Details as Unreal's, a search on top and sections that fold (asset_viewer.h's av_section).
// What a section offers works on the asset: the skins' LODs made again (asset_mesh_rebuild), the morph targets' default
// weights (saved in the .rskm), the Physics Asset and IK Rig opened or made, the sequence's Loop.

static const char* mat_name_of(uint32_t hash, char* out, size_t cap) {
    Material* mt = hash ? mat_find_hash(hash) : NULL;
    out[0] = 0;
    if (mt && mat_vpath(mt, out, cap)) {
        const char* b = strrchr(out, '/');
        if (b) memmove(out, b + 1, strlen(b));
    }
    return out[0] ? out : TR("(its color)");
}

// a skin's mesh's "/Content/..." path ("": a shape or none)
static const char* skin_vpath(const SkSkin* s) { return render_mesh_vpath(render_mesh_slot(s->mesh)); }

// The Skeletal Mesh's Details (the Model tab); returns a tab to switch to (-1: none)
static int mesh_asset_panel(SkelMesh* m) {
    static char find[64], msg[240];
    static AssetMeshBuild b;
    static const SkelMesh* b_for;
    int tab = -1;
    related(m);
    if (b_for != m) { // (the first skin's saved LOD Settings, else 4 LODs at 50 / 25 / 12.5%)
        b_for = m, msg[0] = 0;
        memset(&b, 0, sizeof b);
        b.lods = 4, b.percent[0] = 100.0f, b.percent[1] = 50.0f, b.percent[2] = 25.0f, b.percent[3] = 12.5f, b.min_tris = 64;
    }
    av_details_search(find, sizeof find);
    // its LODs: the skins' triangles summed per LOD (and the parts' LOD 0)
    uint32_t tris[4] = { 0 }, nl = 1, parts_tris = 0;
    for (uint32_t i = 0; i < m->nskins; i++) {
        uint32_t lt[4] = { 0 };
        float le[4];
        uint32_t n = render_mesh_lods(m->skins[i].mesh, lt, le);
        if (n > nl) nl = n;
        for (uint32_t l = 0; l < 4; l++) tris[l] += l < n ? lt[l] : lt[0];
    }
    for (uint32_t i = 0; i < m->nparts; i++) {
        uint32_t lt[4] = { 0 };
        float le[4];
        if (render_mesh_lods(m->parts[i].mesh, lt, le)) parts_tris += lt[0];
    }
    if (av_section("Asset Details", "bones sockets parts skins morph source reimport file rigid")) {
        av_row(TR("Bones"), "%u", m->nbones);
        av_row(TR("Sockets"), "%u", m->nsockets);
        av_row(TR("Skins"), "%u", m->nskins);
        av_row(TR("Parts"), "%u%s", m->nparts, m->rigid ? TR(" (rigid by bone)") : "");
        av_row(TR("Morph Targets"), "%u", m->nmorphs);
        av_row(TR("Triangles"), "%u", tris[0] + parts_tris);
        if (m->source[0]) av_row(TR("Source File"), "%s", m->source);
    }
    if (m->nskins && av_section("LOD Settings", "lod level of detail reduction reduce triangles percent simplify")) {
        igPushTextWrapPos(0.0f);
        if (igBeginTable("sk_lods", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0, 0 }, 0)) {
            igTableSetupColumn("LOD", 0, 0.5f, 0), igTableSetupColumn(TR("Triangles"), 0, 1.0f, 0), igTableSetupColumn("%", 0, 0.6f, 0);
            igTableHeadersRow();
            for (uint32_t l = 0; l < nl && l < 4; l++) {
                igTableNextRow(0, 0);
                igTableNextColumn(), igText("%u", l);
                igTableNextColumn(), igText("%u", tris[l]);
                igTableNextColumn(), igText("%.0f", tris[0] ? 100.0 * tris[l] / tris[0] : 0.0);
            }
            igEndTable();
        }
        igSliderInt(TR("Number of LODs"), &b.lods, 2, 4, "%d", 0);
        for (int l = 1; l < b.lods && l < 4; l++) {
            char lb[48];
            snprintf(lb, sizeof lb, "LOD %d##sklod%d", l, l);
            if (b.percent[l] <= 0.0f) b.percent[l] = b.percent[l - 1] * 0.5f;
            igSliderFloat(lb, &b.percent[l], 0.5f, 100.0f, TR("%.1f%% of the triangles"), ImGuiSliderFlags_Logarithmic);
            if (b.percent[l] > b.percent[l - 1]) b.percent[l] = b.percent[l - 1];
        }
        if (igButton(TR("Apply LOD Settings"), (ImVec2){ 0, 0 })) {
            int done = 0;
            for (uint32_t i = 0; i < m->nskins; i++) {
                const char* vp = skin_vpath(&m->skins[i]);
                if (vp && vp[0] && asset_mesh_rebuild(vp, &b, false, false, msg, sizeof msg)) done++;
            }
            if (done) snprintf(msg + strlen(msg), sizeof msg - strlen(msg), TR(" (%d skin(s))"), done);
        }
        if (msg[0]) igTextWrapped("%s", msg);
        igTextDisabled("%s", TR("The skins' LODs made again on LOD 0's vertices (the bones' weights stay right)."));
        igPopTextWrapPos();
    }
    if ((m->nskins || m->nparts) && av_section("Materials", "material slot color skin part element")) {
        char nm[160];
        for (uint32_t i = 0; i < m->nskins; i++) av_row(TR("Skin"), "%u: %s", i, mat_name_of(m->skins[i].material, nm, sizeof nm));
        for (uint32_t i = 0; i < m->nparts && i < 16; i++) av_row(TR("Part"), "%s: %s", m->bones[m->parts[i].bone].name, mat_name_of(m->parts[i].material, nm, sizeof nm));
        igTextDisabled("%s", TR("A part's material and color: select its bone."));
    }
    if (m->nmorphs && av_section("Morph Targets", "morph target blend shape weight face")) {
        for (uint32_t i = 0; i < m->nmorphs; i++) {
            char lb[96];
            snprintf(lb, sizeof lb, "%s##morph%u", m->morph_name[i], i);
            if (igSliderFloat(lb, &m->morph_default[i], 0.0f, 1.0f, "%.2f", 0)) E.unsaved = true;
        }
        igTextDisabled("%s", TR("Default weights (saved with the mesh); animations' curves and the Set Morph Target node change them while playing."));
    }
    if (av_section("Physics and IK", "physics asset ik rig ragdoll bodies collision constraints")) {
        av_row(TR("Physics Asset"), "%s", g_rel.phys[0] ? g_rel.phys : TR("(none)"));
        if (igButton(g_rel.phys[0] ? TR("Open Physics Asset") : TR("Create Physics Asset"), (ImVec2){ 0, 0 })) tab = AVP_TAB_PHYSICS; // tips: none (Open / Create: the Physics Asset row's buttons, described there)
        av_row(TR("IK Rig"), "%s", g_rel.rig[0] ? g_rel.rig : TR("(none)"));
        if (g_rel.rig[0] && igButton(TR("Open IK Rig"), (ImVec2){ 0, 0 })) tab = AVP_TAB_IKRIG;
    }
    if (av_section("Selected Bone", "bone parent bind pose parts")) mesh_details(m);
    av_details_end();
    return tab;
}

// The Skeleton tab's Details: the skeleton, the bone, its sockets
static void skeleton_panel(SkelMesh* m) {
    static char find[64];
    av_details_search(find, sizeof find);
    if (av_section("Skeleton", "bones hierarchy rig system class")) {
        av_row(TR("Bones"), "%u", m->nbones);
        av_row(TR("Sockets"), "%u", m->nsockets);
        const RigInfo* ri = rig_info(m);
        if (ri) av_row(TR("Rig"), "%s", TR(rig_system_label(ri->system)));
    }
    if (av_section("Selected Bone", "bone parent bind pose sockets socket")) skeleton_details(m);
    av_details_end();
}

// The Animation Sequence's Details: the asset and the notify selected
static void sequence_panel(AnimSeq* s) {
    static char find[64];
    av_details_search(find, sizeof find);
    if (av_section("Animation", "length frames rate fps tracks skeleton loop curves notifies")) {
        av_row(TR("Skeleton"), "%s", s->skeleton);
        av_row(TR("Length"), "%.2f s", s->length);
        av_row(TR("Frames"), "%u", s->nframes);
        av_row(TR("Frame Rate"), "%.1f fps", s->fps > 0.0f ? s->fps : (s->length > 0.0f ? (float)(s->nframes > 1 ? s->nframes - 1 : 1) / s->length : 0.0f));
        av_row(TR("Tracks"), "%u", s->ntracks);
        av_row(TR("Notifies"), "%u", s->nnotifies);
        av_row(TR("Curves"), "%u", s->ncurves);
        av_row(TR("Memory"), "%.1f KB", (double)s->nframes * s->ntracks * 7.0 * 4.0 / 1024.0);
        if (igCheckbox(TR("Loop"), &s->loop)) E.unsaved = s->dirty = true;
    }
}
