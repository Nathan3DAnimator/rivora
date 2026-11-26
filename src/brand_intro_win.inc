// (2026-11-23, the user: "the opening like Blender's: a transparent background, a smooth, pretty animation, the blue of
// the R in the editor's corner as the base") Included by brand.c (Windows). The editor's intro as a window of its own
// over the desktop: a layered window (alpha per pixel, UpdateLayeredWindow) composited on the CPU by a thread of its
// own, started right after glfwInit, so it plays while the engine starts behind it (Vulkan, ImGui) and the main window
// shows only once it ends. The blue R grows in (an ease with a little overshoot) with a soft shadow and a glow behind
// it, a light sweeps across it, "Real Engine" and the version rise in under it with an accent line, then all of it
// fades. A click on it, Esc, Space or Enter fades it out at once. ~2.6 s; < 6 ms of one core a frame (only the boxes
// that have something are composited: the glow's map, the R's and the texts' masks are made once).

#define LI_LEN 2.6f
#define LI_SKIP_FADE 0.22f

typedef struct LiMask {
    int w, h;
    float* a; // 0..1
} LiMask;

static struct {
    HANDLE thread;
    volatile LONG skip;
    bool background;
    int mon_x, mon_y, mon_w, mon_h; // where (0 size: the primary's work area)
} g_li;

const unsigned char* theme_title_font(size_t* n); // theme.c: Sora SemiBold (the menu bar's title)

static float li_ease_out(float x) {
    x = clamp01(x);
    float k = 1.0f - x;
    return 1.0f - k * k * k;
}
static float li_ease_back(float x) { // (a little past, then settled)
    x = clamp01(x);
    const float c1 = 1.25f, c3 = c1 + 1.0f;
    float k = x - 1.0f;
    return 1.0f + c3 * k * k * k + c1 * k * k;
}

static void li_blur(LiMask* m, int r) { // three box passes: ~a gaussian
    if (r < 1 || !m->a) return;
    float* tmp = malloc(sizeof(float) * (size_t)(m->w > m->h ? m->w : m->h));
    if (!tmp) return;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < m->h; y++) {
            float* row = m->a + (size_t)y * m->w, s = 0.0f;
            for (int x = -r; x <= r; x++) s += row[x < 0 ? 0 : x >= m->w ? m->w - 1 : x];
            for (int x = 0; x < m->w; x++) {
                tmp[x] = s / (float)(2 * r + 1);
                int xo = x - r, xi = x + r + 1;
                s += row[xi >= m->w ? m->w - 1 : xi] - row[xo < 0 ? 0 : xo];
            }
            memcpy(row, tmp, sizeof(float) * (size_t)m->w);
        }
        for (int x = 0; x < m->w; x++) {
            float s = 0.0f;
            for (int y = -r; y <= r; y++) s += m->a[(size_t)(y < 0 ? 0 : y >= m->h ? m->h - 1 : y) * m->w + x];
            for (int y = 0; y < m->h; y++) {
                tmp[y] = s / (float)(2 * r + 1);
                int yo = y - r, yi = y + r + 1;
                s += m->a[(size_t)(yi >= m->h ? m->h - 1 : yi) * m->w + x] - m->a[(size_t)(yo < 0 ? 0 : yo) * m->w + x];
            }
            for (int y = 0; y < m->h; y++) m->a[(size_t)y * m->w + x] = tmp[y];
        }
    }
    free(tmp);
}
static bool li_copy(const LiMask* a, LiMask* b) {
    b->w = a->w, b->h = a->h, b->a = malloc(sizeof(float) * (size_t)a->w * a->h);
    if (b->a) memcpy(b->a, a->a, sizeof(float) * (size_t)a->w * a->h);
    return b->a != NULL;
}
static float li_at(const LiMask* m, float x, float y) { // bilinear, 0 outside
    if (x <= -1.0f || y <= -1.0f || x >= (float)m->w || y >= (float)m->h) return 0.0f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0, v[4];
    for (int k = 0; k < 4; k++) {
        int xx = x0 + (k & 1), yy = y0 + (k >> 1);
        v[k] = xx < 0 || yy < 0 || xx >= m->w || yy >= m->h ? 0.0f : m->a[(size_t)yy * m->w + xx];
    }
    return (v[0] + (v[1] - v[0]) * fx) * (1.0f - fy) + (v[2] + (v[3] - v[2]) * fx) * fy;
}

