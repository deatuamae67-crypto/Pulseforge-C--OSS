package org.pulseforge.engine;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.CookieManager;
import java.net.CookiePolicy;
import java.net.HttpURLConnection;
import java.net.URI;
import java.net.URLEncoder;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Native Android HTTPS transport for the Complete content gate.
 *
 * The C++ runtime supplies only manifest-validated Google Drive URLs and
 * app-private staging paths. This bridge follows redirects/cookies, resolves
 * Google's large-file confirmation page, resumes .part files, and atomically
 * promotes a completed transfer. It returns null on success or a diagnostic
 * string on failure so no Java exception crosses the JNI boundary.
 */
public final class CompleteDownloadBridge {
    private static final int MAX_CONFIRMATION_HTML = 4 * 1024 * 1024;
    private static final int BUFFER_BYTES = 256 * 1024;
    private static final int MAX_REDIRECTS = 10;
    private static final int MAX_CONFIRMATION_ROUNDS = 6;
    private static final int MAX_ATTEMPTS = 4;
    private static final String USER_AGENT =
        "Mozilla/5.0 (Linux; Android 16) AppleWebKit/537.36 "
            + "(KHTML, like Gecko) Chrome/139.0.0.0 Mobile Safari/537.36 "
            + "PulseForge/1.0";

    private static final Pattern DIRECT_LINK = Pattern.compile(
        "href=\\\"(/uc\\?export=download[^\\\"]+)\\\"",
        Pattern.CASE_INSENSITIVE
    );
    private static final Pattern DOWNLOAD_FORM = Pattern.compile(
        "<form[^>]*id=\\\"download-form\\\"[^>]*>(.*?)</form>",
        Pattern.CASE_INSENSITIVE | Pattern.DOTALL
    );
    private static final Pattern ATTRIBUTE = Pattern.compile(
        "([A-Za-z0-9_-]+)\\s*=\\s*[\\\"']([^\\\"']*)[\\\"']",
        Pattern.CASE_INSENSITIVE
    );
    private static final Pattern INPUT = Pattern.compile(
        "<input\\b[^>]*>",
        Pattern.CASE_INSENSITIVE
    );
    private static final Pattern DOWNLOAD_URL = Pattern.compile(
        "\\\"downloadUrl\\\":\\\"([^\\\"]+)\\\"",
        Pattern.CASE_INSENSITIVE
    );

    private CompleteDownloadBridge() {}

