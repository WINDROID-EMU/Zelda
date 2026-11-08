package com.dishii.zelda3;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.lang.ref.WeakReference;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;

/**
 * Single source of truth for the RetroAchievements UI. Native callbacks are
 * always forwarded to the main thread; persistence only keeps the username and
 * the login token (never the password).
 */
public final class RetroAchievementsClient {

    private static final String TAG = "Zelda3-RA";
    private static final String PREFS = "retroachievements";

    public static final String DEFAULT_GAME_HASH = "608c22b8ff930c62dc2de54bcd6eba72";

    public static final int STATE_DISABLED = 0;
    public static final int STATE_LOGGED_OUT = 1;
    public static final int STATE_LOGGING_IN = 2;
    public static final int STATE_LOGGED_IN = 3;
    public static final int STATE_LOADING_GAME = 4;
    public static final int STATE_READY = 5;
    public static final int STATE_ERROR = 6;

    public interface Listener {
        void onRetroAchievementsChanged();
    }

    public static final class Achievement {
        public final int id;
        public final String title;
        public final String description;
        public final int points;
        public final boolean unlocked;
        public final String progress;
        public final int bucket;

        Achievement(int id, String title, String description, int points, boolean unlocked,
                    String progress, int bucket) {
            this.id = id;
            this.title = title;
            this.description = description;
            this.points = points;
            this.unlocked = unlocked;
            this.progress = progress;
            this.bucket = bucket;
        }
    }

    public static final class Unlock {
        public final String title;
        public final String description;
        public final int points;
        public final int unlocked;
        public final int total;
        public final int pointsUnlocked;
        public final int pointsTotal;

        Unlock(String title, String description, int points, int unlocked, int total,
               int pointsUnlocked, int pointsTotal) {
            this.title = title;
            this.description = description;
            this.points = points;
            this.unlocked = unlocked;
            this.total = total;
            this.pointsUnlocked = pointsUnlocked;
            this.pointsTotal = pointsTotal;
        }
    }

    private static RetroAchievementsClient sInstance;

    private final Context context;
    private final SharedPreferences prefs;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final List<Listener> listeners = new ArrayList<>();
    private final ArrayDeque<Unlock> pendingUnlocks = new ArrayDeque<>();

    private WeakReference<MainActivity> activityRef;
    private boolean nativeAvailable = true;
    private boolean dialogShowing;

    private boolean enabled;
    private String username;
    private String token;
    private String displayName;
    private String gameHash;

    private int state = STATE_DISABLED;
    private int score;
    private String gameTitle = "";
    private int unlockedCount;
    private int totalCount;
    private int pointsUnlocked;
    private int pointsTotal;
    private String error = "";

    private RetroAchievementsClient(Context context) {
        this.context = context;
        this.prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        this.enabled = prefs.getBoolean("enabled", false);
        this.username = prefs.getString("username", "");
        this.token = prefs.getString("token", "");
        this.displayName = prefs.getString("display_name", "");
        this.gameHash = prefs.getString("hash", DEFAULT_GAME_HASH);
        if (this.gameHash == null || this.gameHash.trim().isEmpty()) {
            this.gameHash = DEFAULT_GAME_HASH;
        }
    }

    public static synchronized void init(Context context) {
        if (sInstance == null) {
            sInstance = new RetroAchievementsClient(context.getApplicationContext());
        }
        sInstance.configureNative();
    }

    public static RetroAchievementsClient get() {
        return sInstance;
    }

    public static int getUnlockedCountStatic() {
        return sInstance != null ? sInstance.unlockedCount : 0;
    }

    public static int getTotalCountStatic() {
        return sInstance != null ? sInstance.totalCount : 0;
    }

    private void configureNative() {
        if (!nativeAvailable) return;
        try {
            MainActivity.nativeRaConfigure(enabled, username, token, gameHash);
        } catch (UnsatisfiedLinkError | Exception e) {
            nativeAvailable = false;
            Log.w(TAG, "Biblioteca nativa do RetroAchievements indisponivel: " + e.getMessage());
        }
        refreshFromNative();
    }