// the logo (the silver R and its orbit on black) as alpha, L x L by a box filter (as brand_mark), in the middle of a
// mask pad wider on each side (its shadow's blur goes past the picture: not cut at its edge)
static bool li_logo(int L, int pad, LiMask* m) {
    uint32_t w = 0, h = 0;
    uint8_t* px = mat_load_image_rgba_mem(g_brand_logo, sizeof g_brand_logo, &w, &h);
    if (!px || w < 8 || h < 8) return free(px), false;
    m->w = m->h = L + pad * 2, m->a = calloc((size_t)m->w * m->h, sizeof(float));
    if (!m->a) return free(px), false;
    for (int y = 0; y < L; y++)
        for (int x = 0; x < L; x++) {
            uint32_t x0 = (uint32_t)x * w / (uint32_t)L, x1 = (uint32_t)(x + 1) * w / (uint32_t)L, y0 = (uint32_t)y * h / (uint32_t)L, y1 = (uint32_t)(y + 1) * h / (uint32_t)L;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            uint32_t n = 0, sum = 0;
            for (uint32_t sy = y0; sy < y1 && sy < h; sy++)
                for (uint32_t sx = x0; sx < x1 && sx < w; sx++, n++) {
                    const uint8_t* p = px + ((size_t)sy * w + sx) * 4;
                    uint32_t mx = p[0] > p[1] ? p[0] : p[1];
                    sum += (mx > p[2] ? mx : p[2]) * p[3] / 255u;
                }
            float a = n ? (float)sum / (float)n / 255.0f * 1.25f : 0.0f;
            m->a[(size_t)(y + pad) * m->w + x + pad] = a > 1.0f ? 1.0f : a;
        }
    free(px);
    return true;
}

// a line of text as alpha (GDI, antialiased, white on black: the green channel), in Sora SemiBold when it loads
static bool li_text(const char* utf8, int px, LiMask* m) {
    wchar_t s[128];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, s, 128) - 1;
    if (n <= 0) return false;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return false;
    HFONT f = NULL;
    static const wchar_t* const faces[3] = { L"Sora SemiBold", L"Sora", L"Segoe UI Semibold" };
    for (int i = 0; i < 3 && !f; i++) {
        f = CreateFontW(-px, 0, 0, 0, i == 1 ? 600 : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, faces[i]);
        SelectObject(dc, f);
        wchar_t got[64] = L"";
        GetTextFaceW(dc, 64, got);
        if (_wcsnicmp(got, L"Sora", 4) && i < 2) DeleteObject(f), f = NULL; // (not installed: the next)
    }
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(dc, s, n, &sz);
    int pad = px / 3 + 2;
    m->w = sz.cx + pad * 2, m->h = sz.cy + pad * 2;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader, bi.bmiHeader.biWidth = m->w, bi.bmiHeader.biHeight = -m->h;
    bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    bool ok = false;
    if (bm && bits) {
        HGDIOBJ old = SelectObject(dc, bm);
        memset(bits, 0, (size_t)m->w * m->h * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, pad, pad, s, n);
        GdiFlush();
        m->a = malloc(sizeof(float) * (size_t)m->w * m->h);
        if (m->a) {
            for (size_t i = 0; i < (size_t)m->w * m->h; i++) m->a[i] = (float)((const uint8_t*)bits)[i * 4 + 1] / 255.0f;
            ok = true;
        }
        SelectObject(dc, old);
    }
    if (bm) DeleteObject(bm);
    if (f) DeleteObject(f);
    DeleteDC(dc);
    return ok;
}

