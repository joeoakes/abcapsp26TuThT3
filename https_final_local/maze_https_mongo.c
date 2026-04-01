#define _GNU_SOURCE
#include <errno.h>
#include <microhttpd.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <mongoc/mongoc.h>
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

#define DEFAULT_LIST_LIMIT 100
#define MAX_LIST_LIMIT 1000
#define MAX_LIST_BYTES (1024 * 1024) /* 1 MiB */

#define DEFAULT_PORT 8445
#define DEFAULT_MONGO_URI "mongodb://localhost:27017"
#define DEFAULT_MONGO_DB  "maze"
#define DEFAULT_MONGO_COL "team3ttmoves"

#define DEFAULT_REDIS_HOST "127.0.0.1"
#define DEFAULT_REDIS_PORT 6379
#define DEFAULT_REDIS_KEY_PREFIX "team3ttmission"

static const char *cert_file    = "certs/server.crt";
static const char *key_file     = "certs/server.key";
static const char *ca_cert_file = "certs/ca.crt";

static mongoc_client_t *mongo_client;
static char *g_ca_cert_pem = NULL;

struct connection_info {
    char *data;
    size_t size;
};

struct strbuf {
    char *data;
    size_t size;
    size_t cap;
};

struct app_config {
    const char *mongo_uri;
    const char *mongo_db;
    const char *mongo_col;
    const char *redis_host;
    int redis_port;
    const char *redis_key_prefix;
};

static struct app_config config;

static void strbuf_free(struct strbuf *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->size = b->cap = 0;
}

static int strbuf_append(struct strbuf *b, const char *s, size_t n) {
    if (n == 0) return 1;
    if (b->size + n + 1 > b->cap) {
        size_t newcap = b->cap ? b->cap : 1024;
        while (newcap < b->size + n + 1) newcap *= 2;
        char *nd = realloc(b->data, newcap);
        if (!nd) return 0;
        b->data = nd;
        b->cap = newcap;
    }
    memcpy(b->data + b->size, s, n);
    b->size += n;
    b->data[b->size] = '\0';
    return 1;
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

static long parse_long_clamped(const char *s, long defv, long minv, long maxv) {
    if (!s || !*s) return defv;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return defv;
    if (v < minv) return minv;
    if (v > maxv) return maxv;
    return v;
}

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

static void get_utc_iso8601(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
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
        reject_msg = "{\"error\":\"client certificate required\"}";
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
        reject_msg = "{\"error\":\"failed to parse certificate\"}";
        fprintf(stderr, "mTLS: cert parse error (client=%d ca=%d)\n", imp_client, imp_ca);
    } else {
        unsigned int verify_status = 0;
        int vret = gnutls_x509_crt_verify(client_crt, &ca_crt, 1, 0, &verify_status);
        if (vret < 0 || verify_status != 0) {
            reject_msg = "{\"error\":\"client certificate not signed by trusted CA\"}";
            fprintf(stderr, "mTLS: CA verification failed (vret=%d status=%u)\n", vret, verify_status);
        }
    }

    gnutls_x509_crt_deinit(client_crt);
    gnutls_x509_crt_deinit(ca_crt);
    return reject_msg;
}

static enum MHD_Result handle_get_moves(struct MHD_Connection *connection) {
    const char *reject_msg = verify_client_cert(connection);
    if (reject_msg) return respond_json(connection, MHD_HTTP_FORBIDDEN, reject_msg);

