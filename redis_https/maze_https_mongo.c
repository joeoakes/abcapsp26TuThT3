#define _GNU_SOURCE
#include <errno.h>
#include <microhttpd.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <bson/bson.h>
#include <hiredis/hiredis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int signo) {
    (void)signo;
    g_stop = 1;
}

#define DEFAULT_PORT 8445
#define DEFAULT_REDIS_HOST "127.0.0.1"
#define DEFAULT_REDIS_PORT 6379
#define DEFAULT_KEY_PREFIX "team3ttmission"

static const char *cert_file    = "certs/server.crt";
static const char *key_file     = "certs/server.key";
static const char *ca_cert_file = "certs/ca.crt";
static char *g_ca_cert_pem = NULL;

struct connection_info {
    char *data;
    size_t size;
};

struct app_config {
    const char *redis_host;
    int redis_port;
    const char *key_prefix;
};

static struct app_config config;

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static enum MHD_Result respond_json(struct MHD_Connection *connection,
                                    unsigned int status,
                                    const char *json)
{
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(strlen(json), (void *)json, MHD_RESPMEM_MUST_COPY);
    if (!resp) return MHD_NO;
    MHD_add_response_header(resp, "Content-Type", "application/json; charset=utf-8");
    enum MHD_Result ret = MHD_queue_response(connection, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

static int body_append(struct connection_info *ci, const char *chunk, size_t chunk_size) {
    if (chunk_size == 0) return 1;
    char *nd = realloc(ci->data, ci->size + chunk_size + 1);
    if (!nd) return 0;
    ci->data = nd;
    memcpy(ci->data + ci->size, chunk, chunk_size);
    ci->size += chunk_size;
    ci->data[ci->size] = '\0';
    return 1;
}

static int64_t now_epoch_seconds(void) {
    return (int64_t)time(NULL);
}

static const char *get_utf8_or(const bson_t *doc, const char *key, const char *defv) {
    bson_iter_t it;
    if (bson_iter_init_find(&it, doc, key) && BSON_ITER_HOLDS_UTF8(&it)) {
        return bson_iter_utf8(&it, NULL);
    }
    return defv;
}

static int64_t get_i64_or(const bson_t *doc, const char *key, int64_t defv) {
    bson_iter_t it;
    if (!bson_iter_init_find(&it, doc, key)) return defv;

    if (BSON_ITER_HOLDS_INT32(&it)) return (int64_t)bson_iter_int32(&it);
    if (BSON_ITER_HOLDS_INT64(&it)) return bson_iter_int64(&it);
    if (BSON_ITER_HOLDS_DOUBLE(&it)) return (int64_t)bson_iter_double(&it);
    return defv;
}

static double get_double_or(const bson_t *doc, const char *key, double defv) {
    bson_iter_t it;
    if (!bson_iter_init_find(&it, doc, key)) return defv;

    if (BSON_ITER_HOLDS_DOUBLE(&it)) return bson_iter_double(&it);
    if (BSON_ITER_HOLDS_INT32(&it)) return (double)bson_iter_int32(&it);
    if (BSON_ITER_HOLDS_INT64(&it)) return (double)bson_iter_int64(&it);
    return defv;
}

static const char *verify_client_cert(struct MHD_Connection *connection) {
    const char *reject_msg = NULL;

    const union MHD_ConnectionInfo *conn_info =
        MHD_get_connection_info(connection, MHD_CONNECTION_INFO_GNUTLS_SESSION);
    gnutls_session_t session = conn_info ?
        (gnutls_session_t)conn_info->tls_session : NULL;
    unsigned int cert_count = 0;
    const gnutls_datum_t *peer_certs = session ?
        gnutls_certificate_get_peers(session, &cert_count) : NULL;

    if (!peer_certs || cert_count == 0) {
        reject_msg = "{\"error\":\"client certificate required\"}\n";
        fprintf(stderr, "mTLS: no client certificate presented\n");
        return reject_msg;
    }

    gnutls_x509_crt_t client_crt, ca_crt;
    gnutls_x509_crt_init(&client_crt);
    gnutls_x509_crt_init(&ca_crt);

    gnutls_datum_t ca_datum = {
        .data = (unsigned char *)g_ca_cert_pem,
        .size = (unsigned int)strlen(g_ca_cert_pem)
    };

    int imp_client = gnutls_x509_crt_import(client_crt, &peer_certs[0], GNUTLS_X509_FMT_DER);
    int imp_ca     = gnutls_x509_crt_import(ca_crt,     &ca_datum,      GNUTLS_X509_FMT_PEM);

    if (imp_client < 0 || imp_ca < 0) {
        reject_msg = "{\"error\":\"failed to parse certificate\"}\n";
        fprintf(stderr, "mTLS: cert parse error (client=%d ca=%d)\n", imp_client, imp_ca);
    } else {
        unsigned int verify_status = 0;
        int vret = gnutls_x509_crt_verify(client_crt, &ca_crt, 1, 0, &verify_status);
        if (vret < 0 || verify_status != 0) {
            reject_msg = "{\"error\":\"client certificate not signed by trusted CA\"}\n";
            fprintf(stderr, "mTLS: CA verification failed (vret=%d status=%u)\n", vret, verify_status);
        }
    }

    gnutls_x509_crt_deinit(client_crt);
    gnutls_x509_crt_deinit(ca_crt);
    return reject_msg;
}

static enum MHD_Result handle_post_mission(struct MHD_Connection *connection,
                                           const char *body)
{
    if (!body || !*body) {
        return respond_json(connection, MHD_HTTP_BAD_REQUEST,
                            "{\"ok\":false,\"error\":\"empty request body\"}\n");
    }

    const char *reject_msg = verify_client_cert(connection);
    if (reject_msg) return respond_json(connection, MHD_HTTP_FORBIDDEN, reject_msg);

    bson_error_t bson_err;
    bson_t *doc = bson_new_from_json((const uint8_t *)body, -1, &bson_err);
    if (!doc) {
        char out[512];
        snprintf(out, sizeof(out),
                 "{\"ok\":false,\"error\":\"invalid JSON: %s\"}\n", bson_err.message);
        return respond_json(connection, MHD_HTTP_BAD_REQUEST, out);
    }

    const char *mission_id = get_utf8_or(doc, "mission_id", NULL);
    if (!mission_id || !*mission_id) {
        bson_destroy(doc);
        return respond_json(connection, MHD_HTTP_BAD_REQUEST,
                            "{\"ok\":false,\"error\":\"mission_id is required\"}\n");
    }

    const char *robot_id = get_utf8_or(doc, "robot_id", "UNKNOWN_ROBOT");
    const char *mission_type = get_utf8_or(doc, "mission_type", "unknown");
    const char *mission_result = get_utf8_or(doc, "mission_result", "unknown");
    const char *abort_reason = get_utf8_or(doc, "abort_reason", "");

    int64_t start_time = get_i64_or(doc, "start_time", now_epoch_seconds());
    int64_t end_time = get_i64_or(doc, "end_time", start_time);

    int64_t moves_left_turn = get_i64_or(doc, "moves_left_turn", 0);
    int64_t moves_right_turn = get_i64_or(doc, "moves_right_turn", 0);
    int64_t moves_straight = get_i64_or(doc, "moves_straight", 0);
    int64_t moves_reverse = get_i64_or(doc, "moves_reverse", 0);
    int64_t moves_total = get_i64_or(doc, "moves_total",
                                     moves_left_turn + moves_right_turn + moves_straight + moves_reverse);

    double distance_traveled = get_double_or(doc, "distance_traveled", 0.0);
    int64_t duration_seconds = get_i64_or(doc, "duration_seconds", end_time - start_time);

    if (duration_seconds < 0) duration_seconds = 0;

    char redis_key[256];
    snprintf(redis_key, sizeof(redis_key), "%s:%s", config.key_prefix, mission_id);

    redisContext *rctx = redisConnect(config.redis_host, config.redis_port);
    if (!rctx || rctx->err) {
        char out[512];
        const char *errstr = (rctx && rctx->errstr) ? rctx->errstr : "failed to connect";
        snprintf(out, sizeof(out),
                 "{\"ok\":false,\"error\":\"redis connect failed: %s\"}\n", errstr);
        if (rctx) redisFree(rctx);
        bson_destroy(doc);
        return respond_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, out);
    }

    redisReply *reply = (redisReply *)redisCommand(
        rctx,
        "HSET %s "
        "mission_id %s "
        "robot_id %s "
        "mission_type %s "
        "start_time %lld "
        "end_time %lld "
        "moves_left_turn %lld "
        "moves_right_turn %lld "
        "moves_straight %lld "
        "moves_reverse %lld "
        "moves_total %lld "
        "distance_traveled %f "
        "duration_seconds %lld "
        "mission_result %s "
        "abort_reason %s",
        redis_key,
        mission_id,
        robot_id,
        mission_type,
        (long long)start_time,
        (long long)end_time,
        (long long)moves_left_turn,
        (long long)moves_right_turn,
        (long long)moves_straight,
        (long long)moves_reverse,
        (long long)moves_total,
        distance_traveled,
        (long long)duration_seconds,
        mission_result,
        abort_reason
    );

    if (!reply) {
        char out[512];
        snprintf(out, sizeof(out),
                 "{\"ok\":false,\"error\":\"redis command failed: %s\"}\n",
                 rctx->errstr ? rctx->errstr : "unknown");
        redisFree(rctx);
        bson_destroy(doc);
        return respond_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, out);
    }

    freeReplyObject(reply);
    redisFree(rctx);
    bson_destroy(doc);

    char out[512];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"key\":\"%s\"}\n",
             redis_key);
    return respond_json(connection, MHD_HTTP_OK, out);
}

