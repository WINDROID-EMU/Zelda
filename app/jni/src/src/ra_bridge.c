#include "ra_bridge.h"

#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc_api_request.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "zelda_rtl.h"

#if defined(__ANDROID__)
#include <jni.h>
#include <android/log.h>
#define RA_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Zelda3-RA", __VA_ARGS__)
#define RA_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "Zelda3-RA", __VA_ARGS__)
#else
#define RA_LOGI(...) fprintf(stderr, __VA_ARGS__)
#define RA_LOGW(...) fprintf(stderr, __VA_ARGS__)
#endif

extern uint8 g_ram[131072];

enum {
  kRaStateDisabled = 0,
  kRaStateLoggedOut = 1,
  kRaStateLoggingIn = 2,
  kRaStateLoggedIn = 3,
  kRaStateLoadingGame = 4,
  kRaStateReady = 5,
  kRaStateError = 6,
};

#define RA_TEXT_SHORT 96
#define RA_TEXT_MEDIUM 384

typedef struct RaAchievement {
  uint32_t id;
  uint32_t points;
  int unlocked;
  int bucket;
  char title[RA_TEXT_SHORT];
  char description[RA_TEXT_MEDIUM];
  char progress[32];
} RaAchievement;

static rc_client_t *g_ra_client;

// Serializes every rc_client call that is not a pure read. The game thread
// calls rc_client_do_frame at 60 Hz; the RA worker performs login/load even
// while the game loop is paused (for example, in the settings screen).
static pthread_mutex_t g_ra_client_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_ra_worker_thread;
static bool g_ra_worker_started;
static volatile bool g_ra_worker_stop;

// Session/configuration state. Guarded by g_ra_cfg_mutex.
static pthread_mutex_t g_ra_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_ra_cfg_enabled;
static bool g_ra_cfg_dirty;
static char g_ra_cfg_user[RA_TEXT_SHORT];
static char g_ra_cfg_token[160];
static char g_ra_cfg_hash[80];
static bool g_ra_pending_logout;
static bool g_ra_pending_password_valid;
static char g_ra_pending_password[160];
static bool g_ra_logged_in;
static char g_ra_active_user[RA_TEXT_SHORT];
static char g_ra_active_token[160];
static char g_ra_attempted_login_user[RA_TEXT_SHORT];
static char g_ra_attempted_login_token[160];
static char g_ra_attempted_hash[80];

// Presentation cache. Guarded by g_ra_cache_mutex and only rebuilt on the
// game thread, so Java can read it safely at any time.
static pthread_mutex_t g_ra_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_ra_state = kRaStateDisabled;
static char g_ra_status_user[RA_TEXT_SHORT];
static char g_ra_status_game[RA_TEXT_SHORT];
static char g_ra_status_error[RA_TEXT_MEDIUM];
static uint32_t g_ra_status_score;
static uint32_t g_ra_status_unlocked;
static uint32_t g_ra_status_total;
static uint32_t g_ra_status_points_unlocked;
static uint32_t g_ra_status_points_total;
static RaAchievement *g_ra_achievements;
static int g_ra_achievement_count;
static bool g_ra_game_loaded;
static volatile bool g_ra_cache_dirty;
static volatile bool g_ra_dialog_paused;

#if defined(__ANDROID__)
static JavaVM *g_ra_jvm;
static jclass g_ra_client_class;
static jmethodID g_ra_mid_login;
static jmethodID g_ra_mid_game_loaded;
static jmethodID g_ra_mid_game_error;
static jmethodID g_ra_mid_unlock;
static jclass g_ra_http_class;
static jmethodID g_ra_mid_http;
static jclass g_ra_integer_class;
static jmethodID g_ra_mid_int_value;
#endif

static void ra_process_pending_locked(void);

static void *ra_worker_main(void *arg) {
  (void)arg;

  while (!g_ra_worker_stop) {
    pthread_mutex_lock(&g_ra_client_mutex);
    ra_process_pending_locked();
    pthread_mutex_unlock(&g_ra_client_mutex);

    for (int i = 0; i < 5 && !g_ra_worker_stop; i++) usleep(20000);
  }

  return NULL;
}

static void ra_start_worker(void) {
  bool start = false;

  pthread_mutex_lock(&g_ra_cfg_mutex);
  if (!g_ra_worker_started) {
    g_ra_worker_started = true;
    g_ra_worker_stop = false;
    start = true;
  }
  pthread_mutex_unlock(&g_ra_cfg_mutex);

  if (start && pthread_create(&g_ra_worker_thread, NULL, ra_worker_main, NULL) != 0) {
    pthread_mutex_lock(&g_ra_cfg_mutex);
    g_ra_worker_started = false;
    pthread_mutex_unlock(&g_ra_cfg_mutex);
    RA_LOGW("Nao foi possivel iniciar a thread do RetroAchievements");
  }
}

static bool ra_is_enabled(void) {
  bool enabled;
  pthread_mutex_lock(&g_ra_cfg_mutex);
  enabled = g_ra_cfg_enabled;
  pthread_mutex_unlock(&g_ra_cfg_mutex);
  return enabled;
}

static void ra_copy_string(char *dst, size_t size, const char *src) {
  if (!src) src = "";
  if (size == 0) return;
  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}

static void ra_set_error(const char *message) {
  pthread_mutex_lock(&g_ra_cache_mutex);
  ra_copy_string(g_ra_status_error, sizeof(g_ra_status_error), message);
  pthread_mutex_unlock(&g_ra_cache_mutex);
}

/* ------------------------------------------------------------------------- */
/* Memory mapping                                                            */
/* ------------------------------------------------------------------------- */

