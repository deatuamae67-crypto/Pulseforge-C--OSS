package org.pulseforge.engine;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/**
 * Extracts the read-only APK payload into app-private storage before SDL starts.
 * PulseForge's streaming loaders need ordinary seekable files; doing this on a
 * worker thread also keeps Android's UI thread responsive on first launch.
 */
public final class BootstrapActivity extends Activity {
    private static final String PAYLOAD_ROOT = "pulseforge";
    private static final String PAYLOAD_VERSION = "v8.0.15-mobile-assets-4";
    private static final String[] REQUIRED_PAYLOAD_FILES = {
        "assets/settings.json",
        "assets/demo/chart.json"
    };

    private TextView status;
    private int copiedFiles;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);

        status = new TextView(this);
        status.setBackgroundColor(Color.rgb(3, 7, 10));
        status.setTextColor(Color.rgb(73, 245, 199));
        status.setGravity(Gravity.CENTER);
        status.setTextSize(18.0f);
        status.setPadding(40, 40, 40, 40);
        status.setText("PulseForge\nA preparar os recursos Android...");
        setContentView(status);

        // Android 16's PhoneWindow does not install its DecorView until content
        // is attached. Calling Window#getInsetsController before setContentView
        // can therefore dereference a null DecorView inside the framework.
        enforceBootstrapPresentation();

        new Thread(this::prepareAndLaunch, "PulseForge-asset-bootstrap").start();
    }

    @Override
    protected void onResume() {
        super.onResume();
        enforceBootstrapPresentation();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            enforceBootstrapPresentation();
        }
    }

    @SuppressWarnings("deprecation")
    private void enforceBootstrapPresentation() {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        final Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        window.clearFlags(WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            final WindowManager.LayoutParams attributes = window.getAttributes();
            attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            window.setAttributes(attributes);
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
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

    private void prepareAndLaunch() {
        try {
            final File installRoot = new File(getFilesDir(), PAYLOAD_ROOT);
            final File marker = new File(installRoot, "." + PAYLOAD_VERSION);
            final File settings = new File(installRoot, "assets/settings.json");

            // A previous interrupted install or malformed settings file must not
            // become permanent merely because a bootstrap marker exists. Keep a
            // backup for diagnosis/user recovery, then reinstall the packaged
            // default settings instead of repeatedly crashing the native runtime.
            if (settings.isFile() && !isValidJsonObject(settings)) {
                quarantineCorruptSettings(settings);
            }

            if (!marker.isFile() || !payloadLooksComplete(installRoot)) {
                copyTree(getAssets(), PAYLOAD_ROOT, getFilesDir(), getFilesDir());
                if (!payloadLooksComplete(installRoot)) {
                    throw new IOException("os recursos Android ficaram incompletos após a instalação");
                }
                writeAtomically(marker, PAYLOAD_VERSION.getBytes(StandardCharsets.UTF_8));
            }

            runOnUiThread(() -> {
                startActivity(new Intent(this, PulseForgeActivity.class));
                finish();
            });
        } catch (Exception error) {
            runOnUiThread(() -> {
                status.setText(
                    "Não foi possível preparar o PulseForge.\n\n"
                        + error.getClass().getSimpleName() + ": " + error.getMessage()
                        + "\n\nToque para tentar novamente."
                );
                status.setOnClickListener(this::retry);
            });
        }
    }

    private void retry(View ignored) {
        status.setOnClickListener(null);
        status.setText("PulseForge\nA tentar novamente...");
        copiedFiles = 0;
        new Thread(this::prepareAndLaunch, "PulseForge-asset-bootstrap-retry").start();
    }

    private static boolean payloadLooksComplete(File installRoot) {
        for (String relativePath : REQUIRED_PAYLOAD_FILES) {
            final File file = new File(installRoot, relativePath);
            if (!file.isFile() || file.length() <= 0) {
                return false;
            }
        }
        return isValidJsonObject(new File(installRoot, "assets/settings.json"));
    }

    private static boolean isValidJsonObject(File file) {
        if (!file.isFile() || file.length() <= 0) {
            return false;
        }
        try (BufferedReader reader = new BufferedReader(
            new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8)
        )) {
            final StringBuilder json = new StringBuilder();
            final char[] buffer = new char[8192];
            int read;
            while ((read = reader.read(buffer)) != -1) {
                json.append(buffer, 0, read);
            }
            new JSONObject(json.toString());
            return true;
        } catch (Exception ignored) {
            return false;
        }
    }

    private static void quarantineCorruptSettings(File settings) throws IOException {
        final File parent = settings.getParentFile();
        if (parent == null) {
            throw new IOException("settings.json não tem diretório pai");
        }
        final File backup = new File(
            parent,
            "settings.corrupt-" + System.currentTimeMillis() + ".json"
        );
        if (!settings.renameTo(backup)) {
            throw new IOException("não foi possível guardar uma cópia do settings.json corrompido");
        }
    }

    private void copyTree(
        AssetManager assets,
        String assetPath,
        File outputBase,
        File securityRoot
    ) throws IOException {
        final String[] children = assets.list(assetPath);
        if (children != null && children.length != 0) {
            final File directory = checkedDestination(outputBase, securityRoot, assetPath);
            if (!directory.isDirectory() && !directory.mkdirs()) {
                throw new IOException("não foi possível criar " + directory);
            }
            for (String child : children) {
                copyTree(assets, assetPath + "/" + child, outputBase, securityRoot);
            }
            return;
        }

        final File destination = checkedDestination(outputBase, securityRoot, assetPath);
        // Settings are user data after first launch; engine upgrades must not
        // silently reset valid keybinds, volume, latency offsets or visual options.
        if (assetPath.endsWith("/assets/settings.json")
            && destination.isFile()
            && destination.length() > 0
            && isValidJsonObject(destination)) {
            return;
        }
        final File parent = destination.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs())) {
            throw new IOException("não foi possível criar " + parent);
        }
        final File temporary = new File(parent, destination.getName() + ".tmp");
        try (InputStream input = assets.open(assetPath);
             FileOutputStream output = new FileOutputStream(temporary, false)) {
            final byte[] buffer = new byte[256 * 1024];
            int read;
            while ((read = input.read(buffer)) != -1) {
                output.write(buffer, 0, read);
            }
            output.flush();
            output.getFD().sync();
        }
        if (destination.exists() && !destination.delete()) {
            throw new IOException("não foi possível substituir " + destination);
        }
        if (!temporary.renameTo(destination)) {
            throw new IOException("não foi possível instalar " + destination);
        }
        ++copiedFiles;
        if ((copiedFiles % 32) == 0) {
            runOnUiThread(() -> status.setText(
                "PulseForge\nA preparar os recursos Android...\n"
                    + copiedFiles + " ficheiros"
            ));
        }
    }

    private static File checkedDestination(
        File outputBase,
        File securityRoot,
        String assetPath
    ) throws IOException {
        final File destination = new File(outputBase, assetPath).getCanonicalFile();
        final String root = securityRoot.getCanonicalPath() + File.separator;
        if (!destination.getPath().startsWith(root)) {
            throw new IOException("caminho de recurso inválido");
        }
        return destination;
    }

    private static void writeAtomically(File destination, byte[] data) throws IOException {
        final File parent = destination.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs())) {
            throw new IOException("não foi possível criar " + parent);
        }
        final File temporary = new File(parent, destination.getName() + ".tmp");
        try (FileOutputStream output = new FileOutputStream(temporary, false)) {
            output.write(data);
            output.flush();
            output.getFD().sync();
        }
        if (destination.exists() && !destination.delete()) {
            throw new IOException("não foi possível atualizar " + destination);
        }
        if (!temporary.renameTo(destination)) {
            throw new IOException("não foi possível finalizar " + destination);
        }
    }
}