    const char *limit_s = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "limit");
    const char *sort_s  = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "sort");
    const char *session_id = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "session_id");

    long limit = parse_long_clamped(limit_s, DEFAULT_LIST_LIMIT, 1, MAX_LIST_LIMIT);
    int sort_dir = (sort_s && (0 == strcmp(sort_s, "asc") || 0 == strcmp(sort_s, "ASC"))) ? 1 : -1;

    bson_t filter;
    bson_init(&filter);
    if (session_id && *session_id) {
        BSON_APPEND_UTF8(&filter, "session_id", session_id);
    }

    bson_t opts;
    bson_init(&opts);
    BSON_APPEND_INT64(&opts, "limit", (int64_t)limit);

    bson_t sort;
    bson_init(&sort);
    BSON_APPEND_INT32(&sort, "_id", sort_dir);
    BSON_APPEND_DOCUMENT(&opts, "sort", &sort);

    mongoc_collection_t *col =
        mongoc_client_get_collection(mongo_client, config.mongo_db, config.mongo_col);
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, &filter, &opts, NULL);

    struct strbuf out = {0};
    int ok = 1;
    int truncated = 0;
    int count = 0;

    ok = ok && strbuf_append(&out, "{\"ok\":true,\"moves\":[", strlen("{\"ok\":true,\"moves\":["));

    const bson_t *doc;
    while (ok && mongoc_cursor_next(cursor, &doc)) {
        size_t json_len = 0;
        char *json = bson_as_relaxed_extended_json(doc, &json_len);
        if (!json) { ok = 0; break; }

        if (out.size + (count ? 1 : 0) + json_len + 2 > MAX_LIST_BYTES) {
            bson_free(json);
            truncated = 1;
            break;
        }

        if (count > 0) ok = ok && strbuf_append(&out, ",", 1);
        ok = ok && strbuf_append(&out, json, json_len);
        bson_free(json);
        count++;
    }

    bson_error_t err;
    if (mongoc_cursor_error(cursor, &err)) ok = 0;

    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(col);
    bson_destroy(&sort);
    bson_destroy(&opts);
    bson_destroy(&filter);

    char numbuf[32];
    ok = ok && strbuf_append(&out, "],\"count\":", strlen("],\"count\":"));
    int nw = snprintf(numbuf, sizeof(numbuf), "%d", count);
    if (nw < 0) ok = 0;
    ok = ok && strbuf_append(&out, numbuf, (size_t)nw);
    ok = ok && strbuf_append(&out, ",\"truncated\":", strlen(",\"truncated\":"));
    ok = ok && strbuf_append(&out, truncated ? "true" : "false", truncated ? 4 : 5);
    ok = ok && strbuf_append(&out, "}\n", 2);

    if (!ok) {
        strbuf_free(&out);
        return respond_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "{\"ok\":false,\"error\":\"failed to query moves\"}\n");
    }

    enum MHD_Result ret = respond_json(connection, MHD_HTTP_OK, out.data ? out.data : "{}\n");
    strbuf_free(&out);
    return ret;
}

static enum MHD_Result handle_post_move(const char *body, struct MHD_Connection *connection) {
    if (!body || !*body) {
        return respond_json(connection, MHD_HTTP_BAD_REQUEST,
                            "{\"ok\":false,\"error\":\"empty request body\"}\n");
    }

    const char *reject_msg = verify_client_cert(connection);
    if (reject_msg) return respond_json(connection, MHD_HTTP_FORBIDDEN, reject_msg);

    bson_error_t error;
    bson_t *doc = bson_new_from_json((const uint8_t *)body, -1, &error);
    if (!doc) {
        char out[512];
        snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"invalid JSON: %s\"}\n", error.message);
        return respond_json(connection, MHD_HTTP_BAD_REQUEST, out);
    }

    char ts[64];
    get_utc_iso8601(ts, sizeof(ts));
    BSON_APPEND_UTF8(doc, "received_at", ts);

    mongoc_collection_t *col =
        mongoc_client_get_collection(mongo_client, config.mongo_db, config.mongo_col);

    if (!mongoc_collection_insert_one(col, doc, NULL, NULL, &error)) {
        char out[512];
        snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"MongoDB insert failed: %s\"}\n", error.message);
        mongoc_collection_destroy(col);
        bson_destroy(doc);
        return respond_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, out);
    }

    mongoc_collection_destroy(col);
    bson_destroy(doc);
    return respond_json(connection, MHD_HTTP_OK, "{\"ok\":true}\n");
}

static enum MHD_Result handle_post_mission(const char *body, struct MHD_Connection *connection) {
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
    snprintf(redis_key, sizeof(redis_key), "%s:%s", config.redis_key_prefix, mission_id);

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
    snprintf(out, sizeof(out), "{\"ok\":true,\"key\":\"%s\"}\n", redis_key);
    return respond_json(connection, MHD_HTTP_OK, out);
}

