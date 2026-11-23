// The Android game (notas-dev/android.md; adapted from the Tlender Mobile's activity): the game itself is native
// (librealengine_game.so through NativeActivity); this class only does what needs Java: the on-screen keyboard's text,
// the full-screen mode, the folders, the language, HTTP, links, the clipboard and a short vibration (the touch buttons).
// The native side calls these methods through JNI (android/glfw_android.c, android/platform_android.c).
package com.realengine.game;

import android.app.NativeActivity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.text.InputType;
import android.util.DisplayMetrics;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

public class GameActivity extends NativeActivity {
    // Registered by the native side (RegisterNatives in android_main)
    static native void nativeText(String text);
    static native void nativeKey(int keyCode);

    private TextSink sink;

    // An invisible view that takes the on-screen keyboard's text: every character is committed at once (no word
    // suggestions or composing), sent to the app as GLFW characters; backspace and enter come as key events.
    private class TextSink extends View {
        TextSink(Context c) {
            super(c);
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean onCheckIsTextEditor() { return true; }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo info) {
            info.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                           | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
            info.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                            | EditorInfo.IME_ACTION_DONE;
            return new BaseInputConnection(this, false) {
                @Override
                public boolean commitText(CharSequence text, int newCursorPosition) {
                    if (text != null && text.length() > 0) nativeText(text.toString());
                    return true;
                }

                @Override
                public boolean deleteSurroundingText(int before, int after) {
                    for (int i = 0; i < Math.max(before, 1); i++) nativeKey(KeyEvent.KEYCODE_DEL);
                    return true;
                }

                @Override
                public boolean performEditorAction(int action) {
                    nativeKey(KeyEvent.KEYCODE_ENTER);
                    return true;
                }
            };
        }
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        sink = new TextSink(this);
        addContentView(sink, new ViewGroup.LayoutParams(1, 1));
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); // (a game: the screen stays on)
        if (Build.VERSION.SDK_INT >= 28)
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        immersive();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) immersive();
    }

    // Full screen: the system bars hide and come back with a swipe from the edge.
    private void immersive() {
        View decor = getWindow().getDecorView();
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = decor.getWindowInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
    }

    // --- The opening animation (src/intro.c on Android) ----------------------------------------------------------
    // The native thread draws each frame (premultiplied R, G, B, A, w x h) and hands it over in introFrame; this window
    // over the app (not focusable: the app keeps the keys and the full screen) shows it centered on the app's dark
    // background, scaled up to the screen, both fading with the mark. A touch skips (nativeIntroSkip).
    static native void nativeIntroSkip();

    private android.app.Dialog introDialog;
    private volatile IntroView introView;

    private class IntroView extends View {
        final android.graphics.Bitmap[] bm = new android.graphics.Bitmap[2];
        int front = -1;
        float bgAlpha = 1, ds = 1;
        final android.graphics.Paint paint = new android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG);
        final android.graphics.Rect dst = new android.graphics.Rect();

        IntroView(Context c, int w, int h, float ds) {
            super(c);
            this.ds = ds;
            for (int i = 0; i < 2; i++) bm[i] = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888);
        }

        @Override
        public boolean onTouchEvent(android.view.MotionEvent e) {
            if (e.getActionMasked() == android.view.MotionEvent.ACTION_DOWN) nativeIntroSkip();
            return true;
        }

        @Override
        protected void onDraw(android.graphics.Canvas c) {
            synchronized (this) {
                c.drawARGB(Math.round(255 * Math.max(0, Math.min(1, bgAlpha))), 0x1d, 0x1d, 0x1d);
                if (front < 0) return;
                android.graphics.Bitmap b = bm[front];
                int vw = getWidth(), vh = getHeight();
                // the mark as big as on the PC (360 dp) but never past the screen
                float s = Math.min(Math.min(vw, vh) * 1.0f / b.getWidth(), getResources().getDisplayMetrics().density / ds);
                int w = Math.round(b.getWidth() * s), h = Math.round(b.getHeight() * s);
                dst.set((vw - w) / 2, (vh - h) / 2, (vw + w) / 2, (vh + h) / 2);
                c.drawBitmap(b, null, dst, paint);
            }
        }
    }

    public void introShow(final int w, final int h, final float ds) {
        runOnUiThread(() -> {
            try {
                introView = new IntroView(this, w, h, ds);
                android.app.Dialog d = new android.app.Dialog(this, android.R.style.Theme_Translucent_NoTitleBar_Fullscreen);
                d.setContentView(introView);
                d.setCancelable(false);
                android.view.Window win = d.getWindow();
                if (win != null) {
                    win.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                                 | WindowManager.LayoutParams.FLAG_FULLSCREEN);
                    win.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
                    win.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
                    if (Build.VERSION.SDK_INT >= 28)
                        win.getAttributes().layoutInDisplayCutoutMode =
                            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
                }
                d.show();
                introDialog = d;
            } catch (Exception e) {
                android.util.Log.w("Tlender", "introShow: " + e);
                introView = null;
            }
        });
    }

    // the native thread: copies the frame into the back bitmap and swaps
    public void introFrame(java.nio.ByteBuffer px, float bgAlpha) {
        IntroView v = introView;
        if (v == null) return;
        synchronized (v) {
            int back = v.front == 0 ? 1 : 0;
            px.rewind();
            v.bm[back].copyPixelsFromBuffer(px);
            v.front = back;
            v.bgAlpha = bgAlpha;
        }
        v.postInvalidateOnAnimation();
    }

    public void introHide() {
        runOnUiThread(() -> {
            try {
                if (introDialog != null) introDialog.dismiss();
            } catch (Exception ignored) {
            }
            introDialog = null;
            introView = null;
        });
    }

    // a stage's name: capitals, letter-spaced, white on nothing; the coverage (alpha) of each pixel, w x h
    public byte[] introCaption(String text, int w, int h, float sizePx, float spacingPx) {
        try {
            android.graphics.Bitmap b = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ALPHA_8);
            android.graphics.Canvas c = new android.graphics.Canvas(b);
            android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
            p.setColor(0xffffffff);
            p.setTextSize(sizePx);
            p.setTypeface(android.graphics.Typeface.create(android.graphics.Typeface.SANS_SERIF, android.graphics.Typeface.BOLD));
            p.setTextAlign(android.graphics.Paint.Align.CENTER);
            if (Build.VERSION.SDK_INT >= 21) p.setLetterSpacing(spacingPx / sizePx);
            android.graphics.Paint.FontMetrics fm = p.getFontMetrics();
            c.drawText(text.toUpperCase(java.util.Locale.ROOT), w * 0.5f, h * 0.5f - (fm.ascent + fm.descent) * 0.5f, p);
            int stride = b.getRowBytes();
            byte[] all = new byte[stride * h], out = new byte[w * h];
            b.copyPixelsToBuffer(java.nio.ByteBuffer.wrap(all));
            for (int y = 0; y < h; y++) System.arraycopy(all, y * stride, out, y * w, w);
            b.recycle();
            return out;
        } catch (Exception e) {
            return null;
        }
    }

    // --- Called from native code ------------------------------------------------------------------------------
    public String filesDir() { return getFilesDir().getAbsolutePath(); }
    public String cacheDirPath() { return getCacheDir().getAbsolutePath(); }

    public String documentsDir() {
        File base = getExternalFilesDir(null);
        if (base == null) base = getFilesDir();
        File d = new File(base, "Saved");
        d.mkdirs();
        return d.getAbsolutePath();
    }

    public String screenInches() {
        DisplayMetrics m = getResources().getDisplayMetrics();
        double w = m.widthPixels / (double) m.xdpi, h = m.heightPixels / (double) m.ydpi;
        return String.valueOf(Math.sqrt(w * w + h * h));
    }

    public String language() { return Locale.getDefault().getLanguage(); }

    // AR (src/ar_arcore.c): the camera's permission (asked once; ARCore starts when it is given) and the screen's rotation
    // (ARCore turns the camera's picture with it: 0, 1, 2, 3 = 0, 90, 180, 270 degrees)
    public String hasCameraPermission() {
        return checkSelfPermission(android.Manifest.permission.CAMERA) == android.content.pm.PackageManager.PERMISSION_GRANTED ? "1" : "0";
    }

    public void requestCameraPermission() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() { requestPermissions(new String[] { android.Manifest.permission.CAMERA }, 7301); }
        });
    }

    // (Q25b, src/xr_mr.c) the Quest's Scene permission (the room of its Space Setup, the real depth): asked once
    public String hasScenePermission() {
        return checkSelfPermission("com.oculus.permission.USE_SCENE") == android.content.pm.PackageManager.PERMISSION_GRANTED ? "1" : "0";
    }

    public void requestScenePermission() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() { requestPermissions(new String[] { "com.oculus.permission.USE_SCENE" }, 7302); }
        });
    }

    public String displayRotation() {
        android.view.Display d = Build.VERSION.SDK_INT >= 30 ? getDisplay() : getWindowManager().getDefaultDisplay();
        return String.valueOf(d != null ? d.getRotation() : 0);
    }

    // The battery saver on: the game drops to 30 fps (src/mobile.c asks every few seconds)
    public String powerSave() {
        android.os.PowerManager pm = (android.os.PowerManager) getSystemService(Context.POWER_SERVICE);
        return pm != null && pm.isPowerSaveMode() ? "1" : "0";
    }

    public void vibrate(final int ms) {
        Vibrator v = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
        if (v == null || !v.hasVibrator()) return;
        if (Build.VERSION.SDK_INT >= 26) v.vibrate(VibrationEffect.createOneShot(ms, VibrationEffect.DEFAULT_AMPLITUDE));
    }

    public void showKeyboard(final boolean show) {
        runOnUiThread(() -> {
            InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm == null) return;
            if (show) {
                sink.requestFocus();
                imm.showSoftInput(sink, 0);
            } else {
                imm.hideSoftInputFromWindow(sink.getWindowToken(), 0);
                immersive();
            }
        });
    }

    // --- HTTP for the native side (src/net_http.c on Android: the reports, the suggestions and the updater) -------------
    // Called on the native worker threads (attached to the VM). Follows redirects (GitHub's assets go to its storage);
    // the body goes to outPath (written as .part, then renamed) or comes back in bodyOut[0]. Returns the HTTP status,
    // 0 with errOut[0] when there was no answer. Progress goes to nativeHttpProgress(ctx, done, total) (false cancels).
    static native boolean nativeHttpProgress(long ctx, long done, long total);

    public int httpRequest(String method, String url, String headers, byte[] body, String outPath, int timeoutMs, long ctx,
                           byte[][] bodyOut, String[] errOut) {
        java.net.HttpURLConnection c = null;
        try {
            java.net.URL u = new java.net.URL(url);
            for (int redirects = 0;; redirects++) {
                c = (java.net.HttpURLConnection) u.openConnection();
                c.setConnectTimeout(timeoutMs);
                c.setReadTimeout(timeoutMs);
                c.setInstanceFollowRedirects(false);
                c.setRequestMethod(method);
                if (headers != null)
                    for (String line : headers.split("\r\n")) {
                        int k = line.indexOf(':');
                        if (k > 0) c.setRequestProperty(line.substring(0, k).trim(), line.substring(k + 1).trim());
                    }
                if (body != null && body.length > 0) {
                    c.setDoOutput(true);
                    c.setFixedLengthStreamingMode(body.length);
                    try (OutputStream os = c.getOutputStream()) {
                        os.write(body);
                    }
                }
                int code = c.getResponseCode();
                boolean redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
                if (redirect && redirects < 10) {
                    String loc = c.getHeaderField("Location");
                    c.disconnect();
                    c = null;
                    if (loc == null) return code;
                    u = new java.net.URL(u, loc);
                    if (code == 303) {
                        method = "GET";
                        body = null;
                    }
                    continue;
                }
                InputStream in = code >= 400 ? c.getErrorStream() : c.getInputStream();
                long total = c.getContentLengthLong();
                byte[] buf = new byte[65536];
                long done = 0;
                if (outPath != null) {
                    File part = new File(outPath + ".part");
                    try (FileOutputStream fo = new FileOutputStream(part)) {
                        int n;
                        while (in != null && (n = in.read(buf)) > 0) {
                            fo.write(buf, 0, n);
                            done += n;
                            if (ctx != 0 && !nativeHttpProgress(ctx, done, total)) {
                                errOut[0] = "cancelado";
                                part.delete();
                                return 0;
                            }
                        }
                    }
                    File dst = new File(outPath);
                    dst.delete();
                    if (!part.renameTo(dst)) {
                        errOut[0] = "não foi possível gravar " + outPath;
                        return 0;
                    }
                    return code;
                }
                java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
                int n;
                while (in != null && (n = in.read(buf)) > 0) {
                    bo.write(buf, 0, n);
                    done += n;
                    if (ctx != 0 && !nativeHttpProgress(ctx, done, total)) {
                        errOut[0] = "cancelado";
                        return 0;
                    }
                }
                bodyOut[0] = bo.toByteArray();
                return code;
            }
        } catch (Exception e) {
            errOut[0] = e.getClass().getSimpleName() + ": " + e.getMessage();
            return 0;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    public void openUrl(final String url) {
        runOnUiThread(() -> {
            try {
                startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
            } catch (Exception ignored) {
            }
        });
    }

    public void setClipboard(final String text) {
        runOnUiThread(() -> {
            ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm != null) cm.setPrimaryClip(ClipData.newPlainText("RealEngine", text));
        });
    }

    public String getClipboard() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm == null || !cm.hasPrimaryClip()) return "";
        ClipData c = cm.getPrimaryClip();
        if (c == null || c.getItemCount() == 0) return "";
        CharSequence t = c.getItemAt(0).coerceToText(this);
        return t == null ? "" : t.toString();
    }

}