    public void login(String user, String password) {
        final String cleanUser = user == null ? "" : user.trim();
        if (cleanUser.isEmpty() || password == null || password.isEmpty()) {
            error = "Informe usuario e senha.";
            notifyListeners();
            return;
        }

        username = cleanUser;
        enabled = true;
        state = STATE_LOGGING_IN;
        error = "";
        prefs.edit()
                .putString("username", cleanUser)
                .putBoolean("enabled", true)
                .apply();

        try {
            MainActivity.nativeRaLogin(cleanUser, password);
        } catch (UnsatisfiedLinkError | Exception e) {
            nativeAvailable = false;
            state = STATE_ERROR;
            error = "Integracao nativa indisponivel.";
            Log.w(TAG, "Falha ao chamar nativeRaLogin: " + e.getMessage());
        }
        notifyListeners();
    }

    public void logout() {
        username = "";
        token = "";
        displayName = "";
        score = 0;
        enabled = false;
        state = STATE_LOGGED_OUT;
        error = "";
        prefs.edit()
                .remove("token")
                .remove("display_name")
                .putString("username", "")
                .putBoolean("enabled", false)
                .apply();

        try {
            MainActivity.nativeRaLogout();
        } catch (UnsatisfiedLinkError | Exception e) {
            Log.w(TAG, "Falha ao chamar nativeRaLogout: " + e.getMessage());
        }
        notifyListeners();
    }

    public void setEnabled(boolean value) {
        enabled = value;
        prefs.edit().putBoolean("enabled", value).apply();
        configureNative();
        notifyListeners();
    }

    public void setGameHash(String hash) {
        String cleanHash = hash == null ? "" : hash.trim();
        if (cleanHash.isEmpty()) cleanHash = DEFAULT_GAME_HASH;
        gameHash = cleanHash;
        prefs.edit().putString("hash", cleanHash).apply();
        configureNative();
        notifyListeners();
    }

    public void refreshFromNative() {
        if (!nativeAvailable) {
            notifyListeners();
            return;
        }
        try {
            String[] status = MainActivity.nativeRaGetStatus();
            if (status != null && status.length >= 9) {
                state = parseInt(status[0], STATE_DISABLED);
                String nativeUser = status[1] == null ? "" : status[1];
                if (!nativeUser.isEmpty()) username = nativeUser;
                score = parseInt(status[2], 0);
                gameTitle = status[3] == null ? "" : status[3];
                unlockedCount = parseInt(status[4], 0);
                totalCount = parseInt(status[5], 0);
                pointsUnlocked = parseInt(status[6], 0);
                pointsTotal = parseInt(status[7], 0);
                error = status[8] == null ? "" : status[8];
            }
        } catch (UnsatisfiedLinkError | Exception e) {
            nativeAvailable = false;
            Log.w(TAG, "Falha ao consultar status nativo: " + e.getMessage());
        }
        notifyListeners();
    }

    public List<Achievement> getAchievements() {
        List<Achievement> result = new ArrayList<>();
        if (!nativeAvailable) return result;

        try {
            String[] entries = MainActivity.nativeRaGetAchievements();
            if (entries == null) return result;
            for (String entry : entries) {
                if (entry == null) continue;
                String[] parts = entry.split("\u001f", -1);
                if (parts.length < 7) continue;
                result.add(new Achievement(
                        parseInt(parts[0], 0),
                        parts[1],
                        parts[2],
                        parseInt(parts[3], 0),
                        parseInt(parts[4], 0) != 0,
                        parts[5],
                        parseInt(parts[6], 0)));
            }
        } catch (UnsatisfiedLinkError | Exception e) {
            Log.w(TAG, "Falha ao listar conquistas: " + e.getMessage());
        }
        return result;
    }

    public void attachActivity(MainActivity activity) {
        activityRef = new WeakReference<>(activity);
        dispatchNextUnlock();
    }

    public void detachActivity(MainActivity activity) {
        MainActivity current = activityRef != null ? activityRef.get() : null;
        if (current == activity) {
            activityRef = null;
            if (activity.isFinishing()) dialogShowing = false;
        }
    }

    public void addListener(Listener listener) {
        if (listener != null && !listeners.contains(listener)) listeners.add(listener);
    }

    public void removeListener(Listener listener) {
        listeners.remove(listener);
    }