static enum MHD_Result request_handler(void *cls,
                       struct MHD_Connection *connection,
                       const char *url,
                       const char *method,
                       const char *version,
                       const char *upload_data,
                       size_t *upload_data_size,
                       void **con_cls)
{
    (void)version;
    (void)cls;

    const int is_mission_post =
        (0 == strcmp(method, "POST")) &&
        ((0 == strcmp(url, "/mission")) || (0 == strcmp(url, "/move")));

    if (0 == strcmp(method, "GET") && 0 == strcmp(url, "/health")) {
        return respond_json(connection, MHD_HTTP_OK, "{\"ok\":true}\n");
    }

    if (!is_mission_post) {
        return respond_json(connection, MHD_HTTP_NOT_FOUND,
                            "{\"ok\":false,\"error\":\"not found\"}\n");
    }

    if (*con_cls == NULL) {
        struct connection_info *ci = calloc(1, sizeof(*ci));
        if (!ci) {
            return respond_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                "{\"ok\":false,\"error\":\"out of memory\"}\n");
        }
        *con_cls = ci;
        return MHD_YES;
    }

    struct connection_info *ci = (struct connection_info *)(*con_cls);

    if (*upload_data_size != 0) {
        if (!body_append(ci, upload_data, *upload_data_size)) {
            free(ci->data);
            free(ci);
            *con_cls = NULL;
            return respond_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                "{\"ok\":false,\"error\":\"out of memory\"}\n");
        }
        *upload_data_size = 0;
        return MHD_YES;
    }

    enum MHD_Result ret = handle_post_mission(connection, ci->data ? ci->data : "");

    free(ci->data);
    free(ci);
    *con_cls = NULL;

    return ret;
}