static LRESULT CALLBACK li_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: InterlockedExchange(&g_li.skip, 1); return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == VK_SPACE || wp == VK_RETURN) InterlockedExchange(&g_li.skip, 1);
        return 0;
    case WM_SETCURSOR: SetCursor(LoadCursor(NULL, IDC_ARROW)); return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// "over" in premultiplied color: c (straight, 0..1) at alpha a over the pixel d
static inline void li_over(float* d, float r, float g, float b, float a) {
    float k = 1.0f - a;
    d[0] = r * a + d[0] * k, d[1] = g * a + d[1] * k, d[2] = b * a + d[2] * k, d[3] = a + d[3] * k;
}

static DWORD WINAPI li_thread(LPVOID arg) {
    (void)arg;
    HDC sdc = GetDC(NULL);
    float dpi = sdc ? (float)GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f : 1.0f;
    if (sdc) ReleaseDC(NULL, sdc);
    if (dpi < 1.0f) dpi = 1.0f;
    int W = (int)(620.0f * dpi), H = (int)(480.0f * dpi), L = (int)(250.0f * dpi);
    LiMask logo = { 0 }, logo_sh = { 0 }, title = { 0 }, title_sh = { 0 }, ver = { 0 }, glow = { 0 };
    int lpad = (int)(24.0f * dpi);
    bool ok = li_logo(L, lpad, &logo) && li_copy(&logo, &logo_sh) && li_text("Rivora Engine", (int)(44.0f * dpi), &title) && li_copy(&title, &title_sh);
    char vs[48];
    snprintf(vs, sizeof vs, "%s", RE_VERSION_STRING);
    ok = ok && li_text(vs, (int)(19.0f * dpi), &ver);
    li_blur(&logo_sh, (int)(5.0f * dpi)), li_blur(&title_sh, (int)(4.0f * dpi));
    // the glow behind the R: a soft disc (made once, its strength animated)
    glow.w = W, glow.h = H, glow.a = ok ? malloc(sizeof(float) * (size_t)glow.w * glow.h) : NULL;
    float cx = W * 0.5f, cy = H * 0.38f, sig = L * 0.5f;
    if (glow.a)
        for (int y = 0; y < glow.h; y++)
            for (int x = 0; x < glow.w; x++) {
                float dx = (float)x - cx, dy = ((float)y - cy) * 1.1f;
                glow.a[(size_t)y * glow.w + x] = expf(-(dx * dx + dy * dy) / (2.0f * sig * sig));
            }
    HWND wnd = NULL;
    HDC mdc = NULL;
    HBITMAP dib = NULL;
    uint8_t* bits = NULL;
    float* acc = NULL;
    if (ok && glow.a) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc, wc.lpfnWndProc = li_proc, wc.hInstance = GetModuleHandleW(NULL), wc.lpszClassName = L"RealEngineIntro";
        wc.hIcon = LoadIconW(wc.hInstance, L"GLFW_ICON"); // (the .rc's name: an id 1 found nothing)
        RegisterClassExW(&wc);
        int x, y;
        if (g_li.mon_w > 0) x = g_li.mon_x + (g_li.mon_w - W) / 2, y = g_li.mon_y + (g_li.mon_h - H) / 2;
        else {
            RECT wa;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            x = (wa.left + wa.right - W) / 2, y = (wa.top + wa.bottom - H) / 2;
        }
        if (g_li.background) x = GetSystemMetrics(SM_XVIRTUALSCREEN) - W - 200, y = GetSystemMetrics(SM_YVIRTUALSCREEN) + 40;
        wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | (g_li.background || g_li.mon_w > 0 ? WS_EX_NOACTIVATE : WS_EX_TOPMOST), L"RealEngineIntro", L"Rivora Engine", WS_POPUP,
                              x, y, W, H, NULL, NULL, wc.hInstance, NULL);
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader, bi.bmiHeader.biWidth = W, bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
        mdc = CreateCompatibleDC(NULL);
        dib = mdc ? CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0) : NULL;
        acc = malloc(sizeof(float) * 4 * (size_t)W * H);
    }
    if (wnd && dib && bits && acc) {
        HGDIOBJ old = SelectObject(mdc, dib);
        HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        bool shown = false;
        LARGE_INTEGER freq, t0, now;
        QueryPerformanceFrequency(&freq), QueryPerformanceCounter(&t0);
        float skip_at = -1.0f;
        const char* shots = getenv("REALENGINE_INTRO_SHOTS");
        static const float shot_t[6] = { 0.2f, 0.45f, 0.8f, 1.1f, 1.6f, 2.3f };
        int shot_i = 0, frames = 0;
        double cost = 0.0, worst = 0.0; // (the composition's time, the measure)
        // where the texts sit (under the R)
        float ty = cy + L * 0.5f + 10.0f * dpi, tx = cx - title.w * 0.5f, vy = ty + title.h - 6.0f * dpi, vx = cx - ver.w * 0.5f;
        // the accent (Azul Real's: the R of the menu bar), lighter at the top, deeper at the foot
        const float top[3] = { 0.60f, 0.76f, 1.0f }, mid[3] = { 43 / 255.0f, 111 / 255.0f, 227 / 255.0f }, foot[3] = { 0.10f, 0.29f, 0.70f };
        for (;;) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) TranslateMessage(&msg), DispatchMessageW(&msg);
            QueryPerformanceCounter(&now);
            float t = (float)((double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
            if (g_li.skip && skip_at < 0.0f) skip_at = t;
            float out = 1.0f - smooth(LI_LEN - 0.45f, LI_LEN, t);
            if (skip_at >= 0.0f) out = fminf(out, 1.0f - clamp01((t - skip_at) / LI_SKIP_FADE));
            if (out <= 0.0f || t > LI_LEN) break;
            // the moments
            float aR = li_ease_out((t - 0.05f) / 0.45f) * out, k = 0.70f + 0.30f * li_ease_back((t - 0.05f) / 0.8f);
            float sweep = (t - 0.6f) / 0.65f, aG = 0.28f * smooth(0.15f, 0.9f, t) * (1.0f + 0.10f * sinf(t * 2.6f)) * out;
            float aT = li_ease_out((t - 0.55f) / 0.55f) * out, aV = li_ease_out((t - 0.8f) / 0.55f) * out;
            float rise = (1.0f - li_ease_out((t - 0.55f) / 0.6f)) * 16.0f * dpi, line = li_ease_out((t - 0.95f) / 0.7f);
            memset(acc, 0, sizeof(float) * 4 * (size_t)W * H);
            // the glow
            for (int y = 0; y < glow.h && aG > 0.002f; y++)
                for (int x = 0; x < W; x++) {
                    float a = glow.a[(size_t)y * W + x] * aG;
                    if (a > 0.002f) li_over(acc + ((size_t)y * W + x) * 4, mid[0], mid[1], mid[2], a);
                }
            // the R: its shadow, then it (scaled about its center), the light's band across it
            if (aR > 0.002f) {
                float half = (L * 0.5f + lpad) * k, lc = L * 0.5f + lpad; // (the mask's middle)
                int x0 = (int)(cx - half), x1 = (int)(cx + half), y0 = (int)(cy - half), y1 = (int)(cy + half + 8.0f * dpi);
                for (int y = y0 < 0 ? 0 : y0; y < y1 && y < H; y++)
                    for (int x = x0 < 0 ? 0 : x0; x < x1 && x < W; x++) {
                        float mx = ((float)x - cx) / k + lc, my = ((float)y - cy) / k + lc;
                        float* d = acc + ((size_t)y * W + x) * 4;
                        float sh = li_at(&logo_sh, mx, my - 5.0f * dpi / k) * 0.55f * aR;
                        if (sh > 0.002f) li_over(d, 0, 0, 0, sh);
                        float a = li_at(&logo, mx, my) * aR;
                        if (a <= 0.002f) continue;
                        float v = clamp01((my - lpad) / (float)L), c[3];
                        for (int i = 0; i < 3; i++) c[i] = v < 0.5f ? top[i] + (mid[i] - top[i]) * (v * 2.0f) : mid[i] + (foot[i] - mid[i]) * (v * 2.0f - 1.0f);
                        if (sweep > 0.0f && sweep < 1.0f) { // (a slanted band of light from the lower left to the upper right)
                            float u = (mx - lpad + (L - (my - lpad))) / (2.0f * L), b = (u - (sweep * 1.5f - 0.25f)) / 0.07f, s = expf(-b * b) * 0.85f;
                            for (int i = 0; i < 3; i++) c[i] += (1.0f - c[i]) * s;
                        }
                        li_over(d, c[0], c[1], c[2], a);
                    }
            }
            // the name and the version, rising in, their shadows; the accent's line between them, growing from the middle
            for (int pass = 0; pass < 2; pass++) {
                const LiMask* m = pass ? &ver : &title;
                float a0 = pass ? aV : aT, ox = pass ? vx : tx, oy = (pass ? vy : ty) + rise;
                if (a0 <= 0.002f) continue;
                for (int y = (int)oy; y < (int)oy + m->h + 3 && y < H; y++)
                    for (int x = (int)ox; x < (int)ox + m->w && x < W; x++) {
                        if (x < 0 || y < 0) continue;
                        float* d = acc + ((size_t)y * W + x) * 4, fy = (float)y - oy, fx = (float)x - ox;
                        if (!pass) {
                            float sh = li_at(&title_sh, fx, fy - 2.0f * dpi) * 0.85f * a0; // (stronger: legible over a light desktop too)
                            if (sh > 0.002f) li_over(d, 0, 0, 0, sh);
                        }
                        float a = li_at(m, fx, fy) * a0;
                        if (a > 0.002f) {
                            if (pass) li_over(d, 0.45f, 0.66f, 1.0f, a);
                            else li_over(d, 0.95f, 0.96f, 0.98f, a);
                        }
                    }
            }
            if (line > 0.0f && aT > 0.0f) {
                float hw = title.w * 0.32f * line, ly = vy - 1.0f * dpi + rise;
                for (int y = (int)ly; y < (int)(ly + 2.0f * dpi) && y < H; y++)
                    for (int x = (int)(cx - hw); x < (int)(cx + hw); x++) {
                        if (x < 0 || x >= W || y < 0) continue;
                        float e = 1.0f - fabsf((float)x - cx) / fmaxf(hw, 1.0f); // (fading toward its ends)
                        li_over(acc + ((size_t)y * W + x) * 4, mid[0], mid[1], mid[2], clamp01(e * 1.6f) * aT * 0.9f);
                    }
            }
            for (size_t i = 0; i < (size_t)W * H; i++) { // BGRA, premultiplied
                const float* s = acc + i * 4;
                bits[i * 4 + 0] = (uint8_t)(clamp01(s[2]) * 255.0f + 0.5f), bits[i * 4 + 1] = (uint8_t)(clamp01(s[1]) * 255.0f + 0.5f);
                bits[i * 4 + 2] = (uint8_t)(clamp01(s[0]) * 255.0f + 0.5f), bits[i * 4 + 3] = (uint8_t)(clamp01(s[3]) * 255.0f + 0.5f);
            }
            if (shots && shot_i < 6 && t >= shot_t[shot_i]) { // (REALENGINE_INTRO_SHOTS=<prefix>: frames over a dark and a light desktop)
                uint8_t* rgb = malloc((size_t)W * H * 3 * 2);
                if (rgb) {
                    for (int y = 0; y < H; y++)
                        for (int x = 0; x < W * 2; x++) {
                            const float* c = acc + ((size_t)y * W + (x % W)) * 4;
                            float bg = x < W ? 0.13f : 0.86f; // (the left half over dark, the right over light)
                            for (int i = 0; i < 3; i++) rgb[((size_t)y * W * 2 + x) * 3 + i] = (uint8_t)(clamp01(c[i] + bg * (1.0f - c[3])) * 255.0f + 0.5f);
                        }
                    char path[600];
                    snprintf(path, sizeof path, "%s%d.png", shots, shot_i);
                    vkctx_png_write_rgb(path, rgb, (uint32_t)W * 2, (uint32_t)H);
                    free(rgb);
                }
                shot_i++;
            }
            POINT src = { 0, 0 };
            SIZE size = { W, H };
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(wnd, NULL, NULL, &size, mdc, &src, 0, &bf, ULW_ALPHA);
            if (!shown) {
                ShowWindow(wnd, g_li.background || g_li.mon_w > 0 ? SW_SHOWNOACTIVATE : SW_SHOW), shown = true;
                if (!g_li.background && g_li.mon_w <= 0) SetForegroundWindow(wnd);
            }
            // the next frame: ~60 per second (a high resolution timer when there is one)
            LARGE_INTEGER after;
            QueryPerformanceCounter(&after);
            double spent = (double)(after.QuadPart - now.QuadPart) / (double)freq.QuadPart, wait = 1.0 / 60.0 - spent;
            frames++, cost += spent, worst = spent > worst ? spent : worst;
            if (wait > 0.0) {
                if (timer) {
                    LARGE_INTEGER due;
                    due.QuadPart = -(LONGLONG)(wait * 1e7);
                    SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
                    WaitForSingleObject(timer, 50);
                } else
                    Sleep((DWORD)(wait * 1000.0));
            }
        }
        if (timer) CloseHandle(timer);
        SelectObject(mdc, old);
        printf("[RealEngine] intro: %d frames in its own window (%d x %d), %.2f ms a frame composited (worst %.2f), on a thread of its own\n", frames, W, H,
               frames ? cost * 1000.0 / frames : 0.0, worst * 1000.0);
    }
    if (wnd) DestroyWindow(wnd);
    if (dib) DeleteObject(dib);
    if (mdc) DeleteDC(mdc);
    free(acc), free(logo.a), free(logo_sh.a), free(title.a), free(title_sh.a), free(ver.a), free(glow.a);
    return 0;
}

