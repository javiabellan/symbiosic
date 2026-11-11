#include "app.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifdef TRACEVIEW_TEST
TestTransport test_transport = NULL;
#endif
static size_t receive(void *ptr, size_t size, size_t n, void *ctx) {
    Buf *b = ctx;
    if (size && n > SIZE_MAX / size)
        return 0;
    size_t bytes = size * n;
    if (b->n + bytes > 8u * 1024 * 1024)
        return 0;
    buf_addn(b, ptr, bytes);
    return bytes;
}
Transport provider_http(Config *cfg, Provider provider, const char *endpoint, const char *body) {
    const char *key = provider_key(cfg, provider);
#ifdef TRACEVIEW_TEST
    if (test_transport)
        return test_transport(endpoint, body, key);
#endif
    Transport t = {0};
    if (!*key) {
        t.error = xstrdup("Provider API key is missing; paid operations are disabled");
        return t;
    }
    CURL *c = curl_easy_init();
    if (!c) {
        t.error = xstrdup("Unable to initialize libcurl");
        return t;
    }
    const char *bases[] = {"https://api.openai.com/v1", "https://api.anthropic.com/v1",
                           "https://openrouter.ai/api/v1",
                           "https://generativelanguage.googleapis.com/v1beta/openai"};
    char *url = fmt("%s/%s", bases[provider], endpoint),
         *auth = provider == AI_ANTHROPIC ? fmt("x-api-key: %s", key) : fmt("Authorization: Bearer %s", key);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth);
    if (provider == AI_ANTHROPIC)
        headers = curl_slist_append(headers, "anthropic-version: 2026-10-21");
    free(auth);
    Buf b = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    if (body) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(body));
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    CURLcode code = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    t.status = (int)status;
    t.body = b.s ? b.s : xstrdup("");
    if (code != CURLE_OK) {
        t.error = fmt("Provider transport failed (%s); outcome unknown. No automatic retry.",
                      curl_easy_strerror(code));
        t.ambiguous = true;
    } else if (status < 200 ||
               status >= 300) { /* Do not echo provider text: it may quote input or credentials. */
        t.error = fmt("Provider HTTP %ld; check model availability, key permissions, quota and "
                      "request settings. No retry was made.",
                      status);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    free(url);
    return t;
}
void transport_free(Transport *t) {
    free(t->body);
    free(t->error);
    memset(t, 0, sizeof(*t));
}
Transport api_post(Config *cfg, const char *endpoint, const char *body) {
    return provider_http(cfg, AI_OPENAI, endpoint, body);
}
