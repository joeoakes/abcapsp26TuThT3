#define _GNU_SOURCE
#include <errno.h>
#include <microhttpd.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <mongoc/mongoc.h>
#include <bson/bson.h>
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
    char *buf = malloc(n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
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

struct app_config {
    const char *mongo_uri;
    const char *mongo_db;
    const char *mongo_col;
};

static struct app_config config;

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
    const char *sort_s  = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "sort"); /* asc|desc */
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

    if (0 == strcmp(method, "GET") && 0 == strcmp(url, "/moves")) {
        return handle_get_moves(connection);
    }

    if (strcmp(method, "POST") != 0 || strcmp(url, "/move") != 0) {
        return MHD_NO;
    }

    if (*con_cls == NULL) {
        struct connection_info *ci = calloc(1, sizeof(*ci));
        *con_cls = ci;
        return MHD_YES;
    }

    struct connection_info *ci = *con_cls;

    if (*upload_data_size != 0) {
        ci->data = realloc(ci->data, ci->size + *upload_data_size + 1);
        memcpy(ci->data + ci->size, upload_data, *upload_data_size);
        ci->size += *upload_data_size;
        ci->data[ci->size] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }

    /* mTLS: verify client certificate is present AND signed by our CA */
    {
        const char *reject_msg = verify_client_cert(connection);
        if (reject_msg) {
            free(ci->data);
            free(ci);
            *con_cls = NULL;
            return respond_json(connection, MHD_HTTP_FORBIDDEN, reject_msg);
        }
    }

    /* MongoDB insert */
    bson_error_t error;
    bson_t *doc = bson_new_from_json((uint8_t *)ci->data, -1, &error);
    if (!doc) {
        fprintf(stderr, "JSON error: %s\n", error.message);
        free(ci->data);
        free(ci);
        *con_cls = NULL;
        return MHD_NO;
    }

    char ts[64];
    get_utc_iso8601(ts, sizeof(ts));
    BSON_APPEND_UTF8(doc, "received_at", ts);

    mongoc_collection_t *col =
        mongoc_client_get_collection(mongo_client,
                                    config.mongo_db,
                                    config.mongo_col);

    bool inserted = mongoc_collection_insert_one(col, doc, NULL, NULL, &error);
    mongoc_collection_destroy(col);
    bson_destroy(doc);

    enum MHD_Result ret;
    if (!inserted) {
        fprintf(stderr, "MongoDB insert failed: %s\n", error.message);
        ret = respond_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "{\"status\":\"error\",\"message\":\"MongoDB insert failed\"}");
    } else {
        ret = respond_json(connection, MHD_HTTP_OK, "{\"status\":\"ok\"}");
    }

    free(ci->data);
    free(ci);
    *con_cls = NULL;

    return ret;
}

int main(void) {
    config.mongo_uri = getenv("MONGO_URI");
    if (!config.mongo_uri || !*config.mongo_uri)
        config.mongo_uri = DEFAULT_MONGO_URI;

    config.mongo_db = getenv("MONGO_DB");
    if (!config.mongo_db || !*config.mongo_db)
        config.mongo_db = DEFAULT_MONGO_DB;

    config.mongo_col = getenv("MONGO_COL");
    if (!config.mongo_col || !*config.mongo_col)
        config.mongo_col = DEFAULT_MONGO_COL;

    /* cert / key / CA path overrides */
    const char *env;
    env = getenv("CERT_FILE");    if (env && *env) cert_file    = env;
    env = getenv("KEY_FILE");     if (env && *env) key_file     = env;
    env = getenv("CA_CERT_FILE"); if (env && *env) ca_cert_file = env;

    mongoc_init();
    mongo_client = mongoc_client_new(config.mongo_uri);
    if (!mongo_client) {
        fprintf(stderr, "Failed to create MongoDB client\n");
        return 1;
    }

    char *cert_pem     = read_file(cert_file);
    char *key_pem      = read_file(key_file);
    char *ca_cert_pem  = read_file(ca_cert_file);
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
        MHD_OPTION_HTTPS_MEM_CERT,  cert_pem,
        MHD_OPTION_HTTPS_MEM_KEY,   key_pem,
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

    printf("HTTPS mTLS server listening on https://localhost:%d/move\n", DEFAULT_PORT);
    printf("MongoDB: %s  DB=%s  Collection=%s\n",
           config.mongo_uri, config.mongo_db, config.mongo_col);
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
    mongoc_client_destroy(mongo_client);
    mongoc_cleanup();
    return 0;
}