static enum MHD_Result handle_get_mission(struct MHD_Connection *connection) {
    const char *reject_msg = verify_client_cert(connection);
    if (reject_msg) return respond_json(connection, MHD_HTTP_FORBIDDEN, reject_msg);

    const char *mission_id =
        MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "mission_id");
    if (!mission_id || !*mission_id) {
        return respond_json(connection, MHD_HTTP_BAD_REQUEST,
                            "{\"ok\":false,\"error\":\"mission_id query param is required\"}\n");
    }

    char redis_key[256];
    snprintf(redis_key, sizeof(redis_key), "%s:%s", config.redis_key_prefix, mission_id);

    redisContext *rctx = redisConnect(config.redis_host, config.redis_port);
    if (!rctx || rctx->err) {
        char out[512];
        const char *errstr = (rctx && rctx->errstr) ? rctx->errstr : "failed to connect";
        snprintf(out, sizeof(out),
                 "{\"ok\":false,\"error\":\"redis connect failed: %s\"}\n", errstr);
        if (rctx) redisFree(rctx);
        return respond_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, out);
    }

    redisReply *reply = (redisReply *)redisCommand(rctx, "HGETALL %s", redis_key);
    if (!reply) {
        char out[512];
        snprintf(out, sizeof(out),
                 "{\"ok\":false,\"error\":\"redis command failed: %s\"}\n",
                 rctx->errstr ? rctx->errstr : "unknown");
        redisFree(rctx);
        return respond_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, out);
    }

    if (reply->type != REDIS_REPLY_ARRAY || reply->elements == 0) {
        freeReplyObject(reply);
        redisFree(rctx);
        return respond_json(connection, MHD_HTTP_NOT_FOUND,
                            "{\"ok\":false,\"error\":\"mission not found\"}\n");
    }

    bson_t out_doc;
    bson_init(&out_doc);
    BSON_APPEND_BOOL(&out_doc, "ok", true);
    BSON_APPEND_UTF8(&out_doc, "key", redis_key);

    bson_t mission_doc;
    BSON_APPEND_DOCUMENT_BEGIN(&out_doc, "mission", &mission_doc);
    for (size_t i = 0; i + 1 < reply->elements; i += 2) {
        redisReply *k = reply->element[i];
        redisReply *v = reply->element[i + 1];
        if (k->type != REDIS_REPLY_STRING || v->type != REDIS_REPLY_STRING) continue;
        bson_append_utf8(&mission_doc, k->str, -1, v->str, -1);
    }
    bson_append_document_end(&out_doc, &mission_doc);

    size_t json_len = 0;
    char *json = bson_as_relaxed_extended_json(&out_doc, &json_len);
    bson_destroy(&out_doc);
    freeReplyObject(reply);
    redisFree(rctx);

    if (!json) {
        return respond_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                            "{\"ok\":false,\"error\":\"failed to build response\"}\n");
    }

    enum MHD_Result ret = respond_json(connection, MHD_HTTP_OK, json);
    bson_free(json);
    return ret;
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

    if (0 == strcmp(method, "GET") && 0 == strcmp(url, "/health")) {
        return respond_json(connection, MHD_HTTP_OK, "{\"ok\":true}\n");
    }
    if (0 == strcmp(method, "GET") && 0 == strcmp(url, "/moves")) {
        return handle_get_moves(connection);
    }
    if (0 == strcmp(method, "GET") && 0 == strcmp(url, "/mission")) {
        return handle_get_mission(connection);
    }

    const int is_move_post = (0 == strcmp(method, "POST") && 0 == strcmp(url, "/move"));
    const int is_mission_post = (0 == strcmp(method, "POST") && 0 == strcmp(url, "/mission"));

    if (!is_move_post && !is_mission_post) {
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

    enum MHD_Result ret = MHD_NO;
    if (is_move_post) {
        ret = handle_post_move(ci->data ? ci->data : "", connection);
    } else if (is_mission_post) {
        ret = handle_post_mission(ci->data ? ci->data : "", connection);
    }

    free(ci->data);
    free(ci);
    *con_cls = NULL;

    return ret;
}

