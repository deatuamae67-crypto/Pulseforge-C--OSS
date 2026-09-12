package org.pulseforge.engine;

import android.content.ContentValues;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Debug;
import android.os.Environment;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import com.arthenica.ffmpegkit.FFmpegKit;
import com.arthenica.ffmpegkit.FFmpegKitConfig;
import com.arthenica.ffmpegkit.FFmpegSession;
import com.arthenica.ffmpegkit.ReturnCode;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.ConcurrentHashMap;

/** SDL host that gives the native runtime stable, writable content roots. */
public final class PulseForgeActivity extends SDLActivity {
    private static final String TAG = "PulseForge";
    private static final long STALE_ARCHIVE_IMPORT_MS = 24L * 60L * 60L * 1000L;
    private final ConcurrentHashMap<Long, FFmpegSession> pulseForgeFfmpegSessions =
        new ConcurrentHashMap<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // PulseForge is a landscape-only game. SENSOR_LANDSCAPE allows the two
        // landscape rotations (0/180 relative to one another) while refusing a
        // 90-degree portrait surface that would make the 1280x720 renderer tiny.
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);

        // SDLActivity can start the native thread from super.onCreate(). The
        // native entry point needs these paths before that can happen; setting
        // them afterwards creates a race where locate_assets() falls back to an
        // unusable Android working directory and the process exits immediately.
        final File assets = new File(getFilesDir(), "pulseforge/assets");
        File mods = getExternalFilesDir("mods");
        if (mods == null) {
            mods = new File(getFilesDir(), "pulseforge/mods");
        }
        if (!mods.isDirectory() && !mods.mkdirs() && !mods.isDirectory()) {
            Log.e(TAG, "Unable to create Android mod root: " + mods);
        }

        setProcessEnvironment("PULSEFORGE_ASSET_ROOT", assets.getAbsolutePath());
        setProcessEnvironment("PULSEFORGE_MOD_ROOT", mods.getAbsolutePath());
        cleanupStaleArchiveImports();

        super.onCreate(savedInstanceState);
        if (mBrokenLibraries) {
            return;
        }

        // Synchronize SDL's native environment as well. The process-level
        // environment above is the startup-critical path; these calls retain
        // compatibility with SDL's own environment handling after libraries load.
        nativeSetenv("PULSEFORGE_ASSET_ROOT", assets.getAbsolutePath());
        nativeSetenv("PULSEFORGE_MOD_ROOT", mods.getAbsolutePath());

        enforceGamePresentation();
        initializeDiscordSocialSdkIfPresent();
    }

    private static void setProcessEnvironment(String name, String value) {
        try {
            Os.setenv(name, value, true);
        } catch (ErrnoException exception) {
            Log.e(TAG, "Unable to set Android process environment variable " + name, exception);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        // SDL3 deliberately returns content:// URIs for Android file dialogs.
        // PulseForge's cross-platform importers consume ordinary filesystem
        // paths, so bridge selected files into app-owned storage before SDL's
        // callback forwards the selection to native code. Folder/tree URIs are
        // intentionally left untouched; this bridge is only for file selections.
        if (resultCode == RESULT_OK && data != null) {
            materializeSelectedContentFile(data);
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    private void materializeSelectedContentFile(Intent data) {
        Uri uri = data.getData();
        if (uri == null && data.getClipData() != null
            && data.getClipData().getItemCount() == 1) {
            uri = data.getClipData().getItemAt(0).getUri();
        }
        if (uri == null || !"content".equalsIgnoreCase(uri.getScheme())) {
            return;
        }

        final String uriText = uri.toString();
        if (uriText.contains("/tree/")) {
            return;
        }

        final String displayName = queryDisplayName(uri);
        final String safeName = sanitizeSelectionName(displayName);
        final boolean archive = isModArchiveName(safeName);

        File root;
        if (archive) {
            root = getExternalCacheDir();
            if (root == null) {
                root = getCacheDir();
            }
            root = new File(root, "pulseforge-file-dialog-archives");
        } else {
            root = getExternalFilesDir("imports");
            if (root == null) {
                root = new File(getFilesDir(), "pulseforge/imports");
            }
        }
        if (!root.isDirectory() && !root.mkdirs() && !root.isDirectory()) {
            Log.e(TAG, "Unable to create Android file-dialog bridge root: " + root);
            return;
        }

        final File target = new File(
            root,
            Long.toUnsignedString(System.nanoTime()) + "-" + safeName
        );
        try (
            InputStream input = getContentResolver().openInputStream(uri);
            FileOutputStream output = new FileOutputStream(target)
        ) {
            if (input == null) {
                Log.e(TAG, "ContentResolver returned no stream for selected URI: " + uri);
                return;
            }
            final byte[] buffer = new byte[256 * 1024];
            int count;
            while ((count = input.read(buffer)) >= 0) {
                if (count != 0) {
                    output.write(buffer, 0, count);
                }
            }
            output.flush();
        } catch (IOException | SecurityException exception) {
            if (!target.delete() && target.exists()) {
                Log.w(TAG, "Unable to remove failed Android dialog bridge file: " + target);
            }
            Log.e(TAG, "Unable to materialize selected Android content URI", exception);
            return;
        }

        // SDLActivity forwards Intent.getData().toString() to native code. A
        // scheme-less Uri created from the absolute path therefore becomes the
        // normal filesystem path expected by std::filesystem and install_mod().
        data.setData(Uri.parse(target.getAbsolutePath()));
        Log.i(TAG, "Materialized Android file-dialog selection to " + target);
    }

    private String queryDisplayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(
            uri,
            new String[]{OpenableColumns.DISPLAY_NAME},
            null,
            null,
            null
        )) {
            if (cursor != null && cursor.moveToFirst()) {
                final int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) {
                    final String value = cursor.getString(index);
                    if (value != null && !value.isEmpty()) {
                        return value;
                    }
                }
            }
        } catch (RuntimeException exception) {
            Log.w(TAG, "Unable to query Android file-dialog display name", exception);
        }
        final String fallback = uri.getLastPathSegment();
        return fallback == null || fallback.isEmpty() ? "selected-file" : fallback;
    }

    private static String sanitizeSelectionName(String value) {
        if (value == null || value.isEmpty()) {
            return "selected-file";
        }
        final StringBuilder result = new StringBuilder(Math.min(value.length(), 180));
        for (int index = 0; index < value.length() && result.length() < 180; ++index) {
            final char character = value.charAt(index);
            final boolean allowed = (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '.' || character == '-' || character == '_';
            result.append(allowed ? character : '_');
        }
        while (result.length() > 0
            && (result.charAt(0) == '.' || result.charAt(0) == ' ')) {
            result.deleteCharAt(0);
        }
        return result.length() == 0 ? "selected-file" : result.toString();
    }

    private static boolean isModArchiveName(String name) {
        final String lower = name.toLowerCase(java.util.Locale.ROOT);
        return lower.endsWith(".zip") || lower.endsWith(".7z")
            || lower.endsWith(".rar") || lower.endsWith(".tar");
    }

    private void cleanupStaleArchiveImports() {
        File root = getExternalCacheDir();
        if (root == null) {
            root = getCacheDir();
        }
        final File directory = new File(root, "pulseforge-file-dialog-archives");
        final File[] entries = directory.listFiles();
        if (entries == null) {
            return;
        }
        final long cutoff = System.currentTimeMillis() - STALE_ARCHIVE_IMPORT_MS;
        for (File entry : entries) {
            if (entry.isFile() && entry.lastModified() < cutoff
                && !entry.delete() && entry.exists()) {
                Log.w(TAG, "Unable to remove stale Android archive bridge file: " + entry);
            }
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        enforceGamePresentation();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        // Android can recreate the Surface/insets while preserving this
        // Activity. Reassert the contract immediately so SDL never receives a
        // portrait-sized or system-bar-constrained gameplay surface.
        enforceGamePresentation();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            enforceGamePresentation();
        }
    }

    @SuppressWarnings("deprecation")
    private void enforceGamePresentation() {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);

        final Window window = getWindow();
        window.addFlags(
            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                | WindowManager.LayoutParams.FLAG_FULLSCREEN
        );
        window.clearFlags(WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            final WindowManager.LayoutParams attributes = window.getAttributes();
            attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            window.setAttributes(attributes);
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // Give SDL the complete physical display surface. Letterboxing to
            // PulseForge's 1280x720 logical space is then performed exactly once
            // by SDL instead of once by Android and again by the engine.
            window.setDecorFitsSystemWindows(false);
            final WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(
                    WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars()
                );
                controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                );
            }
            return;
        }

        window.getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        );
    }

    /** Initialize the optional Discord Social SDK without making no-SDK builds depend on its AAR. */
    private void initializeDiscordSocialSdkIfPresent() {
        try {
            final Class<?> initClass = Class.forName(
                "com.discord.socialsdk.DiscordSocialSdkInit"
            );
            initClass.getMethod("setEngineActivity", android.app.Activity.class)
                .invoke(null, this);
        } catch (ClassNotFoundException ignored) {
            // Discord Social SDK is optional; Rich Presence remains fail-open.
        } catch (ReflectiveOperationException exception) {
            Log.w(
                TAG,
                "Discord Social SDK activity initialization failed",
                exception
            );
        }
    }

    /** Called by the native Discord service; the refresh token never enters settings.json. */
    public String loadDiscordRefreshToken(String applicationId) {
        return DiscordCredentialStore.load(this, applicationId);
    }

    public boolean storeDiscordRefreshToken(String applicationId, String refreshToken) {
        return DiscordCredentialStore.store(this, applicationId, refreshToken);
    }

    public boolean eraseDiscordRefreshToken(String applicationId) {
        return DiscordCredentialStore.erase(this, applicationId);
    }

    public String getPulseForgeRuntimeStats() {
        try {
            final Runtime runtime = Runtime.getRuntime();
            final StringBuilder out = new StringBuilder();
            out.append("android.manufacturer=").append(Build.MANUFACTURER).append('\n');
            out.append("android.model=").append(Build.MODEL).append('\n');
            out.append("android.device=").append(Build.DEVICE).append('\n');
            out.append("android.sdk=").append(Build.VERSION.SDK_INT).append('\n');
            out.append("java.heap.total_bytes=").append(runtime.totalMemory()).append('\n');
            out.append("java.heap.free_bytes=").append(runtime.freeMemory()).append('\n');
            out.append("java.heap.max_bytes=").append(runtime.maxMemory()).append('\n');
            out.append("native.heap.allocated_bytes=").append(Debug.getNativeHeapAllocatedSize()).append('\n');
            out.append("native.heap.free_bytes=").append(Debug.getNativeHeapFreeSize()).append('\n');
            out.append("native.heap.size_bytes=").append(Debug.getNativeHeapSize()).append('\n');
            out.append("process.pss_kib=").append(Debug.getPss()).append('\n');
            if (Build.VERSION.SDK_INT >= 23) {
                final Map<String, String> runtimeStats = new TreeMap<>(Debug.getRuntimeStats());
                for (final Map.Entry<String, String> entry : runtimeStats.entrySet()) {
                    if (entry.getKey().startsWith("art.gc.")
                            || entry.getKey().startsWith("art.gc-")) {
                        out.append(entry.getKey()).append('=').append(entry.getValue()).append('\n');
                    }
                }
            }
            return out.toString();
        } catch (final Throwable throwable) {
            return "android.runtime_stats_error=" + throwable + "\n";
        }
    }

    public String publishPulseForgeDownload(
            final String sourcePath,
            final String displayName,
            final String mimeType) {
        if (sourcePath == null || displayName == null || displayName.isEmpty()) {
            return null;
        }
        final File source = new File(sourcePath);
        if (!source.isFile()) {
            return null;
        }
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                final ContentValues values = new ContentValues();
                values.put(MediaStore.Downloads.DISPLAY_NAME, displayName);
                values.put(MediaStore.Downloads.MIME_TYPE,
                    mimeType == null || mimeType.isEmpty() ? "application/octet-stream" : mimeType);
                values.put(MediaStore.Downloads.RELATIVE_PATH,
                    Environment.DIRECTORY_DOWNLOADS + "/PulseForge");
                values.put(MediaStore.Downloads.IS_PENDING, 1);
                final Uri uri = getContentResolver().insert(
                    MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
                if (uri == null) return null;
                boolean complete = false;
                try (InputStream input = new FileInputStream(source);
                     OutputStream output = getContentResolver().openOutputStream(uri, "w")) {
                    if (output == null) return null;
                    final byte[] buffer = new byte[64 * 1024];
                    int count;
                    while ((count = input.read(buffer)) >= 0) {
                        if (count != 0) output.write(buffer, 0, count);
                    }
                    output.flush();
                    complete = true;
                } finally {
                    if (!complete) getContentResolver().delete(uri, null, null);
                }
                values.clear();
                values.put(MediaStore.Downloads.IS_PENDING, 0);
                getContentResolver().update(uri, values, null, null);
                return uri.toString();
            }

            final File downloads = new File(
                Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
                "PulseForge");
            if (!downloads.exists() && !downloads.mkdirs()) return null;
            final File target = new File(downloads, displayName);
            try (InputStream input = new FileInputStream(source);
                 OutputStream output = new FileOutputStream(target, false)) {
                final byte[] buffer = new byte[64 * 1024];
                int count;
                while ((count = input.read(buffer)) >= 0) {
                    if (count != 0) output.write(buffer, 0, count);
                }
                output.flush();
            }
            return target.getAbsolutePath();
        } catch (final Throwable throwable) {
            Log.e(TAG, "Downloads export failed", throwable);
            return null;
        }
    }

    public String createPulseForgeFfmpegPipe() {
        try {
            return FFmpegKitConfig.registerNewFFmpegPipe(this);
        } catch (final Throwable throwable) {
            Log.e(TAG, "FFmpeg pipe creation failed", throwable);
            return null;
        }
    }

    public long startPulseForgeFfmpeg(final String[] arguments) {
        try {
            final FFmpegSession session = FFmpegKit.executeWithArgumentsAsync(
                arguments,
                completed -> { }
            );
            final long id = session.getSessionId();
            pulseForgeFfmpegSessions.put(id, session);
            return id;
        } catch (final Throwable throwable) {
            Log.e(TAG, "FFmpeg session start failed", throwable);
            return -1L;
        }
    }

    public int waitPulseForgeFfmpeg(final long sessionId) {
        final FFmpegSession session = pulseForgeFfmpegSessions.get(sessionId);
        if (session == null) return -32000;
        try {
            while (session.getReturnCode() == null) {
                final String state = String.valueOf(session.getState());
                if ("FAILED".equals(state)) return -32001;
                Thread.sleep(10L);
            }
            final ReturnCode code = session.getReturnCode();
            return code == null ? -32002 : code.getValue();
        } catch (final InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return -32003;
        } catch (final Throwable throwable) {
            Log.e(TAG, "FFmpeg wait failed", throwable);
            return -32004;
        }
    }

    public String getPulseForgeFfmpegOutput(final long sessionId) {
        final FFmpegSession session = pulseForgeFfmpegSessions.get(sessionId);
        if (session == null) return "";
        try {
            final String output = session.getOutput();
            return output == null ? "" : output;
        } catch (final Throwable throwable) {
            return "FFmpegKit output error: " + throwable;
        }
    }

    public void cancelPulseForgeFfmpeg(final long sessionId) {
        try {
            FFmpegKit.cancel(sessionId);
        } catch (final Throwable throwable) {
            Log.w(TAG, "FFmpeg cancel failed", throwable);
        }
    }

    public void closePulseForgeFfmpegPipe(final String path) {
        if (path == null || path.isEmpty()) return;
        try {
            FFmpegKitConfig.closeFFmpegPipe(path);
        } catch (final Throwable throwable) {
            Log.w(TAG, "FFmpeg pipe close failed", throwable);
        }
    }

}