bool brand_intro_start(bool background, int mon_x, int mon_y, int mon_w, int mon_h) {
    if (!g_brand_intro_on || g_li.thread) return g_li.thread != NULL;
    g_li.background = background, g_li.skip = 0;
    g_li.mon_x = mon_x, g_li.mon_y = mon_y, g_li.mon_w = mon_w, g_li.mon_h = mon_h;
    { // (Sora for the name: the menu bar's title font, from memory, for this process only)
        size_t n = 0;
        const unsigned char* f = theme_title_font(&n);
        DWORD count = 0;
        if (f && n) AddFontMemResourceEx((void*)f, (DWORD)n, NULL, &count);
    }
    g_li.thread = CreateThread(NULL, 0, li_thread, NULL, 0, NULL);
    return g_li.thread != NULL;
}

// the intro in its own window ends (the main window still hidden): the GLFW events pumped meanwhile
static bool li_wait(GLFWwindow* window) {
    if (!g_li.thread) return false;
    while (WaitForSingleObject(g_li.thread, 10) == WAIT_TIMEOUT) {
        glfwPollEvents();
        crash_heartbeat();
        if (window && glfwWindowShouldClose(window)) InterlockedExchange(&g_li.skip, 1);
    }
    CloseHandle(g_li.thread);
    g_li.thread = NULL;
    return true;
}