    /** Returns null on success, otherwise a human-readable diagnostic. */
    public static String download(String rawUrl, String destinationPath) {
        try {
            if (!isAllowedInitialUrl(rawUrl)) {
                return "Complete recusou um URL que não pertence ao Google Drive";
            }
            final File destination = new File(destinationPath).getCanonicalFile();
            final File parent = destination.getParentFile();
            if (parent == null || (!parent.isDirectory() && !parent.mkdirs())) {
                return "não foi possível criar a pasta temporária do mod";
            }

            final CookieManager cookies = new CookieManager(null, CookiePolicy.ACCEPT_ALL);
            final URL resolved = resolveDownloadUrl(new URL(rawUrl), cookies);
            final File part = new File(parent, destination.getName() + ".part");

            IOException lastError = null;
            for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
                try {
                    downloadResolved(resolved, cookies, part);
                    promote(part, destination);
                    return null;
                } catch (IOException error) {
                    lastError = error;
                    if (attempt + 1 < MAX_ATTEMPTS) {
                        try {
                            Thread.sleep(500L * (attempt + 1L));
                        } catch (InterruptedException interrupted) {
                            Thread.currentThread().interrupt();
                            return "transferência Complete interrompida";
                        }
                    }
                }
            }
            return lastError == null
                ? "falha desconhecida ao transferir do Google Drive"
                : lastError.getClass().getSimpleName() + ": " + lastError.getMessage();
        } catch (Exception error) {
            return error.getClass().getSimpleName() + ": " + error.getMessage();
        }
    }

    private static URL resolveDownloadUrl(URL start, CookieManager cookies)
        throws IOException {
        URL current = start;
        for (int round = 0; round < MAX_CONFIRMATION_ROUNDS; ++round) {
            final HttpURLConnection connection = openFollowingRedirects(
                current,
                cookies,
                null
            );
            final int status = connection.getResponseCode();
            if (status >= 400) {
                final String message = "Google Drive devolveu HTTP " + status;
                connection.disconnect();
                throw new IOException(message);
            }
            final String disposition = connection.getHeaderField("Content-Disposition");
            if (disposition != null && !disposition.isEmpty()) {
                final URL resolved = connection.getURL();
                connection.disconnect();
                return resolved;
            }
            final String html = readSmallBody(connection, MAX_CONFIRMATION_HTML);
            final URL base = connection.getURL();
            connection.disconnect();
            final String confirmation = confirmationUrl(html);
            if (confirmation == null || confirmation.isEmpty()) {
                throw new IOException(
                    "Google Drive não forneceu uma resposta de ficheiro transferível"
                );
            }
            current = confirmation.startsWith("https://")
                ? new URL(confirmation)
                : new URL(base, confirmation);
        }
        throw new IOException("o ciclo de confirmação do Google Drive excedeu o limite");
    }

    private static void downloadResolved(
        URL resolved,
        CookieManager cookies,
        File part
    ) throws IOException {
        long resume = part.isFile() ? part.length() : 0L;
        HttpURLConnection connection = openFollowingRedirects(
            resolved,
            cookies,
            resume > 0L ? "bytes=" + resume + "-" : null
        );
        int status = connection.getResponseCode();
        if (status == 416) {
            connection.disconnect();
            if (part.exists() && !part.delete()) {
                throw new IOException("não foi possível reiniciar o download parcial");
            }
            resume = 0L;
            connection = openFollowingRedirects(resolved, cookies, null);
            status = connection.getResponseCode();
        }
        if (status < 200 || status >= 300) {
            connection.disconnect();
            throw new IOException("Google Drive devolveu HTTP " + status);
        }

        final boolean append = resume > 0L
            && status == HttpURLConnection.HTTP_PARTIAL;
        try (InputStream input = connection.getInputStream();
             FileOutputStream output = new FileOutputStream(part, append)) {
            final byte[] buffer = new byte[BUFFER_BYTES];
            int read;
            while ((read = input.read(buffer)) != -1) {
                output.write(buffer, 0, read);
            }
            output.flush();
            output.getFD().sync();
        } finally {
            connection.disconnect();
        }
    }

    private static HttpURLConnection openFollowingRedirects(
        URL start,
        CookieManager cookies,
        String range
    ) throws IOException {
        URL current = start;
        for (int redirect = 0; redirect <= MAX_REDIRECTS; ++redirect) {
            if (!"https".equalsIgnoreCase(current.getProtocol())) {
                throw new IOException("Complete recusou um redirecionamento não HTTPS");
            }
            final HttpURLConnection connection =
                (HttpURLConnection) current.openConnection();
            connection.setInstanceFollowRedirects(false);
            connection.setConnectTimeout(30_000);
            connection.setReadTimeout(60_000);
            connection.setUseCaches(false);
            connection.setRequestProperty("User-Agent", USER_AGENT);
            connection.setRequestProperty("Accept-Encoding", "identity");
            if (range != null) {
                connection.setRequestProperty("Range", range);
            }

            final URI uri = toUri(current);
            final Map<String, List<String>> cookieHeaders = cookies.get(
                uri,
                Collections.emptyMap()
            );
            for (Map.Entry<String, List<String>> entry : cookieHeaders.entrySet()) {
                if (entry.getKey() == null || entry.getValue() == null) continue;
                connection.setRequestProperty(
                    entry.getKey(),
                    joinHeaderValues(entry.getValue())
                );
            }

            final int status = connection.getResponseCode();
            cookies.put(uri, connection.getHeaderFields());
            if (status == HttpURLConnection.HTTP_MOVED_PERM
                || status == HttpURLConnection.HTTP_MOVED_TEMP
                || status == HttpURLConnection.HTTP_SEE_OTHER
                || status == 307 || status == 308) {
                final String location = connection.getHeaderField("Location");
                if (location == null || location.isEmpty()) {
                    connection.disconnect();
                    throw new IOException("redirecionamento Google Drive sem Location");
                }
                final URL next = new URL(current, location);
                connection.disconnect();
                current = next;
                continue;
            }
            return connection;
        }
        throw new IOException("demasiados redirecionamentos Google Drive");
    }

    private static String readSmallBody(HttpURLConnection connection, int maximum)
        throws IOException {
        try (InputStream input = connection.getInputStream();
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            final byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = input.read(buffer)) != -1) {
                if (output.size() > maximum - read) {
                    throw new IOException(
                        "a página de confirmação Google Drive excedeu o limite de segurança"
                    );
                }
                output.write(buffer, 0, read);
            }
            return output.toString(StandardCharsets.UTF_8.name());
        }
    }

    private static String confirmationUrl(String html) {
        Matcher matcher = DIRECT_LINK.matcher(html);
        if (matcher.find()) {
            return "https://docs.google.com" + htmlUnescape(matcher.group(1));
        }

        matcher = DOWNLOAD_FORM.matcher(html);
        if (matcher.find()) {
            final int tagEnd = html.indexOf('>', matcher.start());
            if (tagEnd < 0) return null;
            final String formTag = html.substring(matcher.start(), tagEnd + 1);
            final Map<String, String> formAttributes = attributes(formTag);
            final String action = formAttributes.get("action");
            if (action != null && action.startsWith("https://")) {
                final StringBuilder url = new StringBuilder(htmlUnescape(action));
                boolean first = action.indexOf('?') < 0;
                final Matcher inputs = INPUT.matcher(matcher.group(1));
                while (inputs.find()) {
                    final Map<String, String> values = attributes(inputs.group());
                    final String name = values.get("name");
                    final String value = values.get("value");
                    if (name == null || value == null) continue;
                    url.append(first ? '?' : '&');
                    first = false;
                    url.append(urlEncode(htmlUnescape(name)));
                    url.append('=');
                    url.append(urlEncode(htmlUnescape(value)));
                }
                return url.toString();
            }
        }

        matcher = DOWNLOAD_URL.matcher(html);
        if (matcher.find()) {
            return matcher.group(1)
                .replace("\\u003d", "=")
                .replace("\\u0026", "&")
                .replace("\\u002f", "/")
                .replace("\\/", "/");
        }
        return null;
    }

    private static Map<String, String> attributes(String tag) {
        final java.util.HashMap<String, String> result = new java.util.HashMap<>();
        final Matcher matcher = ATTRIBUTE.matcher(tag);
        while (matcher.find()) {
            result.put(matcher.group(1).toLowerCase(java.util.Locale.ROOT), matcher.group(2));
        }
        return result;
    }

    private static String joinHeaderValues(List<String> values) {
        final StringBuilder output = new StringBuilder();
        for (String value : values) {
            if (value == null) continue;
            if (output.length() != 0) output.append("; ");
            output.append(value);
        }
        return output.toString();
    }

    private static String htmlUnescape(String value) {
        return value
            .replace("&amp;", "&")
            .replace("&quot;", "\"")
            .replace("&#39;", "'")
            .replace("&lt;", "<")
            .replace("&gt;", ">");
    }

    private static String urlEncode(String value) {
        try {
            return URLEncoder.encode(value, "UTF-8").replace("+", "%20");
        } catch (java.io.UnsupportedEncodingException impossible) {
            throw new AssertionError(impossible);
        }
    }

    private static URI toUri(URL url) throws IOException {
        try {
            return url.toURI();
        } catch (java.net.URISyntaxException error) {
            throw new IOException("URL Google Drive inválido", error);
        }
    }

    private static boolean isAllowedInitialUrl(String rawUrl) {
        return rawUrl != null
            && (rawUrl.startsWith("https://drive.google.com/")
                || rawUrl.startsWith("https://drive.usercontent.google.com/"));
    }

    private static void promote(File part, File destination) throws IOException {
        if (!part.isFile()) {
            throw new IOException("o download parcial não existe");
        }
        if (destination.exists() && !destination.delete()) {
            throw new IOException("não foi possível substituir o ficheiro do mod");
        }
        if (!part.renameTo(destination)) {
            throw new IOException("não foi possível finalizar o ficheiro do mod");
        }
    }
}
