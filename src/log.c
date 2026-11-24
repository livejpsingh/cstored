#include "kvs/log.h"
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <inttypes.h>

static FILE *g_log_stream = NULL;
static kvs_log_level_t g_min_level = KVS_LOG_INFO;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *level_to_str(kvs_log_level_t level) {
    switch (level) {
        case KVS_LOG_DEBUG: return "DEBUG";
        case KVS_LOG_INFO:  return "INFO ";
        case KVS_LOG_WARN:  return "WARN ";
        case KVS_LOG_ERROR: return "ERROR";
        case KVS_LOG_FATAL: return "FATAL";
        default:            return "UNKN ";
    }
}

void kvs_log_init(FILE *out_stream, kvs_log_level_t min_level) {
    pthread_mutex_lock(&g_log_lock);
    g_log_stream = (out_stream != NULL) ? out_stream : stderr;
    g_min_level = min_level;
    pthread_mutex_unlock(&g_log_lock);
}

void kvs_log_set_level(kvs_log_level_t min_level) {
    pthread_mutex_lock(&g_log_lock);
    g_min_level = min_level;
    pthread_mutex_unlock(&g_log_lock);
}

void kvs_log_write(
    kvs_log_level_t level,
    const char *file,
    int line,
    const char *fmt,
    ...
) {
    if (level < g_min_level) {
        return;
    }

    /* Extract timestamp */
    struct timespec ts;
#if defined(CLOCK_REALTIME)
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    ts.tv_sec = time(NULL);
    ts.tv_nsec = 0;
#endif

    struct tm tm_buf;
    time_t sec = ts.tv_sec;
#if defined(_WIN32)
    gmtime_s(&tm_buf, &sec);
#else
    gmtime_r(&sec, &tm_buf);
#endif

    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    /* Format message body into fixed stack buffer */
    char msg_buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    /* Serialize log line write under lock to eliminate interleaved lines */
    pthread_mutex_lock(&g_log_lock);
    FILE *stream = (g_log_stream != NULL) ? g_log_stream : stderr;

    uintptr_t tid = (uintptr_t)pthread_self();
    fprintf(stream, "[%s.%03ldZ] [%s] [tid:%" PRIuPTR "] [%s:%d] %s\n",
            time_str,
            (long)(ts.tv_nsec / 1000000L),
            level_to_str(level),
            tid,
            file ? file : "unknown",
            line,
            msg_buf);

    fflush(stream);
    pthread_mutex_unlock(&g_log_lock);
}