    private void notifyListeners() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            mainHandler.post(this::notifyListeners);
            return;
        }
        for (Listener listener : new ArrayList<>(listeners)) {
            listener.onRetroAchievementsChanged();
        }
    }

    private void dispatchNextUnlock() {
        if (dialogShowing) return;
        final MainActivity activity = activityRef != null ? activityRef.get() : null;
        if (activity == null || activity.isFinishing()) return;

        final Unlock unlock = pendingUnlocks.poll();
        if (unlock == null) return;

        dialogShowing = true;
        activity.showAchievementUnlocked(unlock, new Runnable() {
            @Override
            public void run() {
                dialogShowing = false;
                dispatchNextUnlock();
            }
        });
    }

    /* ------------------------- native callbacks ------------------------- */

    public static void onRaLoginResult(boolean success, String user, String newToken,
                                      String display, String message) {
        final RetroAchievementsClient client = sInstance;
        if (client == null) return;
        client.mainHandler.post(() -> client.handleLoginResult(success, user, newToken, display, message));
    }

    public static void onRaGameLoaded(String title, int gameId, int unlocked, int total,
                                      int unlockedPoints, int totalPoints) {
        final RetroAchievementsClient client = sInstance;
        if (client == null) return;
        client.mainHandler.post(() -> client.handleGameLoaded(title, gameId, unlocked, total,
                unlockedPoints, totalPoints));
    }

    public static void onRaGameError(String message) {
        final RetroAchievementsClient client = sInstance;
        if (client == null) return;
        client.mainHandler.post(() -> client.handleGameError(message));
    }

    public static void onRaAchievementUnlocked(String title, String description, int points,
                                               int unlocked, int total, int unlockedPoints,
                                               int totalPoints) {
        final RetroAchievementsClient client = sInstance;
        if (client == null) return;
        client.mainHandler.post(() -> client.handleUnlock(title, description, points, unlocked,
                total, unlockedPoints, totalPoints));
    }

    private void handleLoginResult(boolean success, String user, String newToken,
                                   String display, String message) {
        if (success) {
            username = user == null || user.isEmpty() ? username : user;
            displayName = display == null ? "" : display;
            token = newToken == null ? "" : newToken;
            enabled = true;
            error = "";
            prefs.edit()
                    .putString("username", username)
                    .putString("token", token)
                    .putString("display_name", displayName)
                    .putBoolean("enabled", true)
                    .apply();
        } else {
            state = STATE_ERROR;
            error = message == null || message.isEmpty() ? "Falha ao entrar no RetroAchievements." : message;
        }
        refreshFromNative();
    }

    private void handleGameLoaded(String title, int gameId, int unlocked, int total,
                                  int unlockedPoints, int totalPoints) {
        gameTitle = title == null ? "" : title;
        unlockedCount = unlocked;
        totalCount = total;
        pointsUnlocked = unlockedPoints;
        pointsTotal = totalPoints;
        state = STATE_READY;
        error = "";
        notifyListeners();
    }

    private void handleGameError(String message) {
        state = STATE_ERROR;
        error = message == null || message.isEmpty() ? "Falha ao carregar as conquistas." : message;
        notifyListeners();
    }

    private void handleUnlock(String title, String description, int points, int unlocked,
                              int total, int unlockedPoints, int totalPoints) {
        unlockedCount = unlocked;
        totalCount = total;
        pointsUnlocked = unlockedPoints;
        pointsTotal = totalPoints;
        pendingUnlocks.add(new Unlock(title, description, points, unlocked, total,
                unlockedPoints, totalPoints));
        notifyListeners();
        dispatchNextUnlock();
    }

    private static int parseInt(String value, int fallback) {
        try {
            return Integer.parseInt(value.trim());
        } catch (Exception e) {
            return fallback;
        }
    }

    /* ----------------------------- getters ----------------------------- */

    public Context getContext() {
        return context;
    }

    public boolean isEnabled() {
        return enabled;
    }

    public boolean hasToken() {
        return token != null && !token.isEmpty();
    }

    public String getUsername() {
        return username == null ? "" : username;
    }

    public String getDisplayName() {
        return displayName == null || displayName.isEmpty() ? getUsername() : displayName;
    }

    public String getGameHash() {
        return gameHash;
    }

    public int getState() {
        return state;
    }

    public int getScore() {
        return score;
    }

    public String getGameTitle() {
        return gameTitle == null ? "" : gameTitle;
    }

    public int getUnlockedCount() {
        return unlockedCount;
    }

    public int getTotalCount() {
        return totalCount;
    }

    public int getPointsUnlocked() {
        return pointsUnlocked;
    }

    public int getPointsTotal() {
        return pointsTotal;
    }

    public String getError() {
        return error == null ? "" : error;
    }
}