static uint32_t ra_read_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client) {
  (void)client;
  if (!buffer || num_bytes == 0) return 0;

  const uint64_t end = (uint64_t)address + num_bytes;

  // SNES WRAM: 0x7E0000-0x7FFFFF maps 1:1 to the 128 KiB g_ram buffer.
  if (address >= 0x7E0000 && end <= 0x800000) {
    memcpy(buffer, g_ram + (address - 0x7E0000), num_bytes);
    return num_bytes;
  }

  // Low RAM mirror (0x0000-0x1FFF).
  if (end <= 0x2000) {
    memcpy(buffer, g_ram + address, num_bytes);
    return num_bytes;
  }

  // Cartridge SRAM (save file), exposed as 0x700000-0x701FFF.
  if (address >= 0x700000 && end <= 0x702000 && g_zenv.sram) {
    memcpy(buffer, g_zenv.sram + (address - 0x700000), num_bytes);
    return num_bytes;
  }

  return 0;
}

/* ------------------------------------------------------------------------- */
/* JNI helpers                                                               */
/* ------------------------------------------------------------------------- */

#if defined(__ANDROID__)
static JNIEnv *ra_acquire_env(bool *attached) {
  JNIEnv *env = NULL;
  jint status;

  *attached = false;
  if (!g_ra_jvm) return NULL;

  status = (*g_ra_jvm)->GetEnv(g_ra_jvm, (void **)&env, JNI_VERSION_1_6);
  if (status == JNI_EDETACHED) {
    if ((*g_ra_jvm)->AttachCurrentThread(g_ra_jvm, &env, NULL) != JNI_OK) return NULL;
    *attached = true;
  } else if (status != JNI_OK) {
    return NULL;
  }
  return env;
}

static void ra_release_env(bool attached) {
  if (attached && g_ra_jvm) (*g_ra_jvm)->DetachCurrentThread(g_ra_jvm);
}

static jstring ra_new_string(JNIEnv *env, const char *utf8) {
  const unsigned char *p;
  size_t length, capacity, out_length = 0;
  jchar *out;
  jstring result;

  if (!utf8) utf8 = "";
  length = strlen(utf8);
  if (length == 0) return (*env)->NewStringUTF(env, "");

  capacity = length + 1;
  out = (jchar *)malloc(capacity * sizeof(jchar));
  if (!out) return (*env)->NewStringUTF(env, "");

  p = (const unsigned char *)utf8;
  while (*p) {
    uint32_t codepoint;
    int extra, i;

    if (*p < 0x80) {
      codepoint = *p;
      extra = 0;
    } else if ((*p & 0xE0) == 0xC0) {
      codepoint = *p & 0x1F;
      extra = 1;
    } else if ((*p & 0xF0) == 0xE0) {
      codepoint = *p & 0x0F;
      extra = 2;
    } else if ((*p & 0xF8) == 0xF0) {
      codepoint = *p & 0x07;
      extra = 3;
    } else {
      p++;
      out[out_length++] = 0xFFFD;
      continue;
    }
    p++;

    for (i = 0; i < extra; i++) {
      if ((*p & 0xC0) != 0x80) {
        codepoint = 0xFFFD;
        break;
      }
      codepoint = (codepoint << 6) | (*p & 0x3F);
      p++;
    }

    if (codepoint < 0x10000) {
      out[out_length++] = (jchar)codepoint;
    } else {
      codepoint -= 0x10000;
      out[out_length++] = (jchar)(0xD800 + (codepoint >> 10));
      out[out_length++] = (jchar)(0xDC00 + (codepoint & 0x3FF));
    }
  }

  result = (*env)->NewString(env, out, (jsize)out_length);
  free(out);
  return result;
}

