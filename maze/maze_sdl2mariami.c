static void send_json_mtls(const char *json) {
    CURL *curl = curl_easy_init();
    if (!curl) return;

    struct curl_slist *headers = NULL;
    struct curl_slist *resolve = NULL;

    // Set Content-Type header
    headers = curl_slist_append(headers, "Content-Type: application/json");

    // Map hostname to LAN IP for TLS verification
    resolve = curl_slist_append(resolve, "ABINGTO-SBSEGML:8443:192.168.1.188");

    // Set URL using hostname that matches CN in certificate
    curl_easy_setopt(curl, CURLOPT_URL, "https://ABINGTO-SBSEGML:8443/move");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);

    // Paths to your certificates (in /home/mariami/mtls/)
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/home/mariami/mtls/ca.crt");
    curl_easy_setopt(curl, CURLOPT_SSLCERT, "/home/mariami/mtls/pi.crt");
    curl_easy_setopt(curl, CURLOPT_SSLKEY, "/home/mariami/mtls/pi.key");

    // Enable verification
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    // POST data
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L); // 2-second timeout

    // Perform the request
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl POST failed: %s\n", curl_easy_strerror(res));
    }

    // Clean up
    curl_slist_free_all(resolve);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}
