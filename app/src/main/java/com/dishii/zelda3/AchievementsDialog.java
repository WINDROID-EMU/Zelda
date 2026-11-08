package com.dishii.zelda3;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Color;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/** Dialog that lists the loaded RetroAchievements set and the user's progress. */
public final class AchievementsDialog {

    private AchievementsDialog() {
    }

    public static void show(final Activity activity, final boolean pauseGame) {
        if (activity == null || activity.isFinishing()) return;

        final RetroAchievementsClient client = RetroAchievementsClient.get();
        final Dialog dialog = new Dialog(activity);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        dialog.setContentView(R.layout.dialog_achievements);
        if (dialog.getWindow() != null) {
            dialog.getWindow().setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
            dialog.getWindow().setBackgroundDrawableResource(android.R.color.transparent);
        }

        final TextView summary = dialog.findViewById(R.id.tv_achievements_summary);
        final TextView status = dialog.findViewById(R.id.tv_achievements_status);
        final LinearLayout list = dialog.findViewById(R.id.list_achievements);
        final Button close = dialog.findViewById(R.id.btn_close_achievements);

        if (client != null) {
            client.refreshFromNative();
        }

        if (summary != null) {
            summary.setText(buildSummary(activity, client));
        }
        if (status != null) {
            status.setText(buildStatus(activity, client));
            status.setVisibility(View.VISIBLE);
        }

        List<RetroAchievementsClient.Achievement> achievements =
                client == null ? new ArrayList<>() : client.getAchievements();

        if (list != null) {
            list.removeAllViews();
            LayoutInflater inflater = LayoutInflater.from(activity);
            List<RetroAchievementsClient.Achievement> unlocked = new ArrayList<>();
            List<RetroAchievementsClient.Achievement> locked = new ArrayList<>();

            for (RetroAchievementsClient.Achievement achievement : achievements) {
                if (achievement.unlocked) {
                    unlocked.add(achievement);
                } else {
                    locked.add(achievement);
                }
            }

            if (achievements.isEmpty()) {
                TextView empty = new TextView(activity);
                empty.setText(R.string.ra_dialog_empty);
                empty.setTextColor(Color.parseColor("#B0C2DE"));
                empty.setTextSize(12f);
                empty.setPadding(0, dp(activity, 12), 0, 0);
                list.addView(empty);
            } else {
                addSection(activity, inflater, list,
                        activity.getString(R.string.ra_section_unlocked, unlocked.size()), unlocked);
                addSection(activity, inflater, list,
                        activity.getString(R.string.ra_section_locked, locked.size()), locked);
            }
        }

        if (close != null) {
            close.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    dialog.dismiss();
                }
            });
        }

        if (pauseGame) {
            dialog.setOnDismissListener(new android.content.DialogInterface.OnDismissListener() {
                @Override
                public void onDismiss(android.content.DialogInterface d) {
                    setPaused(false);
                }
            });
        }

        dialog.show();

        if (pauseGame) {
            setPaused(true);
        }
    }

    private static void setPaused(boolean paused) {
        try {
            MainActivity.nativeRaPause(paused);
        } catch (UnsatisfiedLinkError | Exception e) {
            Log.w("Zelda3-RA", "Falha ao alterar a pausa do RetroAchievements: " + e.getMessage());
        }
    }

    private static void addSection(Activity activity, LayoutInflater inflater, LinearLayout container,
                                   String title, List<RetroAchievementsClient.Achievement> achievements) {
        if (achievements.isEmpty()) return;

        TextView header = new TextView(activity);
        header.setText(title);
        header.setTextColor(Color.parseColor("#FFD700"));
        header.setTextSize(12f);
        header.setPadding(0, dp(activity, 10), 0, dp(activity, 4));
        container.addView(header);

        for (RetroAchievementsClient.Achievement achievement : achievements) {
            View row = inflater.inflate(R.layout.item_achievement, container, false);
            TextView state = row.findViewById(R.id.tv_achievement_state);
            TextView name = row.findViewById(R.id.tv_achievement_title);
            TextView description = row.findViewById(R.id.tv_achievement_description);
            TextView points = row.findViewById(R.id.tv_achievement_points);
            TextView progress = row.findViewById(R.id.tv_achievement_progress);

            state.setText(achievement.unlocked ? "🏆" : "🔒");
            name.setText(achievement.title);
            name.setTextColor(Color.parseColor(achievement.unlocked ? "#FFD700" : "#FFFFFF"));
            description.setText(achievement.description);
            description.setVisibility(achievement.description == null || achievement.description.isEmpty()
                    ? View.GONE : View.VISIBLE);
            points.setText(activity.getString(R.string.ra_points, achievement.points));

            if (achievement.progress != null && !achievement.progress.isEmpty()) {
                progress.setText(achievement.progress);
                progress.setVisibility(View.VISIBLE);
            } else {
                progress.setVisibility(View.GONE);
            }

            container.addView(row);
        }
    }

    private static String buildSummary(Activity activity, RetroAchievementsClient client) {
        if (client == null) return activity.getString(R.string.ra_not_available);

        switch (client.getState()) {
            case RetroAchievementsClient.STATE_READY:
                return activity.getString(R.string.ra_summary_ready,
                        client.getUnlockedCount(), client.getTotalCount(),
                        client.getPointsUnlocked(), client.getPointsTotal());
            case RetroAchievementsClient.STATE_LOADING_GAME:
                return activity.getString(R.string.ra_summary_loading);
            case RetroAchievementsClient.STATE_ERROR:
                return client.getError();
            case RetroAchievementsClient.STATE_DISABLED:
                return activity.getString(R.string.ra_status_disabled);
            case RetroAchievementsClient.STATE_LOGGING_IN:
                return activity.getString(R.string.ra_status_logging_in);
            default:
                return activity.getString(R.string.ra_summary_offline);
        }
    }

    private static String buildStatus(Activity activity, RetroAchievementsClient client) {
        if (client == null) return "";

        if (client.getState() == RetroAchievementsClient.STATE_READY && !client.getGameTitle().isEmpty()) {
            return activity.getString(R.string.ra_dialog_game, client.getGameTitle());
        }
        if (client.getState() == RetroAchievementsClient.STATE_ERROR && !client.getError().isEmpty()) {
            return client.getError();
        }
        if (client.getUsername().isEmpty()) {
            return activity.getString(R.string.ra_dialog_not_connected);
        }
        return activity.getString(R.string.ra_dialog_connected, client.getDisplayName());
    }

    private static int dp(Activity activity, int value) {
        return (int) (value * activity.getResources().getDisplayMetrics().density + 0.5f);
    }
}
