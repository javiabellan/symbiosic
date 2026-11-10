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
         *auth = fmt("Authorization: Bearer %s", key);
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
static J *meta(Trace *t) {
    return get(root(t->doc), "meta");
}
Doc *projection(App *a, Trace *t, char **body, char **error) {
    *body = NULL;
    J *diag = get(meta(t), "diagnostics");
    if (*sget(diag, "ingestion_error")) {
        *error = xstrdup(sget(diag, "ingestion_error"));
        return NULL;
    }
    Doc *request = doc_new();
    J *r = root(request);
    put(request, r, "model", js(request, a->cfg.decision_model));
    put(request, r, "input", js(request, ""));
    J *cache = obj(request);
    put(request, cache, "mode", js(request, "explicit"));
    put(request, r, "prompt_cache_options", cache);
    put(request, r, "questions", copy(request, get(root(a->rubric), "questions")));
    const char *endpoint;
    Doc *wire = provider_request(&a->cfg, true, r, &endpoint);
    size_t overhead = token_json(root(wire));
    doc_free(wire);
    size_t budget = model_budget(&a->cfg.decision), totals[6];
    Fidelity *f = fidelity_prepare(t);
    for (int l = 1; l <= 6; l++)
        totals[l - 1] = fidelity_tokens(f, l) + overhead;
    int level = fidelity_select(totals, budget);
    if (!level) {
        *error = fmt("context_limit: no complete trace view fits. Level 01 needs %zu estimated "
                     "input tokens; %zu available after output/framing reserves. Choose a larger "
                     "model input limit.",
                     totals[0], budget);
        doc_free(request);
        return NULL;
    }
    Doc *coverage = NULL;
    char *text = fidelity_render(t, level, &coverage);
    put(request, r, "input", js(request, text));
    free(text);
    wire = provider_request(&a->cfg, true, r, &endpoint);
    *body = json(root(wire));
    doc_free(wire);
    doc_free(request);
    put(coverage, root(coverage), "endpoint", js(coverage, endpoint));
    put(coverage, root(coverage), "ai_provider",
        js(coverage, provider_name(a->cfg.decision.provider)));
    put(coverage, root(coverage), "input_tokens", ji(coverage, (int64_t)totals[level - 1]));
    put(coverage, root(coverage), "input_budget", ji(coverage, (int64_t)budget));
    put(coverage, root(coverage), "overhead_tokens", ji(coverage, (int64_t)overhead));
    put(coverage, root(coverage), "request_bytes", ji(coverage, (int64_t)strlen(*body)));
    return coverage;
}
Transport api_post(Config *cfg, const char *endpoint, const char *body) {
    return provider_http(cfg, AI_OPENAI, endpoint, body);
}
static Doc *cached(App *app, Docs *cache, J *entry, const char *model, const char *rubric) {
    for (size_t i = 0; i < cache->n; i++) {
        J *r = root(cache->v[i]);
        if (strcmp(sget(r, "trace_id"), sget(entry, "trace_id")) ||
            integer(get(r, "revision")) != integer(get(entry, "revision")) ||
            integer(get(r, "normalizer_version")) != TV_VERSION || strcmp(sget(r, "model"), model))
            continue;
        Doc *candidate = NULL;
        if (get(r, "request_body"))
            candidate = doc_clone(cache->v[i]);
        else if (valid_id(sget(r, "id")) && valid_id(sget(r, "trace_id"))) {
            char *path = fmt("%s/analysis/%s/%s.json", app->cfg.data_dir, sget(r, "trace_id"),
                             sget(r, "id")),
                 *error = NULL;
            size_t n;
            char *text = read_file(path, MAX_FILE_BYTES, &n, &error);
            if (text)
                candidate = doc_parse(text, n);
            free(text);
            free(error);
            free(path);
        }
        J *full = root(candidate);
        if (candidate && !strcmp(sget(full, "rubric_content"), rubric) &&
            !strcmp(sget(full, "request_body"), sget(entry, "request_body")) &&
            !strcmp(sget(get(full, "coverage"), "ai_provider"),
                    sget(get(entry, "coverage"), "ai_provider")))
            return candidate;
        doc_free(candidate);
    }
    return NULL;
}
Doc *estimate_build(App *a, Snapshot *s, Doc *selection, char **error) {
    Doc *d = doc_new();
    J *r = root(d), *entries = arr(d);
    char id[33];
    if (!new_id(id)) {
        doc_free(d);
        *error = xstrdup("Random ID failed");
        return NULL;
    }
    put(d, r, "id", js(d, id));
    char *now = now_iso();
    put(d, r, "created_at", js(d, now));
    free(now);
    put(d, r, "expires_at_epoch", ji(d, (int64_t)time(NULL) + 600));
    put(d, r, "job_id", js(d, NULL));
    put(d, r, "model", js(d, a->cfg.decision_model));
    put(d, r, "ai_provider", js(d, provider_name(a->cfg.decision.provider)));
    put(d, r, "rate_usd_per_m", jnum(d, a->cfg.rate));
    put(d, r, "price_checked", js(d, PRICE_DATE));
    char *rubric = json(root(a->rubric));
    put(d, r, "rubric_content", js(d, rubric));
    put(d, r, "rubric_version", copy(d, get(root(a->rubric), "version")));
    put(d, r, "entries", entries);
    int64_t eligible = 0, cached_count = 0, excluded = 0, tokens = 0, low = 0, high = 0;
    bool all = boolean(get(root(selection), "all"));
    J *ids = get(root(selection), "trace_ids");
    size_t count = all ? s->traces.n : yyjson_mut_arr_size(ids);
    for (size_t i = 0; i < count; i++) {
        const char *tid = all ? s->traces.v[i]->id : str(yyjson_mut_arr_get(ids, i));
        bool duplicate = false;
        for (size_t j = 0; j < i && !all; j++)
            if (!strcmp(tid, str(yyjson_mut_arr_get(ids, j))))
                duplicate = true;
        if (duplicate)
            continue;
        Trace *t = trace_find(&s->traces, tid);
        J *entry = obj(d);
        add(entries, entry);
        put(d, entry, "trace_id", js(d, tid));
        if (!t) {
            put(d, entry, "status", js(d, "excluded"));
            put(d, entry, "reason", js(d, "Trace not found"));
            excluded++;
            continue;
        }
        put(d, entry, "revision", copy(d, get(meta(t), "revision")));
        put(d, entry, "provider", copy(d, get(meta(t), "provider")));
        put(d, entry, "cwd", copy(d, get(meta(t), "cwd")));
        char *body = NULL, *err = NULL;
        Doc *cov = projection(a, t, &body, &err);
        if (!cov) {
            put(d, entry, "status", js(d, "excluded"));
            put(d, entry, "reason", js(d, err));
            excluded++;
            free(err);
            continue;
        }
        put(d, entry, "request_body", js(d, body));
        put(d, entry, "coverage", copy(d, root(cov)));
        doc_free(cov);
        Doc *hit = cached(a, &s->analyses, entry, a->cfg.decision_model, rubric);
        free(body);
        int64_t estimate = integer(get(get(entry, "coverage"), "input_tokens"));
        put(d, entry, "tokens_estimated", ji(d, estimate));
        put(d, entry, "tokens_low", ji(d, estimate));
        put(d, entry, "tokens_high", ji(d, estimate));
        put(d, entry, "estimated_usd", jnum(d, hit ? 0 : (double)estimate * a->cfg.rate / 1000000));
        if (hit) {
            cached_count++;
            put(d, entry, "status", js(d, "cached"));
            put(d, entry, "analysis_id", copy(d, get(root(hit), "id")));
            doc_free(hit);
        } else {
            eligible++;
            tokens += estimate;
            low += estimate;
            high += estimate;
            put(d, entry, "status", js(d, "eligible"));
        }
    }
    free(rubric);
    put(d, r, "total", ji(d, (int64_t)yyjson_mut_arr_size(entries)));
    put(d, r, "eligible", ji(d, eligible));
    put(d, r, "cached", ji(d, cached_count));
    put(d, r, "excluded", ji(d, excluded));
    put(d, r, "tokens_estimated", ji(d, tokens));
    put(d, r, "tokens_low", ji(d, low));
    put(d, r, "tokens_high", ji(d, high));
    put(d, r, "estimated_usd", jnum(d, (double)tokens * a->cfg.rate / 1000000));
    put(d, r, "estimate_note",
        js(d, "Local tiktoken-c o200k_base section counts plus request overhead. The same "
              "tokenizer is a heuristic for every provider. Input costs exclude any "
              "text-generation output charges. Limits include output and framing reserves. Refresh "
              "after changing model settings."));
    if (!save_doc(a, "quotes", id, d, error)) {
        doc_free(d);
        return NULL;
    }
    return d;
}
static bool unit(J *v) {
    return yyjson_mut_is_num(v) && isfinite(number(v)) && number(v) >= 0 && number(v) <= 1;
}
Doc *interpret_answers(J *response, J *questions, bool incomplete, char **error) {
    Doc *d = doc_new();
    J *answers = get(response, "answers"), *selected = arr(d), *uncertain = arr(d);
    if (!yyjson_mut_is_arr(answers)) {
        *error = xstrdup("Invalid Decisions response: answers missing");
        doc_free(d);
        return NULL;
    }
    size_t i, max;
    J *q;
    yyjson_mut_arr_foreach(questions, i, max, q) {
        const char *name = sget(q, "name"), *type = sget(q, "type");
        J *answer = NULL;
        size_t j, n;
        J *a;
        yyjson_mut_arr_foreach(answers, j, n, a) if (!strcmp(name, sget(a, "name"))) {
            if (answer)
                goto invalid;
            answer = a;
        }
        if (!answer)
            goto invalid;
        J *v = obj(d), *value = js(d, NULL);
        put(d, v, "type", js(d, type));
        put(d, v, "answer", copy(d, answer));
        const char *at = sget(answer, "type");
        if (!strcmp(at, "refusal"))
            put(d, v, "unknown_reason", js(d, "refusal"));
        else if (strcmp(type, at))
            goto invalid;
        else if (!strcmp(type, "predicate")) {
            J *p = get(answer, "probability");
            if (!unit(p))
                goto invalid;
            double probability = number(p);
            if (probability >= 0.8)
                value = jb(d, true);
            else if (probability <= 0.2 && !incomplete)
                value = jb(d, false);
            else
                put(d, v, "unknown_reason",
                    js(d, incomplete && probability <= 0.2
                              ? "incomplete evidence prevents negative conclusion"
                              : "uncertain probability"));
        } else if (!strcmp(type, "choice")) {
            J *confidence = get(answer, "confidence"), *chosen = get(answer, "choice");
            if (!unit(confidence) || !yyjson_mut_is_str(chosen) ||
                !yyjson_mut_is_arr(get(answer, "probabilities")))
                goto invalid;
            bool allowed = false;
            J *option;
            yyjson_mut_arr_foreach(get(q, "choices"), j, n,
                                   option) if (!strcmp(sget(option, "value"), str(chosen)))
                allowed = true;
            if (!allowed)
                goto invalid;
            if (number(confidence) >= 0.6 && strcmp(str(chosen), "unknown") &&
                !(incomplete &&
                  (!strcmp(str(chosen), "none") || !strcmp(str(chosen), "none_observed"))))
                value = copy(d, chosen);
        } else if (!strcmp(type, "score")) {
            J *confidence = get(answer, "confidence"), *score = get(answer, "score");
            if (!unit(confidence) || !yyjson_mut_is_num(score) || number(score) < 0 ||
                number(score) > 4 || !yyjson_mut_is_arr(get(answer, "probabilities")))
                goto invalid;
            if (number(confidence) >= 0.6 && !(incomplete && number(score) == 0))
                value = copy(d, score);
        }
        put(d, v, "value", value);
        put(d, root(d), name, v);
        if (!strncmp(name, "activity_", 9)) {
            if (yyjson_mut_is_true(value))
                add(selected, js(d, name + 9));
            else if (yyjson_mut_is_null(value))
                add(uncertain, js(d, name + 9));
        }
    }
    J *plan = get(get(root(d), "plan_basis"), "value"),
      *implementation = get(get(root(d), "implementation_observed"), "value");
    if (!yyjson_mut_is_str(plan) || !strcmp(str(plan), "none") ||
        !yyjson_mut_is_true(implementation)) {
        J *follow = get(root(d), "plan_following");
        put(d, follow, "value", js(d, NULL));
        put(d, follow, "unknown_reason",
            js(d, "No confident applicable plan and observed implementation"));
    }
    J *activities = obj(d);
    put(d, activities, "selected", selected);
    put(d, activities, "uncertain", uncertain);
    put(d, root(d), "activities", activities);
    return d;
invalid:
    *error = xstrdup("Invalid Decisions response: missing, duplicate or malformed required answer");
    doc_free(d);
    return NULL;
}
Doc *analyze_entry(App *a, J *quote, J *entry, Docs *cache, char **error) {
    Doc *hit = cached(a, cache, entry, sget(quote, "model"), sget(quote, "rubric_content"));
    if (hit) {
        put(hit, root(hit), "reused", jb(hit, true));
        return hit;
    }
    if (!strcmp(sget(entry, "status"), "cached")) {
        *error = xstrdup(
            "Quoted cached result is unavailable; no paid request made. Refresh the estimate.");
        return NULL;
    }
    const char *body = sget(entry, "request_body");
    if (!*body) {
        *error = xstrdup("Invalid frozen request");
        return NULL;
    }
    Doc *d = doc_new();
    J *r = root(d);
    char id[33];
    if (!new_id(id)) {
        doc_free(d);
        *error = xstrdup("Random ID generation failed");
        return NULL;
    }
    put(d, r, "id", js(d, id));
    put(d, r, "trace_id", copy(d, get(entry, "trace_id")));
    put(d, r, "revision", copy(d, get(entry, "revision")));
    put(d, r, "normalizer_version", ji(d, TV_VERSION));
    put(d, r, "rubric_version", copy(d, get(quote, "rubric_version")));
    put(d, r, "rubric_content", copy(d, get(quote, "rubric_content")));
    put(d, r, "model", copy(d, get(quote, "model")));
    char *now = now_iso();
    put(d, r, "created_at", js(d, now));
    free(now);
    put(d, r, "request_body", js(d, body));
    Doc *request = doc_parse(body, strlen(body));
    put(d, r, "request", copy(d, root(request)));
    Provider provider;
    if (!provider_parse(sget(quote, "ai_provider"), &provider))
        provider = AI_OPENAI;
    const char *endpoint = sget(get(entry, "coverage"), "endpoint");
    if (!*endpoint)
        endpoint = "decisions";
    Transport t = provider_http(&a->cfg, provider, endpoint, body);
    put(d, r, "raw_response", js(d, t.body));
    put(d, r, "http_status", ji(d, t.status));
    put(d, r, "coverage", copy(d, get(entry, "coverage")));
    put(d, r, "estimate", copy(d, entry));
    yyjson_mut_obj_remove_str(get(r, "estimate"), "request_body");
    Doc *raw = t.body ? doc_parse(t.body, strlen(t.body)) : NULL;
    Doc *response = provider_response(provider, true, endpoint, root(raw));
    doc_free(raw);
    put(d, r, "response", copy(d, root(response)));
    put(d, r, "usage", copy(d, get(root(response), "usage")));
    Doc *frozen_rubric =
        doc_parse(sget(quote, "rubric_content"), strlen(sget(quote, "rubric_content")));
    char *interpret_error = NULL;
    Doc *values = NULL;
    if (!t.error && response)
        values =
            interpret_answers(root(response), get(root(frozen_rubric), "questions"),
                              boolean(get(get(entry, "coverage"), "incomplete")), &interpret_error);
    doc_free(frozen_rubric);
    if (t.error || !response || !values) {
        put(d, r, "error",
            js(d, t.error           ? t.error
                  : interpret_error ? interpret_error
                                    : "Provider returned invalid JSON"));
        put(d, r, "outcome_unknown", jb(d, t.ambiguous));
        free(interpret_error);
        doc_free(request);
        doc_free(response);
        transport_free(&t);
        return d;
    }
    put(d, r, "values", copy(d, root(values)));
    doc_free(values);
    J *usage = get(r, "usage"), *details = get(usage, "input_tokens_details");
    bool unresolved = strcmp(endpoint, "decisions") ||
                      !yyjson_mut_is_int(get(usage, "input_tokens")) ||
                      integer(get(usage, "input_tokens")) < 0 ||
                      !yyjson_mut_is_int(get(details, "cached_tokens")) ||
                      !yyjson_mut_is_int(get(details, "cache_write_tokens")) ||
                      integer(get(details, "cached_tokens")) != 0 ||
                      integer(get(details, "cache_write_tokens")) != 0;
    put(d, r, "pricing_unresolved", jb(d, unresolved));
    put(d, r, "cost_usd",
        unresolved ? js(d, NULL)
                   : jnum(d, (double)integer(get(usage, "input_tokens")) *
                                 number(get(quote, "rate_usd_per_m")) / 1000000));
    put(d, r, "cost_note", js(d, "App calculation at the frozen Decisions rate; not an invoice"));
    char *dir = fmt("analysis/%s", sget(entry, "trace_id")), *save_error = NULL;
    if (!save_doc(a, dir, id, d, &save_error)) {
        put(d, r, "result_not_saved", jb(d, true));
        put(d, r, "save_error", js(d, save_error));
        free(save_error);
    }
    free(dir);
    doc_free(request);
    doc_free(response);
    transport_free(&t);
    return d;
}
