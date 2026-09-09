package org.pulseforge.engine;

import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.os.Build;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;

/** SDL host that gives the native runtime stable, writable content roots. */
public final class PulseForgeActivity extends SDLActivity {
    private static final String TAG = "PulseForge";

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
}
