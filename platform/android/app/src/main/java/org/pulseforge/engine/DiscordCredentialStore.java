package org.pulseforge.engine;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import android.util.Log;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * Stores only the Discord OAuth refresh token. Ciphertext lives in private
 * SharedPreferences; the AES key is non-exportable in AndroidKeyStore.
 * This class is loaded only when Discord account persistence is actually used.
 */
final class DiscordCredentialStore {
    private static final String TAG = "PulseForge";
    private static final String PREFS = "pulseforge_discord_secure_v1";
    private static final String KEYSTORE = "AndroidKeyStore";
    private static final String TRANSFORMATION = "AES/GCM/NoPadding";
    private static final int GCM_TAG_BITS = 128;

    private DiscordCredentialStore() {}

    static boolean available() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.M;
    }

    static boolean store(Context context, String applicationId, String refreshToken) {
        if (!available() || context == null || applicationId == null
            || applicationId.isEmpty() || refreshToken == null || refreshToken.isEmpty()) {
            return false;
        }
        try {
            final SecretKey key = getOrCreateKey(applicationId);
            final Cipher cipher = Cipher.getInstance(TRANSFORMATION);
            cipher.init(Cipher.ENCRYPT_MODE, key);
            final byte[] iv = cipher.getIV();
            final byte[] ciphertext = cipher.doFinal(
                refreshToken.getBytes(StandardCharsets.UTF_8)
            );
            final ByteBuffer packed = ByteBuffer.allocate(
                4 + iv.length + ciphertext.length
            );
            packed.putInt(iv.length);
            packed.put(iv);
            packed.put(ciphertext);
            return preferences(context).edit()
                .putString(applicationId, Base64.encodeToString(packed.array(), Base64.NO_WRAP))
                .commit();
        } catch (Exception error) {
            Log.w(TAG, "Could not persist the Discord refresh token", error);
            return false;
        }
    }

    static String load(Context context, String applicationId) {
        if (!available() || context == null || applicationId == null
            || applicationId.isEmpty()) {
            return null;
        }
        final String encoded = preferences(context).getString(applicationId, null);
        if (encoded == null || encoded.isEmpty()) {
            return null;
        }
        try {
            final byte[] packedBytes = Base64.decode(encoded, Base64.NO_WRAP);
            final ByteBuffer packed = ByteBuffer.wrap(packedBytes);
            if (packed.remaining() < 4) {
                erase(context, applicationId);
                return null;
            }
            final int ivBytes = packed.getInt();
            if (ivBytes < 12 || ivBytes > 32 || packed.remaining() <= ivBytes) {
                erase(context, applicationId);
                return null;
            }
            final byte[] iv = new byte[ivBytes];
            packed.get(iv);
            final byte[] ciphertext = new byte[packed.remaining()];
            packed.get(ciphertext);

            final KeyStore store = KeyStore.getInstance(KEYSTORE);
            store.load(null);
            final java.security.Key key = store.getKey(alias(applicationId), null);
            if (!(key instanceof SecretKey)) {
                erase(context, applicationId);
                return null;
            }
            final Cipher cipher = Cipher.getInstance(TRANSFORMATION);
            cipher.init(
                Cipher.DECRYPT_MODE,
                (SecretKey) key,
                new GCMParameterSpec(GCM_TAG_BITS, iv)
            );
            final byte[] plaintext = cipher.doFinal(ciphertext);
            return new String(plaintext, StandardCharsets.UTF_8);
        } catch (Exception error) {
            Log.w(TAG, "Could not restore the Discord refresh token", error);
            return null;
        }
    }

    static boolean erase(Context context, String applicationId) {
        if (context == null || applicationId == null || applicationId.isEmpty()) {
            return true;
        }
        boolean ok = true;
        try {
            if (!preferences(context).edit().remove(applicationId).commit()) {
                ok = false;
            }
        } catch (Exception error) {
            Log.w(TAG, "Could not remove Discord credential ciphertext", error);
            ok = false;
        }
        if (!available()) {
            return ok;
        }
        try {
            final KeyStore store = KeyStore.getInstance(KEYSTORE);
            store.load(null);
            final String alias = alias(applicationId);
            if (store.containsAlias(alias)) {
                store.deleteEntry(alias);
            }
        } catch (Exception error) {
            Log.w(TAG, "Could not remove Discord credential key", error);
            ok = false;
        }
        return ok;
    }

    private static SharedPreferences preferences(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private static String alias(String applicationId) {
        return "pulseforge.discord.refresh." + applicationId;
    }

    private static SecretKey getOrCreateKey(String applicationId) throws Exception {
        final KeyStore store = KeyStore.getInstance(KEYSTORE);
        store.load(null);
        final String alias = alias(applicationId);
        final java.security.Key existing = store.getKey(alias, null);
        if (existing instanceof SecretKey) {
            return (SecretKey) existing;
        }

        final KeyGenerator generator = KeyGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_AES,
            KEYSTORE
        );
        generator.init(
            new KeyGenParameterSpec.Builder(
                alias,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true)
                .build()
        );
        return generator.generateKey();
    }
}
