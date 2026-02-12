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

#define DEFAULT_PORT 8445
#define DEFAULT_MONGO_URI "mongodb://localhost:27017"
#define DEFAULT_MONGO_DB  "maze"
#define DEFAULT_MONGO_COL "moves"

static const char *cert_file    = "certs/server.crt";
static const char *key_file     = "certs/server.key";
static const char *ca_cert_file = "certs/ca.crt";
static mongoc_client_t *mongo_client;
static char *g_ca_cert_pem = NULL;

struct connection_info {
    char *data;
    size_t size;
};

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

static enum MHD_Result handle_post(void *cls,
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

    if (strcmp(method, "POST") != 0 || strcmp(url, "/move") != 0)
        return MHD_NO;

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
        }

        /* Parse and verify the client cert against MazeLab-CA */
        if (!reject_msg) {
            gnutls_x509_crt_t client_crt, ca_crt;
            gnutls_x509_crt_init(&client_crt);
            gnutls_x509_crt_init(&ca_crt);

            gnutls_datum_t ca_datum = {
                .data = (unsigned char *)g_ca_cert_pem,
                .size = strlen(g_ca_cert_pem)
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
        }

        if (reject_msg) {
            free(ci->data);
            free(ci);
            *con_cls = NULL;

            struct MHD_Response *resp = MHD_create_response_from_buffer(
                strlen(reject_msg), (void *)reject_msg, MHD_RESPMEM_PERSISTENT);
            MHD_add_response_header(resp, "Content-Type", "application/json");
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_FORBIDDEN, resp);
            MHD_destroy_response(resp);
            return ret;
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

    mongoc_collection_insert_one(col, doc, NULL, NULL, &error);
    mongoc_collection_destroy(col);
    bson_destroy(doc);

    const char *response = "{\"status\":\"ok\"}";
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(strlen(response),
                                         (void *)response,
                                         MHD_RESPMEM_PERSISTENT);

    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);

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
        &handle_post, NULL,
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