int main(void) {
    const char *env;

    config.redis_host = getenv("REDIS_HOST");
    if (!config.redis_host || !*config.redis_host)
        config.redis_host = DEFAULT_REDIS_HOST;

    env = getenv("REDIS_PORT");
    config.redis_port = (env && *env) ? atoi(env) : DEFAULT_REDIS_PORT;
    if (config.redis_port <= 0 || config.redis_port > 65535)
        config.redis_port = DEFAULT_REDIS_PORT;

    config.key_prefix = getenv("REDIS_KEY_PREFIX");
    if (!config.key_prefix || !*config.key_prefix)
        config.key_prefix = DEFAULT_KEY_PREFIX;

    /* cert / key / CA path overrides */
    env = getenv("CERT_FILE");    if (env && *env) cert_file    = env;
    env = getenv("KEY_FILE");     if (env && *env) key_file     = env;
    env = getenv("CA_CERT_FILE"); if (env && *env) ca_cert_file = env;

    char *cert_pem     = read_file(cert_file);
    char *key_pem      = read_file(key_file);
    char *ca_cert_pem  = read_file(ca_cert_file);
    g_ca_cert_pem = ca_cert_pem;

    if (!cert_pem || !key_pem || !ca_cert_pem) {
        fprintf(stderr, "Failed to read cert/key/CA files\n");
        free(cert_pem);
        free(key_pem);
        free(ca_cert_pem);
        return 1;
    }

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_TLS,
        DEFAULT_PORT,
        NULL, NULL,
        &request_handler, NULL,
        MHD_OPTION_HTTPS_MEM_CERT,  cert_pem,
        MHD_OPTION_HTTPS_MEM_KEY,   key_pem,
        MHD_OPTION_HTTPS_MEM_TRUST, ca_cert_pem,
        MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr, "Failed to start HTTPS server\n");
        free(cert_pem);
        free(key_pem);
        free(ca_cert_pem);
        return 1;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    printf("HTTPS mTLS Redis server listening on https://localhost:%d/mission\n", DEFAULT_PORT);
    printf("Compat endpoint also enabled: https://localhost:%d/move\n", DEFAULT_PORT);
    printf("Redis: %s:%d  key_prefix=%s\n",
           config.redis_host, config.redis_port, config.key_prefix);
    fflush(stdout);

    while (!g_stop) {
        struct timespec ts = {0, 200000000L}; /* 200 ms */
        nanosleep(&ts, NULL);
    }

    printf("\nShutting down...\n");
    MHD_stop_daemon(daemon);
    free(cert_pem);
    free(key_pem);
    free(ca_cert_pem);
    return 0;
}