static void ra_cache_jni(JNIEnv *env) {
  jclass cls;
  jmethodID mid;

  if (!env) return;
  if (!g_ra_jvm) (*env)->GetJavaVM(env, &g_ra_jvm);

  if (!g_ra_client_class) {
    cls = (*env)->FindClass(env, "com/dishii/zelda3/RetroAchievementsClient");
    if (cls) {
      g_ra_client_class = (jclass)(*env)->NewGlobalRef(env, cls);
      (*env)->DeleteLocalRef(env, cls);
    }
  }
  if (g_ra_client_class) {
    if (!g_ra_mid_login) {
      mid = (*env)->GetStaticMethodID(env, g_ra_client_class, "onRaLoginResult",
                                      "(ZLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
      if (mid) g_ra_mid_login = mid;
    }
    if (!g_ra_mid_game_loaded) {
      mid = (*env)->GetStaticMethodID(env, g_ra_client_class, "onRaGameLoaded",
                                      "(Ljava/lang/String;IIIII)V");
      if (mid) g_ra_mid_game_loaded = mid;
    }
    if (!g_ra_mid_game_error) {
      mid = (*env)->GetStaticMethodID(env, g_ra_client_class, "onRaGameError", "(Ljava/lang/String;)V");
      if (mid) g_ra_mid_game_error = mid;
    }
    if (!g_ra_mid_unlock) {
      mid = (*env)->GetStaticMethodID(env, g_ra_client_class, "onRaAchievementUnlocked",
                                      "(Ljava/lang/String;Ljava/lang/String;IIIII)V");
      if (mid) g_ra_mid_unlock = mid;
    }
  }

  if (!g_ra_http_class) {
    cls = (*env)->FindClass(env, "com/dishii/zelda3/RaHttpBridge");
    if (cls) {
      g_ra_http_class = (jclass)(*env)->NewGlobalRef(env, cls);
      (*env)->DeleteLocalRef(env, cls);
    }
  }
  if (g_ra_http_class && !g_ra_mid_http) {
    mid = (*env)->GetStaticMethodID(env, g_ra_http_class, "performHttpRequest",
                                    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/Object;");
    if (mid) g_ra_mid_http = mid;
  }

  if (!g_ra_integer_class) {
    cls = (*env)->FindClass(env, "java/lang/Integer");
    if (cls) {
      g_ra_integer_class = (jclass)(*env)->NewGlobalRef(env, cls);
      (*env)->DeleteLocalRef(env, cls);
    }
  }
  if (g_ra_integer_class && !g_ra_mid_int_value) {
    mid = (*env)->GetMethodID(env, g_ra_integer_class, "intValue", "()I");
    if (mid) g_ra_mid_int_value = mid;
  }

  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

static void ra_notify_login(bool success, const char *user, const char *token, const char *display, const char *message) {
  bool attached;
  JNIEnv *env;

  if (!g_ra_jvm || !g_ra_client_class || !g_ra_mid_login) return;
  env = ra_acquire_env(&attached);
  if (!env) return;

  jstring juser = ra_new_string(env, user);
  jstring jtoken = ra_new_string(env, token);
  jstring jdisplay = ra_new_string(env, display);
  jstring jmessage = ra_new_string(env, message ? message : "");
  (*env)->CallStaticVoidMethod(env, g_ra_client_class, g_ra_mid_login,
                               success ? JNI_TRUE : JNI_FALSE, juser, jtoken, jdisplay, jmessage);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
  (*env)->DeleteLocalRef(env, juser);
  (*env)->DeleteLocalRef(env, jtoken);
  (*env)->DeleteLocalRef(env, jdisplay);
  (*env)->DeleteLocalRef(env, jmessage);
  ra_release_env(attached);
}

static void ra_notify_game_loaded(const char *title, int game_id, int unlocked, int total,
                                  int points_unlocked, int points_total) {
  bool attached;
  JNIEnv *env;

  if (!g_ra_jvm || !g_ra_client_class || !g_ra_mid_game_loaded) return;
  env = ra_acquire_env(&attached);
  if (!env) return;

  jstring jtitle = ra_new_string(env, title);
  (*env)->CallStaticVoidMethod(env, g_ra_client_class, g_ra_mid_game_loaded, jtitle, game_id,
                               unlocked, total, points_unlocked, points_total);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
  (*env)->DeleteLocalRef(env, jtitle);
  ra_release_env(attached);
}

static void ra_notify_game_error(const char *message) {
  bool attached;
  JNIEnv *env;

  if (!g_ra_jvm || !g_ra_client_class || !g_ra_mid_game_error) return;
  env = ra_acquire_env(&attached);
  if (!env) return;

  jstring jmessage = ra_new_string(env, message ? message : "");
  (*env)->CallStaticVoidMethod(env, g_ra_client_class, g_ra_mid_game_error, jmessage);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
  (*env)->DeleteLocalRef(env, jmessage);
  ra_release_env(attached);
}

static void ra_notify_unlock(const char *title, const char *description, int points,
                             int unlocked, int total, int points_unlocked, int points_total) {
  bool attached;
  JNIEnv *env;

  if (!g_ra_jvm || !g_ra_client_class || !g_ra_mid_unlock) return;
  env = ra_acquire_env(&attached);
  if (!env) return;

  jstring jtitle = ra_new_string(env, title);
  jstring jdescription = ra_new_string(env, description);
  (*env)->CallStaticVoidMethod(env, g_ra_client_class, g_ra_mid_unlock, jtitle, jdescription, points,
                               unlocked, total, points_unlocked, points_total);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
  (*env)->DeleteLocalRef(env, jtitle);
  (*env)->DeleteLocalRef(env, jdescription);
  ra_release_env(attached);
}
#endif  // __ANDROID__

/* ------------------------------------------------------------------------- */
/* Server transport                                                          */
/* ------------------------------------------------------------------------- */

typedef struct RaPendingRequest {
  char *url;
  char *post_data;
  char *content_type;
  rc_client_server_callback_t callback;
  void *callback_data;
} RaPendingRequest;

static void ra_free_pending_request(RaPendingRequest *request) {
  if (!request) return;
  free(request->url);
  free(request->post_data);
  free(request->content_type);
  free(request);
}

#if defined(__ANDROID__)
static void *ra_http_thread(void *arg) {
  RaPendingRequest *pending = (RaPendingRequest *)arg;
  rc_api_server_response_t response;
  bool attached = false;
  JNIEnv *env;

  response.body = NULL;
  response.body_length = 0;
  response.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;

  env = ra_acquire_env(&attached);
  if (env && g_ra_http_class && g_ra_mid_http) {
    jstring jurl = ra_new_string(env, pending->url);
    jstring jpost = pending->post_data ? ra_new_string(env, pending->post_data) : NULL;
    jstring jcontent_type = pending->content_type ? ra_new_string(env, pending->content_type) : NULL;

    jobjectArray result = (jobjectArray)(*env)->CallStaticObjectMethod(
        env, g_ra_http_class, g_ra_mid_http, jurl, jpost, jcontent_type);

    if ((*env)->ExceptionCheck(env)) {
      (*env)->ExceptionClear(env);
      result = NULL;
    }

    if (result) {
      jobject status_obj = (*env)->GetObjectArrayElement(env, result, 0);
      jbyteArray body_obj = (jbyteArray)(*env)->GetObjectArrayElement(env, result, 1);
      int status = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
      jsize length = 0;
      char *body = NULL;

      if (status_obj && g_ra_mid_int_value) {
        status = (*env)->CallIntMethod(env, status_obj, g_ra_mid_int_value);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
      }
      if (body_obj) {
        length = (*env)->GetArrayLength(env, body_obj);
        if (length > 0) {
          // rcheevos expects a NUL-terminated body even though it also receives
          // the explicit length, so copy into an owned buffer.
          body = (char *)malloc((size_t)length + 1);
          if (body) {
            (*env)->GetByteArrayRegion(env, body_obj, 0, length, (jbyte *)body);
            if ((*env)->ExceptionCheck(env)) {
              (*env)->ExceptionClear(env);
              free(body);
              body = NULL;
              length = 0;
            } else {
              body[length] = '\0';
            }
          } else {
            length = 0;
          }
        }
      }

      response.http_status_code = status;
      response.body = body;
      response.body_length = (size_t)length;

      pending->callback(&response, pending->callback_data);

      free(body);
      if (body_obj) (*env)->DeleteLocalRef(env, body_obj);
      if (status_obj) (*env)->DeleteLocalRef(env, status_obj);
      (*env)->DeleteLocalRef(env, result);
    } else {
      pending->callback(&response, pending->callback_data);
    }

    (*env)->DeleteLocalRef(env, jurl);
    if (jpost) (*env)->DeleteLocalRef(env, jpost);
    if (jcontent_type) (*env)->DeleteLocalRef(env, jcontent_type);
  } else {
    pending->callback(&response, pending->callback_data);
  }

  if (attached) ra_release_env(attached);
  ra_free_pending_request(pending);
  return NULL;
}

static void ra_server_call(const rc_api_request_t *request, rc_client_server_callback_t callback,
                           void *callback_data, rc_client_t *client) {
  (void)client;

  RaPendingRequest *pending = (RaPendingRequest *)calloc(1, sizeof(RaPendingRequest));
  pthread_t thread;

  if (!pending) {
    rc_api_server_response_t response;
    response.body = NULL;
    response.body_length = 0;
    response.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
    callback(&response, callback_data);
    return;
  }

  pending->url = request->url ? strdup(request->url) : NULL;
  pending->post_data = request->post_data ? strdup(request->post_data) : NULL;
  pending->content_type = request->content_type ? strdup(request->content_type) : NULL;
  pending->callback = callback;
  pending->callback_data = callback_data;

  if (!pending->url || pthread_create(&thread, NULL, ra_http_thread, pending) != 0) {
    rc_api_server_response_t response;
    response.body = NULL;
    response.body_length = 0;
    response.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
    ra_free_pending_request(pending);
    callback(&response, callback_data);
    return;
  }

  pthread_detach(thread);
}
#else
static void ra_server_call(const rc_api_request_t *request, rc_client_server_callback_t callback,
                           void *callback_data, rc_client_t *client) {
  (void)request;
  (void)client;
  rc_api_server_response_t response;
  response.body = NULL;
  response.body_length = 0;
  response.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
  callback(&response, callback_data);
}
#endif

/* ------------------------------------------------------------------------- */
/* Cache                                                                     */
/* ------------------------------------------------------------------------- */

static void ra_rebuild_achievement_cache(void) {
  RaAchievement *items = NULL;
  int count = 0;
  int capacity = 0;

  if (g_ra_client && rc_client_is_game_loaded(g_ra_client) && rc_client_has_achievements(g_ra_client)) {
    rc_client_achievement_list_t *list = rc_client_create_achievement_list(
        g_ra_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);

    if (list) {
      for (uint32_t b = 0; b < list->num_buckets; b++) {
        const rc_client_achievement_bucket_t *bucket = &list->buckets[b];
        for (uint32_t i = 0; i < bucket->num_achievements; i++) {
          const rc_client_achievement_t *achievement = bucket->achievements[i];
          if (!achievement) continue;

          if (count == capacity) {
            int new_capacity = capacity ? capacity * 2 : 64;
            RaAchievement *grown = (RaAchievement *)realloc(items, sizeof(RaAchievement) * new_capacity);
            if (!grown) break;
            items = grown;
            capacity = new_capacity;
          }

          RaAchievement *item = &items[count++];
          memset(item, 0, sizeof(*item));
          item->id = achievement->id;
          item->points = achievement->points;
          item->unlocked = achievement->unlocked != 0 ? 1 : 0;
          item->bucket = bucket->bucket_type;
          ra_copy_string(item->title, sizeof(item->title), achievement->title);
          ra_copy_string(item->description, sizeof(item->description), achievement->description);
          ra_copy_string(item->progress, sizeof(item->progress), achievement->measured_progress);
        }
      }
      rc_client_destroy_achievement_list(list);
    }
  }

  pthread_mutex_lock(&g_ra_cache_mutex);
  free(g_ra_achievements);
  g_ra_achievements = items;
  g_ra_achievement_count = count;
  pthread_mutex_unlock(&g_ra_cache_mutex);
}

static void ra_store_game_info(rc_client_t *client) {
  const rc_client_game_t *game;
  rc_client_user_game_summary_t summary;

  if (!client || !rc_client_is_game_loaded(client)) return;

  game = rc_client_get_game_info(client);
  memset(&summary, 0, sizeof(summary));
  rc_client_get_user_game_summary(client, &summary);

  pthread_mutex_lock(&g_ra_cache_mutex);
  g_ra_state = kRaStateReady;
  g_ra_game_loaded = true;
  g_ra_status_error[0] = '\0';
  ra_copy_string(g_ra_status_game, sizeof(g_ra_status_game), game ? game->title : "");
  g_ra_status_unlocked = summary.num_unlocked_achievements;
  g_ra_status_total = summary.num_core_achievements;
  g_ra_status_points_unlocked = summary.points_unlocked;
  g_ra_status_points_total = summary.points_core;
  pthread_mutex_unlock(&g_ra_cache_mutex);

  ra_notify_game_loaded(game ? game->title : "", game ? (int)game->id : 0,
                        (int)summary.num_unlocked_achievements, (int)summary.num_core_achievements,
                        (int)summary.points_unlocked, (int)summary.points_core);
}

/* ------------------------------------------------------------------------- */
/* rc_client callbacks                                                       */
/* ------------------------------------------------------------------------- */

static void ra_load_callback(int result, const char *error_message, rc_client_t *client, void *userdata);

static void ra_begin_load(rc_client_t *client, const char *hash) {
  if (!client || !hash || !hash[0]) return;

  pthread_mutex_lock(&g_ra_cfg_mutex);
  ra_copy_string(g_ra_attempted_hash, sizeof(g_ra_attempted_hash), hash);
  pthread_mutex_unlock(&g_ra_cfg_mutex);

  if (rc_client_is_game_loaded(client)) rc_client_unload_game(client);

  pthread_mutex_lock(&g_ra_cache_mutex);
  g_ra_state = kRaStateLoadingGame;
  g_ra_game_loaded = false;
  g_ra_status_error[0] = '\0';
  pthread_mutex_unlock(&g_ra_cache_mutex);

  rc_client_begin_load_game(client, hash, ra_load_callback, NULL);
}

static void ra_login_callback(int result, const char *error_message, rc_client_t *client, void *userdata) {
  (void)userdata;

  if (result == RC_OK) {
    const rc_client_user_t *user = rc_client_get_user_info(client);
    const char *username = (user && user->username) ? user->username : "";
    const char *token = (user && user->token) ? user->token : "";
    const char *display = (user && user->display_name) ? user->display_name : username;
    uint32_t score = user ? user->score : 0;

    pthread_mutex_lock(&g_ra_cfg_mutex);
    g_ra_logged_in = true;
    ra_copy_string(g_ra_active_user, sizeof(g_ra_active_user), username);
    ra_copy_string(g_ra_active_token, sizeof(g_ra_active_token), token);
    // Remember the issued token so a later configure with the same token does
    // not look like a different account.
    ra_copy_string(g_ra_cfg_token, sizeof(g_ra_cfg_token), token);
    ra_copy_string(g_ra_cfg_user, sizeof(g_ra_cfg_user), username);
    g_ra_attempted_hash[0] = '\0';
    // Let the game thread start the load on the next frame; rc_client
    // functions that change the loaded game must not run on this worker thread.
    g_ra_cfg_dirty = true;
    pthread_mutex_unlock(&g_ra_cfg_mutex);

    pthread_mutex_lock(&g_ra_cache_mutex);
    g_ra_state = kRaStateLoggedIn;
    ra_copy_string(g_ra_status_user, sizeof(g_ra_status_user), username);
    g_ra_status_score = score;
    g_ra_status_error[0] = '\0';
    pthread_mutex_unlock(&g_ra_cache_mutex);

    RA_LOGI("Login efetuado: %s", username);
    ra_notify_login(true, username, token, display, NULL);
    g_ra_cache_dirty = true;
  } else {
    pthread_mutex_lock(&g_ra_cfg_mutex);
    g_ra_logged_in = false;
    pthread_mutex_unlock(&g_ra_cfg_mutex);

    pthread_mutex_lock(&g_ra_cache_mutex);
    g_ra_state = kRaStateError;
    ra_copy_string(g_ra_status_error, sizeof(g_ra_status_error),
                   error_message ? error_message : "Falha ao entrar no RetroAchievements");
    pthread_mutex_unlock(&g_ra_cache_mutex);

    char failed_user[RA_TEXT_SHORT];
    pthread_mutex_lock(&g_ra_cfg_mutex);
    ra_copy_string(failed_user, sizeof(failed_user), g_ra_cfg_user);
    pthread_mutex_unlock(&g_ra_cfg_mutex);

    RA_LOGW("Falha no login: %s", error_message ? error_message : "erro desconhecido");
    ra_notify_login(false, failed_user, "", "", error_message);
  }
}

static void ra_load_callback(int result, const char *error_message, rc_client_t *client, void *userdata) {
  (void)userdata;

  if (result == RC_OK) {
    RA_LOGI("Jogo carregado no RetroAchievements");
    ra_store_game_info(client);
    g_ra_cache_dirty = true;
  } else {
    const char *message = error_message ? error_message : "Nao foi possivel carregar o jogo no RetroAchievements";

    pthread_mutex_lock(&g_ra_cache_mutex);
    g_ra_state = kRaStateError;
    g_ra_game_loaded = false;
    ra_copy_string(g_ra_status_error, sizeof(g_ra_status_error), message);
    pthread_mutex_unlock(&g_ra_cache_mutex);

    RA_LOGW("Falha ao carregar jogo: %s", message);
    ra_notify_game_error(message);
  }
}

static void ra_log_callback(const char *message, const rc_client_t *client) {
  (void)client;
  RA_LOGI("%s", message ? message : "");
}

static void ra_event_handler(const rc_client_event_t *event, rc_client_t *client) {
  if (!event) return;

  switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
      const rc_client_achievement_t *achievement = event->achievement;
      rc_client_user_game_summary_t summary;

      if (!achievement) break;
      memset(&summary, 0, sizeof(summary));
      rc_client_get_user_game_summary(client, &summary);

      pthread_mutex_lock(&g_ra_cache_mutex);
      g_ra_status_unlocked = summary.num_unlocked_achievements;
      g_ra_status_total = summary.num_core_achievements;
      g_ra_status_points_unlocked = summary.points_unlocked;
      g_ra_status_points_total = summary.points_core;
      pthread_mutex_unlock(&g_ra_cache_mutex);

      g_ra_cache_dirty = true;
      RA_LOGI("Conquista desbloqueada: %s (%u pts)", achievement->title ? achievement->title : "?",
              achievement->points);

      ra_notify_unlock(achievement->title, achievement->description, (int)achievement->points,
                       (int)summary.num_unlocked_achievements, (int)summary.num_core_achievements,
                       (int)summary.points_unlocked, (int)summary.points_core);
      break;
    }
    case RC_CLIENT_EVENT_SERVER_ERROR: {
      const rc_client_server_error_t *error = event->server_error;
      if (error && error->error_message) {
        RA_LOGW("Erro do servidor RA: %s", error->error_message);
        ra_set_error(error->error_message);
      }
      break;
    }
    case RC_CLIENT_EVENT_DISCONNECTED:
      ra_set_error("Sem conexao com o RetroAchievements. Os desbloqueios serao enviados depois.");
      break;
    case RC_CLIENT_EVENT_RECONNECTED:
      ra_set_error("");
      break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
      RA_LOGI("Todas as conquistas foram obtidas!");
      break;
    default:
      break;
  }
}

/* ------------------------------------------------------------------------- */
/* Game-thread processing                                                    */
/* ------------------------------------------------------------------------- */

void RaProcessPending(void) {
  pthread_mutex_lock(&g_ra_client_mutex);
  ra_process_pending_locked();
  pthread_mutex_unlock(&g_ra_client_mutex);
}

static void ra_process_pending_locked(void) {
  bool enabled, dirty, logout_requested, password_valid, logged_in;
  char user[RA_TEXT_SHORT], token[160], hash[80], password[160];
  int state;

  pthread_mutex_lock(&g_ra_cfg_mutex);
  enabled = g_ra_cfg_enabled;
  dirty = g_ra_cfg_dirty;
  logout_requested = g_ra_pending_logout;
  password_valid = g_ra_pending_password_valid;
  logged_in = g_ra_logged_in;
  if (password_valid) ra_copy_string(password, sizeof(password), g_ra_pending_password);
  ra_copy_string(user, sizeof(user), g_ra_cfg_user);
  ra_copy_string(token, sizeof(token), g_ra_cfg_token);
  ra_copy_string(hash, sizeof(hash), g_ra_cfg_hash);
  if (dirty) {
    g_ra_cfg_dirty = false;
    g_ra_pending_logout = false;
    g_ra_pending_password_valid = false;
  }
  pthread_mutex_unlock(&g_ra_cfg_mutex);

  pthread_mutex_lock(&g_ra_cache_mutex);
  state = g_ra_state;
  pthread_mutex_unlock(&g_ra_cache_mutex);

  if (!enabled) {
    if (state != kRaStateDisabled) {
      if (g_ra_client) {
        if (logged_in) rc_client_logout(g_ra_client);
        if (rc_client_is_game_loaded(g_ra_client)) rc_client_unload_game(g_ra_client);
      }
      pthread_mutex_lock(&g_ra_cfg_mutex);
      g_ra_logged_in = false;
      g_ra_active_user[0] = '\0';
      g_ra_active_token[0] = '\0';
      g_ra_attempted_login_user[0] = '\0';
      g_ra_attempted_login_token[0] = '\0';
      g_ra_attempted_hash[0] = '\0';
      pthread_mutex_unlock(&g_ra_cfg_mutex);

      pthread_mutex_lock(&g_ra_cache_mutex);
      g_ra_state = kRaStateDisabled;
      g_ra_game_loaded = false;
      g_ra_status_error[0] = '\0';
      pthread_mutex_unlock(&g_ra_cache_mutex);
      g_ra_cache_dirty = true;
    }
    return;
  }

  if (!dirty) {
    // Still keep the summary/achievement cache fresh after asynchronous work.
    if (g_ra_cache_dirty) {
      g_ra_cache_dirty = false;
      ra_rebuild_achievement_cache();
    }
    return;
  }

  if (!g_ra_client) {
    g_ra_client = rc_client_create(ra_read_memory, ra_server_call);
    if (!g_ra_client) {
      pthread_mutex_lock(&g_ra_cache_mutex);
      g_ra_state = kRaStateError;
      ra_copy_string(g_ra_status_error, sizeof(g_ra_status_error),
                     "Nao foi possivel inicializar o RetroAchievements");
      pthread_mutex_unlock(&g_ra_cache_mutex);
      RA_LOGW("rc_client_create falhou");
      return;
    }
    rc_client_set_event_handler(g_ra_client, ra_event_handler);
    rc_client_set_hardcore_enabled(g_ra_client, 0);
    rc_client_set_unofficial_enabled(g_ra_client, 0);
    rc_client_enable_logging(g_ra_client, RC_CLIENT_LOG_LEVEL_WARN, ra_log_callback);

    pthread_mutex_lock(&g_ra_cache_mutex);
    g_ra_state = kRaStateLoggedOut;
    pthread_mutex_unlock(&g_ra_cache_mutex);
  }

  if (logout_requested) {
    if (logged_in) rc_client_logout(g_ra_client);
    if (rc_client_is_game_loaded(g_ra_client)) rc_client_unload_game(g_ra_client);

    pthread_mutex_lock(&g_ra_cfg_mutex);
    g_ra_logged_in = false;
    g_ra_active_user[0] = '\0';
    g_ra_active_token[0] = '\0';
    g_ra_attempted_login_user[0] = '\0';
    g_ra_attempted_login_token[0] = '\0';
    g_ra_attempted_hash[0] = '\0';
    pthread_mutex_unlock(&g_ra_cfg_mutex);

    pthread_mutex_lock(&g_ra_cache_mutex);
    g_ra_state = kRaStateLoggedOut;
    g_ra_game_loaded = false;
    g_ra_status_user[0] = '\0';
    g_ra_status_score = 0;
    g_ra_status_game[0] = '\0';
    g_ra_status_unlocked = 0;
    g_ra_status_total = 0;
    g_ra_status_points_unlocked = 0;
    g_ra_status_points_total = 0;
    g_ra_status_error[0] = '\0';
    pthread_mutex_unlock(&g_ra_cache_mutex);

    g_ra_cache_dirty = true;
    ra_rebuild_achievement_cache();
    RA_LOGI("Logout do RetroAchievements");
    return;
  }

  if (password_valid && user[0] && password[0]) {
    pthread_mutex_lock(&g_ra_cfg_mutex);
    g_ra_attempted_login_user[0] = '\0';
    g_ra_attempted_login_token[0] = '\0';
    pthread_mutex_unlock(&g_ra_cfg_mutex);

    pthread_mutex_lock(&g_ra_cache_mutex);
    g_ra_state = kRaStateLoggingIn;
    g_ra_status_error[0] = '\0';
    pthread_mutex_unlock(&g_ra_cache_mutex);

    RA_LOGI("Iniciando login com senha para %s", user);
    rc_client_begin_login_with_password(g_ra_client, user, password, ra_login_callback, NULL);
    memset(password, 0, sizeof(password));
    return;
  }

  if (!logged_in && token[0] && user[0]) {
    bool attempt = false;

    pthread_mutex_lock(&g_ra_cfg_mutex);
    if (strcmp(g_ra_attempted_login_token, token) != 0 ||
        strcmp(g_ra_attempted_login_user, user) != 0) {
      ra_copy_string(g_ra_attempted_login_user, sizeof(g_ra_attempted_login_user), user);
      ra_copy_string(g_ra_attempted_login_token, sizeof(g_ra_attempted_login_token), token);
      attempt = true;
    }
    pthread_mutex_unlock(&g_ra_cfg_mutex);

    if (attempt) {
      pthread_mutex_lock(&g_ra_cache_mutex);
      g_ra_state = kRaStateLoggingIn;
      g_ra_status_error[0] = '\0';
      pthread_mutex_unlock(&g_ra_cache_mutex);

      RA_LOGI("Iniciando login com token para %s", user);
      rc_client_begin_login_with_token(g_ra_client, user, token, ra_login_callback, NULL);
    }
    return;
  }

  if (logged_in && hash[0]) {
    bool load = false;

    pthread_mutex_lock(&g_ra_cfg_mutex);
    if (strcmp(g_ra_attempted_hash, hash) != 0) {
      load = true;
    }
    pthread_mutex_unlock(&g_ra_cfg_mutex);

    if (load) ra_begin_load(g_ra_client, hash);
  }
}

void RaFrame(void) {
  pthread_mutex_lock(&g_ra_client_mutex);

  if (g_ra_client && !g_ra_dialog_paused && ra_is_enabled()) {
    if (!rc_client_is_game_loaded(g_ra_client)) {
      if (g_ra_cache_dirty) {
        g_ra_cache_dirty = false;
        ra_rebuild_achievement_cache();
      }
    } else {
      rc_client_do_frame(g_ra_client);

      if (g_ra_cache_dirty) {
        g_ra_cache_dirty = false;
        ra_rebuild_achievement_cache();
      }
    }
  }

  pthread_mutex_unlock(&g_ra_client_mutex);
}

void RaShutdown(void) {
  bool was_started;

  g_ra_worker_stop = true;

  pthread_mutex_lock(&g_ra_cfg_mutex);
  was_started = g_ra_worker_started;
  g_ra_worker_started = false;
  pthread_mutex_unlock(&g_ra_cfg_mutex);

  if (was_started && !pthread_equal(pthread_self(), g_ra_worker_thread)) {
    pthread_join(g_ra_worker_thread, NULL);
  }

  pthread_mutex_lock(&g_ra_client_mutex);
  if (g_ra_client) {
    rc_client_destroy(g_ra_client);
    g_ra_client = NULL;
  }
  pthread_mutex_lock(&g_ra_cache_mutex);
  free(g_ra_achievements);
  g_ra_achievements = NULL;
  g_ra_achievement_count = 0;
  pthread_mutex_unlock(&g_ra_cache_mutex);
  pthread_mutex_unlock(&g_ra_client_mutex);
}

bool RaIsDialogPaused(void) {
  return g_ra_dialog_paused;
}

void RaSetDialogPaused(bool paused) {
  g_ra_dialog_paused = paused;
}

/* ------------------------------------------------------------------------- */
/* JNI entry points                                                          */
/* ------------------------------------------------------------------------- */

#if defined(__ANDROID__)
static void ra_copy_jstring(JNIEnv *env, jstring value, char *dst, size_t size) {
  if (!value) {
    dst[0] = '\0';
    return;
  }
  const char *chars = (*env)->GetStringUTFChars(env, value, NULL);
  ra_copy_string(dst, size, chars ? chars : "");
  if (chars) (*env)->ReleaseStringUTFChars(env, value, chars);
}

JNIEXPORT void JNICALL Java_com_dishii_zelda3_MainActivity_nativeRaConfigure(
    JNIEnv *env, jclass cls, jboolean enabled, jstring user, jstring token, jstring hash) {
  (void)cls;
  char new_user[RA_TEXT_SHORT], new_token[160], new_hash[80];

  ra_cache_jni(env);
  ra_copy_jstring(env, user, new_user, sizeof(new_user));
  ra_copy_jstring(env, token, new_token, sizeof(new_token));
  ra_copy_jstring(env, hash, new_hash, sizeof(new_hash));

  pthread_mutex_lock(&g_ra_cfg_mutex);
  if (strcmp(new_user, g_ra_cfg_user) != 0 || strcmp(new_token, g_ra_cfg_token) != 0) {
    g_ra_attempted_login_user[0] = '\0';
    g_ra_attempted_login_token[0] = '\0';
    if (g_ra_client && g_ra_logged_in) {
      // The session belongs to a different user now; force a fresh login.
      g_ra_logged_in = false;
    }
  }
  if (strcmp(new_hash, g_ra_cfg_hash) != 0) {
    g_ra_attempted_hash[0] = '\0';
  }
  g_ra_cfg_enabled = enabled == JNI_TRUE;
  ra_copy_string(g_ra_cfg_user, sizeof(g_ra_cfg_user), new_user);
  ra_copy_string(g_ra_cfg_token, sizeof(g_ra_cfg_token), new_token);
  ra_copy_string(g_ra_cfg_hash, sizeof(g_ra_cfg_hash), new_hash);
  g_ra_cfg_dirty = true;
  pthread_mutex_unlock(&g_ra_cfg_mutex);

  ra_start_worker();

  RA_LOGI("Configuracao recebida: enabled=%d user=%s", enabled == JNI_TRUE, new_user);
}

JNIEXPORT void JNICALL Java_com_dishii_zelda3_MainActivity_nativeRaLogin(
    JNIEnv *env, jclass cls, jstring user, jstring password) {
  (void)cls;
  char new_user[RA_TEXT_SHORT], new_password[160];

  ra_cache_jni(env);
  ra_copy_jstring(env, user, new_user, sizeof(new_user));
  ra_copy_jstring(env, password, new_password, sizeof(new_password));

  pthread_mutex_lock(&g_ra_cfg_mutex);
  ra_copy_string(g_ra_cfg_user, sizeof(g_ra_cfg_user), new_user);
  ra_copy_string(g_ra_pending_password, sizeof(g_ra_pending_password), new_password);
  g_ra_pending_password_valid = true;
  g_ra_pending_logout = false;
  g_ra_cfg_enabled = true;
  g_ra_cfg_dirty = true;
  pthread_mutex_unlock(&g_ra_cfg_mutex);

  ra_start_worker();

  memset(new_password, 0, sizeof(new_password));
  RA_LOGI("Login solicitado para %s", new_user);
}

JNIEXPORT void JNICALL Java_com_dishii_zelda3_MainActivity_nativeRaLogout(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;

  pthread_mutex_lock(&g_ra_cfg_mutex);
  g_ra_cfg_token[0] = '\0';
  g_ra_pending_logout = true;
  g_ra_pending_password_valid = false;
  g_ra_cfg_dirty = true;
  pthread_mutex_unlock(&g_ra_cfg_mutex);

  RA_LOGI("Logout solicitado");
}

JNIEXPORT void JNICALL Java_com_dishii_zelda3_MainActivity_nativeRaPause(JNIEnv *env, jclass cls,
                                                                        jboolean paused) {
  (void)env;
  (void)cls;
  g_ra_dialog_paused = paused == JNI_TRUE;
}

JNIEXPORT jobjectArray JNICALL Java_com_dishii_zelda3_MainActivity_nativeRaGetStatus(
    JNIEnv *env, jclass cls) {
  (void)cls;
  jclass string_class = (*env)->FindClass(env, "java/lang/String");
  jobjectArray array = (*env)->NewObjectArray(env, 9, string_class, NULL);
  char values[9][RA_TEXT_MEDIUM];

  pthread_mutex_lock(&g_ra_cache_mutex);
  snprintf(values[0], sizeof(values[0]), "%d", g_ra_state);
  ra_copy_string(values[1], sizeof(values[1]), g_ra_status_user);
  snprintf(values[2], sizeof(values[2]), "%u", g_ra_status_score);
  ra_copy_string(values[3], sizeof(values[3]), g_ra_status_game);
  snprintf(values[4], sizeof(values[4]), "%u", g_ra_status_unlocked);
  snprintf(values[5], sizeof(values[5]), "%u", g_ra_status_total);
  snprintf(values[6], sizeof(values[6]), "%u", g_ra_status_points_unlocked);
  snprintf(values[7], sizeof(values[7]), "%u", g_ra_status_points_total);
  ra_copy_string(values[8], sizeof(values[8]), g_ra_status_error);
  pthread_mutex_unlock(&g_ra_cache_mutex);

  for (int i = 0; i < 9; i++) {
    jstring value = ra_new_string(env, values[i]);
    (*env)->SetObjectArrayElement(env, array, i, value);
    (*env)->DeleteLocalRef(env, value);
  }
  (*env)->DeleteLocalRef(env, string_class);
  return array;
}

JNIEXPORT jobjectArray JNICALL Java_com_dishii_zelda3_MainActivity_nativeRaGetAchievements(
    JNIEnv *env, jclass cls) {
  (void)cls;
  jclass string_class = (*env)->FindClass(env, "java/lang/String");
  jobjectArray array;
  int count;

  pthread_mutex_lock(&g_ra_cache_mutex);
  count = g_ra_achievement_count;
  pthread_mutex_unlock(&g_ra_cache_mutex);

  array = (*env)->NewObjectArray(env, count, string_class, NULL);

  pthread_mutex_lock(&g_ra_cache_mutex);
  count = g_ra_achievement_count;
  for (int i = 0; i < count; i++) {
    const RaAchievement *item = &g_ra_achievements[i];
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "%u\x1f%s\x1f%s\x1f%u\x1f%d\x1f%s\x1f%d", item->id, item->title,
             item->description, item->points, item->unlocked, item->progress, item->bucket);
    jstring value = ra_new_string(env, buffer);
    (*env)->SetObjectArrayElement(env, array, i, value);
    (*env)->DeleteLocalRef(env, value);
  }
  pthread_mutex_unlock(&g_ra_cache_mutex);

  (*env)->DeleteLocalRef(env, string_class);
  return array;
}
#endif  // __ANDROID__