int main(void) {
    const char *env;

    config.mongo_uri = getenv("MONGO_URI");
    if (!config.mongo_uri || !*config.mongo_uri)
        config.mongo_uri = DEFAULT_MONGO_URI;

    config.mongo_db = getenv("MONGO_DB");
    if (!config.mongo_db || !*config.mongo_db)
        config.mongo_db = DEFAULT_MONGO_DB;

    config.mongo_col = getenv("MONGO_COL");
    if (!config.mongo_col || !*config.mongo_col)
        config.mongo_col = DEFAULT_MONGO_COL;

    config.redis_host = getenv("REDIS_HOST");
    if (!config.redis_host || !*config.redis_host)
        config.redis_host = DEFAULT_REDIS_HOST;

    env = getenv("REDIS_PORT");
    config.redis_port = (env && *env) ? atoi(env) : DEFAULT_REDIS_PORT;
    if (config.redis_port <= 0 || config.redis_port > 65535)
        config.redis_port = DEFAULT_REDIS_PORT;

    config.redis_key_prefix = getenv("REDIS_KEY_PREFIX");
    if (!config.redis_key_prefix || !*config.redis_key_prefix)
        config.redis_key_prefix = DEFAULT_REDIS_KEY_PREFIX;

    env = getenv("CERT_FILE");    if (env && *env) cert_file    = env;
    env = getenv("KEY_FILE");     if (env && *env) key_file     = env;
    env = getenv("CA_CERT_FILE"); if (env && *env) ca_cert_file = env;

    mongoc_init();
    mongo_client = mongoc_client_new(config.mongo_uri);
    if (!mongo_client) {
        fprintf(stderr, "Failed to create MongoDB client\n");
        mongoc_cleanup();
        return 1;
    }

    char *cert_pem = read_file(cert_file);
    char *key_pem = read_file(key_file);
    char *ca_cert_pem = read_file(ca_cert_file);
    g_ca_cert_pem = ca_cert_pem;

    if (!cert_pem || !key_pem || !ca_cert_pem) {
        fprintf(stderr, "Failed to read cert/key/CA files\n");
        free(cert_pem);
        free(key_pem);
        free(ca_cert_pem);
        mongoc_client_destroy(mongo_client);
        mongoc_cleanup();
        return 1;
    }

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_TLS,
        DEFAULT_PORT,
        NULL, NULL,
        &request_handler, NULL,
        MHD_OPTION_HTTPS_MEM_CERT, cert_pem,
        MHD_OPTION_HTTPS_MEM_KEY, key_pem,
        MHD_OPTION_HTTPS_MEM_TRUST, ca_cert_pem,
        MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr, "Failed to start HTTPS server\n");
        free(cert_pem);
        free(key_pem);
        free(ca_cert_pem);
        mongoc_client_destroy(mongo_client);
        mongoc_cleanup();
        return 1;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    printf("HTTPS mTLS final server listening on https://localhost:%d\n", DEFAULT_PORT);
    printf("Telemetry (Mongo): POST /move, GET /moves\n");
    printf("Mission (Redis):   POST /mission, GET /mission?mission_id=<id>\n");
    printf("Health:            GET /health\n");
    printf("MongoDB: %s DB=%s Collection=%s\n", config.mongo_uri, config.mongo_db, config.mongo_col);
    printf("Redis: %s:%d key_prefix=%s\n", config.redis_host, config.redis_port, config.redis_key_prefix);
    fflush(stdout);

    while (!g_stop) {
        struct timespec ts = {0, 200000000L};
        nanosleep(&ts, NULL);
    }

    printf("\nShutting down...\n");
    MHD_stop_daemon(daemon);
    free(cert_pem);
    free(key_pem);
    free(ca_cert_pem);
    mongoc_client_destroy(mongo_client);
    mongoc_cleanup();
    return 0;
}
