#include "app.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

typedef enum { TASK_ESTIMATE, TASK_BATCH, TASK_CHAT, TASK_MODELS } TaskKind;
struct Task {
    uv_work_t work;
    App *app;
    TaskKind kind;
    Task *next;
    Snapshot snapshot;
    Doc *selection, *quote, *job, *chat, *result;
    char *text, *error;
    size_t index;
    Reply reply;
    void *reply_ctx;
};
typedef struct {
    uv_work_t work;
    App *app;
    Traces before, after;
    char *error;
} Ingest;
static J *meta(Trace *t) {
    return get(root(t->doc), "meta");
}
static void persist_job(App *a, Doc *job) {
    char *e = NULL;
    if (!save_doc(a, "jobs", sget(root(job), "id"), job, &e)) {
        put(job, root(job), "result_not_saved", jb(job, true));
        put(job, root(job), "save_error", js(job, e));
        fprintf(stderr, "traceview: job not saved: %s\n", e);
        free(e);
    }
}
static void ingest_worker(uv_work_t *w) {
    Ingest *in = w->data;
    discover(in->app, &in->before, &in->after, &in->error);
}
static void ingest_done(uv_work_t *w, int status) {
    Ingest *in = w->data;
    App *a = in->app;
    if (status == 0) {
        bool changed = a->traces.n != in->after.n;
        for (size_t i = 0; i < in->after.n && !changed; i++)
            if (a->traces.v[i] != in->after.v[i])
                changed = true;
        traces_free(&a->traces);
        a->traces = in->after;
        memset(&in->after, 0, sizeof(in->after));
        if (changed)
            a->catalog_revision++;
        free(a->discovery_error);
        a->discovery_error = in->error;
        in->error = NULL;
    }
    traces_free(&in->before);
    traces_free(&in->after);
    free(in->error);
    free(in);
    a->ingest_busy = false;
    if (a->ingest_pending && !a->stopping) {
        a->ingest_pending = false;
        schedule_discovery(a);
    }
}
void schedule_discovery(App *a) {
    if (a->stopping)
        return;
    if (a->ingest_busy) {
        a->ingest_pending = true;
        return;
    }
    Ingest *in = xcalloc(1, sizeof(*in));
    in->app = a;
    in->work.data = in;
    for (size_t i = 0; i < a->traces.n; i++) {
        trace_retain(a->traces.v[i]);
        traces_push(&in->before, a->traces.v[i]);
    }
    a->ingest_busy = true;
    int rc = uv_queue_work(&a->loop, &in->work, ingest_worker, ingest_done);
    if (rc) {
        free(a->discovery_error);
        a->discovery_error = xstrdup(uv_strerror(rc));
        ingest_done(&in->work, rc);
    }
}
static void task_free(Task *t) {
    snapshot_free(&t->snapshot);
    doc_free(t->selection);
    doc_free(t->quote);
    doc_free(t->job);
    doc_free(t->chat);
    doc_free(t->result);
    free(t->text);
    free(t->error);
    free(t);
}
static void queue(App *a, Task *t) {
    t->next = NULL;
    if (a->tail)
        a->tail->next = t;
    else
        a->head = t;
    a->tail = t;
}
static void finish_batch(Task *t) {
    J *j = root(t->job);
    size_t count = yyjson_mut_arr_size(get(root(t->quote), "entries"));
    int64_t fail = integer(get(j, "failed")), success = integer(get(j, "succeeded"));
    const char *state = t->index < count ? "queued"
                        : fail           ? (success ? "partial" : "failed")
                                         : "completed";
    put(t->job, j, "state", js(t->job, state));
    put(t->job, j, "processed", ji(t->job, (int64_t)t->index));
}
static void ai_worker(uv_work_t *w) {
    Task *t = w->data;
    App *a = t->app;
    if (t->kind == TASK_MODELS) {
        t->result = provider_models(&a->cfg, (Provider)t->index, &t->error);
        return;
    }
    if (t->kind == TASK_ESTIMATE) {
        t->result = estimate_build(a, &t->snapshot, t->selection, &t->error);
        return;
    }
    if (t->kind == TASK_CHAT) {
        t->result = chat_turn(a, t->chat, &t->snapshot, t->text, &t->error);
        put(t->job, root(t->job), "state",
            js(t->job, t->error || get(root(t->result), "turn_error") ? "failed" : "completed"));
        if (t->result) {
            put(t->job, root(t->job), "usage", copy(t->job, get(root(t->result), "usage")));
            put(t->job, root(t->job), "result_not_saved",
                copy(t->job, get(root(t->result), "result_not_saved")));
        }
        if (t->error)
            put(t->job, root(t->job), "error", js(t->job, t->error));
        persist_job(a, t->job);
        return;
    }
    J *entries = get(root(t->quote), "entries"), *entry = yyjson_mut_arr_get(entries, t->index);
    if (!entry) {
        finish_batch(t);
        persist_job(a, t->job);
        return;
    }
    Doc *job = t->job;
    J *j = root(job), *row = obj(job);
    put(job, row, "trace_id", copy(job, get(entry, "trace_id")));
    put(job, row, "revision", copy(job, get(entry, "revision")));
    if (!strcmp(sget(entry, "status"), "excluded")) {
        put(job, row, "status", js(job, "excluded"));
        put(job, row, "error", copy(job, get(entry, "reason")));
    } else {
        t->result = analyze_entry(a, root(t->quote), entry, &t->snapshot.analyses, &t->error);
        J *r = root(t->result);
        if (t->error || get(r, "error")) {
            put(job, row, "status", js(job, "failed"));
            put(job, row, "error", t->error ? js(job, t->error) : copy(job, get(r, "error")));
            put(job, row, "usage", copy(job, get(r, "usage")));
            put(job, row, "raw_response", copy(job, get(r, "raw_response")));
            put(job, row, "outcome_unknown", copy(job, get(r, "outcome_unknown")));
            put(job, j, "failed", ji(job, integer(get(j, "failed")) + 1));
        } else {
            put(job, row, "status", js(job, boolean(get(r, "reused")) ? "cached" : "completed"));
            put(job, row, "analysis_id", copy(job, get(r, "id")));
            put(job, row, "usage",
                boolean(get(r, "reused")) ? js(job, NULL) : copy(job, get(r, "usage")));
            put(job, row, "cost_usd",
                boolean(get(r, "reused")) ? jnum(job, 0) : copy(job, get(r, "cost_usd")));
            put(job, row, "result_not_saved", copy(job, get(r, "result_not_saved")));
            put(job, j, "succeeded", ji(job, integer(get(j, "succeeded")) + 1));
        }
    }
    add(get(j, "results"), row);
    t->index++;
    finish_batch(t);
    persist_job(a, job);
}
static void ai_done(uv_work_t *w, int status) {
    Task *t = w->data;
    App *a = t->app;
    a->ai_busy = false;
    if (status && !t->error)
        t->error = xstrdup(uv_strerror(status));
    if (status && t->job) {
        put(t->job, root(t->job), "state", js(t->job, "failed"));
        put(t->job, root(t->job), "error", js(t->job, t->error));
        persist_job(a, t->job);
    }
    if (t->kind == TASK_MODELS) {
        t->reply(t->reply_ctx, t->result ? 200 : 502,
                 t->result
                     ? doc_clone(t->result)
                     : error_doc("models_failed", t->error ? t->error : "Unable to load models"));
    } else if (t->kind == TASK_ESTIMATE) {
        if (t->result) {
            docs_push(&a->quotes, doc_clone(t->result));
            Doc *preview = doc_clone(t->result);
            yyjson_mut_obj_remove_str(root(preview), "rubric_content");
            size_t i, n;
            J *e;
            yyjson_mut_arr_foreach(get(root(preview), "entries"), i, n, e) {
                yyjson_mut_obj_remove_str(e, "request_body");
                yyjson_mut_obj_remove_str(get(e, "coverage"), "input_event_ids");
            }
            t->reply(t->reply_ctx, 200, preview);
        } else
            t->reply(t->reply_ctx, 500,
                     error_doc("estimate_failed", t->error ? t->error : "Unable to build quote"));
    } else {
        if (t->kind == TASK_BATCH && t->result && !get(root(t->result), "error")) {
            if (!docs_find(&a->analyses, "id", sget(root(t->result), "id")))
                docs_push(&a->analyses, analysis_index(t->result));
            a->catalog_revision++;
        }
        if (t->kind == TASK_CHAT && t->result)
            docs_replace(&a->chats, "id", doc_clone(t->result));
        if (a->stopping && !strcmp(sget(root(t->job), "state"), "queued")) {
            put(t->job, root(t->job), "state", js(t->job, "interrupted"));
            persist_job(a, t->job);
        }
        docs_replace(&a->jobs, "id", doc_clone(t->job));
        if (t->kind == TASK_BATCH && !a->stopping && !status &&
            t->index < yyjson_mut_arr_size(get(root(t->quote), "entries"))) {
            doc_free(t->result);
            t->result = NULL;
            free(t->error);
            t->error = NULL;
            docs_free(&t->snapshot.analyses);
            queue(a, t);
            ai_kick(a);
            return;
        }
    }
    task_free(t);
    ai_kick(a);
}
void ai_kick(App *a) {
    if (a->stopping || a->ai_busy || !a->head)
        return;
    Task *t = a->head;
    a->head = t->next;
    if (!a->head)
        a->tail = NULL;
    t->work.data = t;
    if (t->kind == TASK_BATCH) { /* Cache check is refreshed between every trace request. */
        for (size_t i = 0; i < a->analyses.n; i++)
            docs_push(&t->snapshot.analyses, doc_clone(a->analyses.v[i]));
    }
    if (t->job) {
        put(t->job, root(t->job), "state", js(t->job, "running"));
        persist_job(a, t->job);
        docs_replace(&a->jobs, "id", doc_clone(t->job));
    }
    a->ai_busy = true;
    int rc = uv_queue_work(&a->loop, &t->work, ai_worker, ai_done);
    if (rc)
        ai_done(&t->work, rc);
}
void enqueue_models(App *a, Provider provider, Reply reply, void *ctx) {
    Task *t = xcalloc(1, sizeof(*t));
    t->app = a;
    t->kind = TASK_MODELS;
    t->index = (size_t)provider;
    t->reply = reply;
    t->reply_ctx = ctx;
    queue(a, t);
    ai_kick(a);
}
void enqueue_estimate(App *a, Doc *selection, Reply reply, void *ctx) {
    Task *t = xcalloc(1, sizeof(*t));
    t->app = a;
    t->kind = TASK_ESTIMATE;
    t->selection = selection;
    t->reply = reply;
    t->reply_ctx = ctx;
    /* Quotes retain immutable trace references; AI inputs are token-budgeted later. */
    bool all = boolean(get(root(selection), "all"));
    J *ids = get(root(selection), "trace_ids");
    for (size_t i = 0; i < a->traces.n; i++) {
        Trace *tr = a->traces.v[i];
        bool include = all;
        size_t k, n;
        J *id;
        yyjson_mut_arr_foreach(ids, k, n, id) if (!strcmp(str(id), tr->id)) include = true;
        if (include) {
            trace_retain(tr);
            traces_push(&t->snapshot.traces, tr);
        }
    }
    for (size_t i = 0; i < a->analyses.n; i++)
        if (trace_find(&t->snapshot.traces, sget(root(a->analyses.v[i]), "trace_id")))
            docs_push(&t->snapshot.analyses, doc_clone(a->analyses.v[i]));
    queue(a, t);
    ai_kick(a);
}
static Doc *job_ack(const char *id) {
    Doc *d = doc_new();
    put(d, root(d), "job_id", js(d, id));
    return d;
}
Doc *start_analysis(App *a, const char *id, int *status) {
    *status = 202;
    for (size_t i = 0; i < a->jobs.n; i++)
        if (!strcmp(sget(root(a->jobs.v[i]), "quote_id"), id))
            return job_ack(sget(root(a->jobs.v[i]), "id"));
    Doc *quote = docs_find(&a->quotes, "id", id);
    if (!quote) {
        *status = 404;
        return error_doc("not_found", "Quote not found");
    }
    if (integer(get(root(quote), "expires_at_epoch")) <= (int64_t)time(NULL)) {
        *status = 409;
        return error_doc("expired_quote", "Quote expired; refresh the estimate");
    }
    Provider quoted_provider;
    if (!provider_parse(sget(root(quote), "ai_provider"), &quoted_provider))
        quoted_provider = AI_OPENAI;
    if (!*provider_key(&a->cfg, quoted_provider) && integer(get(root(quote), "eligible"))) {
        *status = 409;
        return error_doc("missing_key",
                         "Selected provider API key is missing; paid operations are disabled");
    }
    char job_id[33];
    if (!new_id(job_id)) {
        *status = 500;
        return error_doc("random_failed", "Unable to generate job ID");
    }
    Doc *job = doc_new();
    J *r = root(job);
    put(job, r, "id", js(job, job_id));
    put(job, r, "quote_id", js(job, id));
    put(job, r, "kind", js(job, "analysis"));
    put(job, r, "state", js(job, "queued"));
    put(job, r, "results", arr(job));
    put(job, r, "total", copy(job, get(root(quote), "total")));
    put(job, r, "succeeded", ji(job, 0));
    put(job, r, "failed", ji(job, 0));
    put(job, r, "processed", ji(job, 0));
    put(job, r, "estimated_usd", copy(job, get(root(quote), "estimated_usd")));
    put(job, r, "tokens_estimated", copy(job, get(root(quote), "tokens_estimated")));
    char *e = NULL;
    if (!save_doc(a, "jobs", job_id, job, &e)) {
        *status = 500;
        Doc *err = error_doc("persistence_failed", e);
        free(e);
        doc_free(job);
        return err;
    }
    /* Durable job lookup by quote_id provides idempotency even if quote update fails/crashes. */
    put(quote, root(quote), "job_id", js(quote, job_id));
    if (!save_doc(a, "quotes", id, quote, &e)) {
        fprintf(stderr, "traceview: quote link not saved: %s\n", e);
        free(e);
    }
    docs_push(&a->jobs, doc_clone(job));
    Task *t = xcalloc(1, sizeof(*t));
    t->app = a;
    t->kind = TASK_BATCH;
    t->job = job;
    t->quote = doc_clone(quote);
    queue(a, t);
    ai_kick(a);
    return job_ack(job_id);
}
static bool valid_scope(App *a, J *scope) {
    if (!yyjson_mut_is_obj(scope))
        return false;
    const char *type = sget(scope, "type");
    if (!strcmp(type, "all"))
        return yyjson_mut_obj_size(scope) == 1;
    if (!strcmp(type, "trace"))
        return yyjson_mut_obj_size(scope) == 2 && valid_id(sget(scope, "id")) &&
               trace_find(&a->traces, sget(scope, "id"));
    if (!strcmp(type, "directory") && yyjson_mut_obj_size(scope) == 2 &&
        yyjson_mut_is_str(get(scope, "cwd"))) {
        for (size_t i = 0; i < a->traces.n; i++)
            if (!strcmp(sget(meta(a->traces.v[i]), "cwd"), sget(scope, "cwd")))
                return true;
    }
    return false;
}
Doc *create_chat(App *a, J *scope, int *status) {
    *status = 200;
    if (!valid_scope(a, scope)) {
        *status = 400;
        return error_doc("bad_scope", "Choose an existing trace, exact directory, or all scope");
    }
    char id[33];
    if (!new_id(id)) {
        *status = 500;
        return error_doc("random_failed", "Unable to generate chat ID");
    }
    Doc *d = doc_new();
    put(d, root(d), "id", js(d, id));
    put(d, root(d), "scope", copy(d, scope));
    put(d, root(d), "messages", arr(d));
    put(d, root(d), "usage", arr(d));
    char *now = now_iso();
    put(d, root(d), "created_at", js(d, now));
    put(d, root(d), "updated_at", js(d, now));
    free(now);
    char *err = NULL;
    if (!save_doc(a, "chats", id, d, &err)) {
        *status = 500;
        doc_free(d);
        Doc *e = error_doc("persistence_failed", err);
        free(err);
        return e;
    }
    docs_push(&a->chats, d);
    Doc *result = doc_new();
    put(result, root(result), "chat_id", js(result, id));
    return result;
}
Doc *start_chat(App *a, const char *id, const char *text, int *status) {
    *status = 202;
    Doc *chat = docs_find(&a->chats, "id", id);
    if (!chat) {
        *status = 404;
        return error_doc("not_found", "Chat not found");
    }
    if (!*provider_key(&a->cfg, a->cfg.chat.provider)) {
        *status = 409;
        return error_doc("missing_key",
                         "Selected provider API key is missing; chat has separate paid API usage");
    }
    const char *last = sget(root(chat), "latest_job");
    Doc *prev = docs_find(&a->jobs, "id", last);
    if (prev && (!strcmp(sget(root(prev), "state"), "running") ||
                 !strcmp(sget(root(prev), "state"), "queued"))) {
        *status = 409;
        return error_doc("chat_busy", "This chat already has a turn in flight");
    }
    Task *t = xcalloc(1, sizeof(*t));
    t->app = a;
    t->kind = TASK_CHAT;
    t->text = xstrdup(text);
    char *err = NULL;
    if (!snapshot_take(a, get(root(chat), "scope"), &t->snapshot, &err)) {
        *status = 413;
        Doc *e = error_doc("scope_too_large", err);
        free(err);
        task_free(t);
        return e;
    }
    char job_id[33];
    if (!new_id(job_id)) {
        task_free(t);
        *status = 500;
        return error_doc("random_failed", "Unable to create job ID");
    }
    t->job = doc_new();
    J *j = root(t->job);
    put(t->job, j, "id", js(t->job, job_id));
    put(t->job, j, "kind", js(t->job, "chat"));
    put(t->job, j, "chat_id", js(t->job, id));
    put(t->job, j, "state", js(t->job, "queued"));
    if (!save_doc(a, "jobs", job_id, t->job, &err)) {
        *status = 500;
        Doc *e = error_doc("persistence_failed", err);
        free(err);
        task_free(t);
        return e;
    }
    t->chat = doc_clone(chat);
    put(t->chat, root(t->chat), "latest_job", js(t->chat, job_id));
    J *m = obj(t->chat);
    put(t->chat, m, "role", js(t->chat, "user"));
    put(t->chat, m, "text", js(t->chat, text));
    put(t->chat, m, "completed", jb(t->chat, false));
    add(get(root(t->chat), "messages"), m);
    if (!save_doc(a, "chats", id, t->chat, &err)) {
        put(t->job, j, "state", js(t->job, "failed"));
        put(t->job, j, "error", js(t->job, err));
        persist_job(a, t->job);
        docs_push(&a->jobs, doc_clone(t->job));
        *status = 500;
        Doc *e = error_doc("persistence_failed", err);
        free(err);
        task_free(t);
        return e;
    }
    docs_push(&a->jobs, doc_clone(t->job));
    docs_replace(&a->chats, "id", doc_clone(t->chat));
    queue(a, t);
    ai_kick(a);
    return job_ack(job_id);
}
Doc *state_doc(App *a) {
    Doc *d = doc_new();
    J *r = root(d), *items = arr(d);
    put(d, r, "catalog_revision", ji(d, (int64_t)a->catalog_revision));
    put(d, r, "traces", items);
    for (size_t i = 0; i < a->traces.n; i++) {
        Doc *s = trace_summary(a->traces.v[i], &a->analyses);
        add(items, copy(d, root(s)));
        doc_free(s);
    }
    Snapshot snap = {.traces = a->traces, .analyses = a->analyses};
    Doc *groups = aggregate(&snap, "directory", 0, MAX_SOURCES);
    put(d, r, "stats", copy(d, get(root(groups), "stats")));
    put(d, r, "groups", copy(d, get(root(groups), "items")));
    doc_free(groups);
    put(d, r, "key_available", jb(d, *provider_key(&a->cfg, a->cfg.chat.provider) != 0));
    put(d, r, "decision_key_available",
        jb(d, *provider_key(&a->cfg, a->cfg.decision.provider) != 0));
    put(d, r, "decision_model", js(d, a->cfg.decision_model));
    put(d, r, "chat_model", js(d, a->cfg.chat_model));
    put(d, r, "price_checked", js(d, PRICE_DATE));
    put(d, r, "rate_usd_per_m", jnum(d, a->cfg.rate));
    put(d, r, "ingestion_running", jb(d, a->ingest_busy));
    put(d, r, "ai_running", jb(d, a->ai_busy));
    size_t queued = 0;
    for (Task *t = a->head; t; t = t->next)
        queued++;
    put(d, r, "queued", ji(d, (int64_t)queued));
    J *recent = arr(d);
    put(d, r, "recent_jobs", recent);
    size_t start = a->jobs.n > 20 ? a->jobs.n - 20 : 0;
    for (size_t i = start; i < a->jobs.n; i++) {
        J *source = root(a->jobs.v[i]);
        if (strcmp(sget(source, "kind"), "analysis"))
            continue;
        J *j = obj(d);
        const char *fields[] = {"id", "kind", "state", "total", "processed"};
        for (size_t k = 0; k < 5; k++)
            put(d, j, fields[k], copy(d, get(source, fields[k])));
        add(recent, j);
    }
    put(d, r, "discovery_error", js(d, a->discovery_error));
    return d;
}
#ifndef TRACEVIEW_TEST
static void poll_cb(uv_timer_t *timer) {
    schedule_discovery(timer->data);
}
static void signal_cb(uv_signal_t *handle, int sig) {
    (void)sig;
    app_shutdown(handle->data);
}
#endif
void app_shutdown(App *a) {
    if (a->stopping)
        return;
    a->stopping = true;
    fprintf(stderr,
            "traceview: stopping; waiting for active workers (network timeout at most 120s)\n");
    http_stop(a);
    uv_timer_stop(&a->poll);
    uv_close((uv_handle_t *)&a->poll, NULL);
    uv_signal_stop(&a->sigint);
    uv_signal_stop(&a->sigterm);
    uv_close((uv_handle_t *)&a->sigint, NULL);
    uv_close((uv_handle_t *)&a->sigterm, NULL);
    while (a->head) {
        Task *t = a->head;
        a->head = t->next;
        if (t->job) {
            put(t->job, root(t->job), "state", js(t->job, "interrupted"));
            persist_job(a, t->job);
            docs_replace(&a->jobs, "id", doc_clone(t->job));
        }
        if (t->reply)
            t->reply(t->reply_ctx, 503, error_doc("shutting_down", "Application is stopping"));
        task_free(t);
    }
    a->tail = NULL;
}
bool app_init(App *a, char **error) {
    memset(a, 0, sizeof(*a));
    a->lock_fd = -1;
    if (!config_load(&a->cfg, error))
        return false;
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) {
        *error = xstrdup("libcurl initialization failed");
        return false;
    }
    size_t n;
    char *s = read_file("rubric.json", MAX_SCHEMA_BYTES, &n, error);
    if (!s)
        return false;
    a->rubric = doc_parse(s, n);
    free(s);
    if (!a->rubric || !yyjson_mut_is_arr(get(root(a->rubric), "questions"))) {
        *error = xstrdup("Invalid rubric.json");
        return false;
    }
    s = read_file("tools.json", MAX_SCHEMA_BYTES, &n, error);
    if (!s)
        return false;
    a->tools = doc_parse(s, n);
    free(s);
    if (!a->tools || yyjson_mut_arr_size(root(a->tools)) != 4) {
        *error = xstrdup("Invalid tools.json");
        return false;
    }
    a->html = read_file("web/index.html", 1024 * 1024, &n, error);
    if (!a->html)
        return false;
    return tokenizer_init(error) && store_open(a, error) && ai_settings_load(a, error);
}
void app_destroy(App *a) {
    store_close(a);
    doc_free(a->rubric);
    doc_free(a->tools);
    free(a->html);
    free(a->discovery_error);
    config_free(&a->cfg);
    tokenizer_destroy();
    curl_global_cleanup();
}
#ifndef TRACEVIEW_TEST
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    App a;
    char *error = NULL;
    if (!app_init(&a, &error)) {
        fprintf(stderr, "traceview: %s\n", error ? error : "Initialization failed");
        free(error);
        app_destroy(&a);
        return 1;
    }
    uv_loop_init(&a.loop);
    if (!http_start(&a, &error)) {
        fprintf(stderr, "traceview: %s\n", error);
        free(error);
        uv_run(&a.loop, UV_RUN_DEFAULT);
        uv_loop_close(&a.loop);
        app_destroy(&a);
        return 1;
    }
    uv_timer_init(&a.loop, &a.poll);
    a.poll.data = &a;
    uv_timer_start(&a.poll, poll_cb, 0, a.cfg.poll_ms);
    uv_signal_init(&a.loop, &a.sigint);
    uv_signal_init(&a.loop, &a.sigterm);
    a.sigint.data = &a;
    a.sigterm.data = &a;
    uv_signal_start(&a.sigint, signal_cb, SIGINT);
    uv_signal_start(&a.sigterm, signal_cb, SIGTERM);
    printf("http://127.0.0.1:%d\n", a.cfg.port);
    fflush(stdout);
    uv_run(&a.loop, UV_RUN_DEFAULT);
    uv_loop_close(&a.loop);
    app_destroy(&a);
    return 0;
}
#endif
