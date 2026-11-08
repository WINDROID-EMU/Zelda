package com.dishii.zelda3;

import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * Performs the HTTPS requests requested by the native rc_client (rcheevos).
 * This is called from a native worker thread, so it must block until the
 * response is available.
 */
final class RaHttpBridge {

    private static final String TAG = "Zelda3-RA";
    private static final String USER_AGENT = "Zelda3-Android/1.0 (rcheevos/12.5.0)";
    private static final int CONNECT_TIMEOUT_MS = 15000;
    private static final int READ_TIMEOUT_MS = 20000;

    private RaHttpBridge() {
    }

    /**
     * @return Object[] { Integer httpStatus, byte[] body }. A status of -1
     *         means the request could not be completed locally.
     */
    public static Object[] performHttpRequest(String url, String postData, String contentType) {
        int status = -1;
        byte[] body = new byte[0];
        HttpURLConnection connection = null;

        try {
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(CONNECT_TIMEOUT_MS);
            connection.setReadTimeout(READ_TIMEOUT_MS);
            connection.setRequestProperty("User-Agent", USER_AGENT);
            connection.setRequestProperty("Accept", "application/json");

            if (postData != null) {
                connection.setRequestMethod("POST");
                connection.setDoOutput(true);
                if (contentType != null && !contentType.isEmpty()) {
                    connection.setRequestProperty("Content-Type", contentType);
                }
                byte[] payload = postData.getBytes("UTF-8");
                connection.setFixedLengthStreamingMode(payload.length);
                OutputStream output = connection.getOutputStream();
                try {
                    output.write(payload);
                } finally {
                    output.close();
                }
            } else {
                connection.setRequestMethod("GET");
            }

            status = connection.getResponseCode();
            InputStream input = status >= 400 ? connection.getErrorStream() : connection.getInputStream();
            if (input != null) {
                body = readAll(input);
            }
        } catch (Exception e) {
            Log.w(TAG, "Falha na requisicao RetroAchievements: " + e.getMessage());
            status = -1;
            body = new byte[0];
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }

        return new Object[] { status, body };
    }

    private static byte[] readAll(InputStream input) throws Exception {
        ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        byte[] chunk = new byte[8192];
        int read;
        try {
            while ((read = input.read(chunk)) > 0) {
                buffer.write(chunk, 0, read);
            }
        } finally {
            input.close();
        }
        return buffer.toByteArray();
    }
}
