package org.pulseforge.engine;

import android.os.Bundle;

import org.libsdl.app.SDLActivity;

import java.io.File;

/** SDL host that gives the native runtime stable, writable content roots. */
public final class PulseForgeActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries) {
            return;
        }

        initializeDiscordSocialSdkIfPresent();

        final File assets = new File(getFilesDir(), "pulseforge/assets");
        File mods = getExternalFilesDir("mods");
        if (mods == null) {
            mods = new File(getFilesDir(), "pulseforge/mods");
        }
        if (!mods.isDirectory()) {
            mods.mkdirs();
        }

        nativeSetenv("PULSEFORGE_ASSET_ROOT", assets.getAbsolutePath());
        nativeSetenv("PULSEFORGE_MOD_ROOT", mods.getAbsolutePath());
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
            android.util.Log.w(
                "PulseForge",
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
