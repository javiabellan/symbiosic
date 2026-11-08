#include "app.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>

static int checks = 0, calls = 0, chat_round = 0, mode = 0;
static App *test_app;
static char *sent_body;
static char cited_id[33];
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static J *meta(Trace *t) {
    return get(root(t->doc), "meta");
}
static Doc *parse(const char *s) {
    Doc *d = doc_parse(s, strlen(s));
    CHECK(d);
    return d;
}
static void fixture(const char *source, const char *target) {
    size_t n;
    char *e = NULL, *s = read_file(source, MAX_NATIVE, &n, &e);
    CHECK(s);
    CHECK(atomic_write(target, s, n, &e));
    free(s);
    free(e);
}
static void append(const char *p, const char *text) {
    int fd = open(p, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
}
static Trace *norm(const char *path, const char *provider, Trace *old) {
    char *e = NULL;
    Trace *t = normalize(path, provider, old, &e);
    if (e)
        fprintf(stderr, "normalize: %s\n", e);
    CHECK(t);
    free(e);
    return t;
}
static bool contains(Trace *t, const char *text) {
    char *s = json(root(t->doc));
    bool found = strstr(s, text) != NULL;
    free(s);
    return found;
}
static Transport fake(const char *endpoint, const char *body, const char *key) {
    CHECK(!strcmp(key, "test-only"));
    calls++;
    free(sent_body);
    sent_body = xstrdup(body);
    Doc *request = parse(body), *response = doc_new();
    J *r = root(response);
    if (mode == 4)
        uv_sleep(250);
    if (mode == 1) {
        doc_free(request);
        doc_free(response);
        return (Transport){.status = 0,
                           .body = xstrdup(""),
                           .error = xstrdup("Timeout; outcome unknown"),
                           .ambiguous = true};
    }
    if (mode == 2) {
        doc_free(request);
        doc_free(response);
        return (Transport){.status = 200, .body = xstrdup("not json")};
    }
    if (!strcmp(endpoint, "decisions")) {
        CHECK(!strcmp(sget(get(root(request), "prompt_cache_options"), "mode"), "explicit"));
        J *answers = arr(response);
        put(response, r, "model", js(response, "gpt-6-luna"));
        put(response, r, "answers", answers);
        size_t i, n;
        J *q;
        yyjson_mut_arr_foreach(get(root(request), "questions"), i, n, q) {
            J *a = obj(response);
            const char *name = sget(q, "name"), *type = sget(q, "type");
            put(response, a, "name", js(response, name));
            if (!strcmp(name, "human_verification")) {
                put(response, a, "type", js(response, "refusal"));
                add(answers, a);
                continue;
            }
            put(response, a, "type", js(response, type));
            if (!strcmp(type, "predicate"))
                put(response, a, "probability",
                    jnum(response, !strcmp(name, "activity_research")  ? .5
                                   : !strcmp(name, "failed_tool_call") ? .05
                                                                       : .95));
            else if (!strcmp(type, "choice")) {
                put(response, a, "choice",
                    js(response, !strcmp(name, "plan_basis")
                                     ? "both"
                                     : sget(yyjson_mut_arr_get(get(q, "choices"), 0), "value")));
                put(response, a, "confidence", jnum(response, .9));
                put(response, a, "probabilities", arr(response));
            } else {
                put(response, a, "score", jnum(response, 2.75));
                put(response, a, "confidence", jnum(response, .9));
                put(response, a, "probabilities", arr(response));
            }
            add(answers, a);
        }
    } else {
        CHECK(!strcmp(endpoint, "responses"));
        CHECK(!boolean(get(root(request), "store")));
        CHECK(!boolean(get(root(request), "parallel_tool_calls")));
        CHECK(!strcmp(sget(get(root(request), "reasoning"), "effort"), "none"));
        J *output = arr(response);
        put(response, r, "output", output);
        if (chat_round == 0) {
            J *call = obj(response);
            put(response, call, "type", js(response, "function_call"));
            put(response, call, "name", js(response, "get_stats"));
            put(response, call, "call_id", js(response, "stats-1"));
            put(response, call, "arguments",
                js(response, "{\"group_by\":\"provider\",\"offset\":0,\"limit\":50}"));
            add(output, call);
        } else if (chat_round == 1) {
            bool found = false;
            size_t i, n;
            J *v;
            yyjson_mut_arr_foreach(get(root(request), "input"), i, n,
                                   v) if (!strcmp(sget(v, "type"), "function_call_output") &&
                                          !strcmp(sget(v, "call_id"), "stats-1")) found = true;
            CHECK(found);
            J *call = obj(response);
            put(response, call, "type", js(response, "function_call"));
            put(response, call, "name", js(response, "read_events"));
            put(response, call, "call_id", js(response, "read-1"));
            char *args = fmt("{\"trace_id\":\"%s\",\"start_seq\":0,\"limit\":1,\"text_offset\":0}",
                             cited_id);
            put(response, call, "arguments", js(response, args));
            free(args);
            add(output, call);
        } else {
            bool found = false;
            size_t i, n;
            J *v;
            yyjson_mut_arr_foreach(get(root(request), "input"), i, n,
                                   v) if (!strcmp(sget(v, "call_id"), "read-1")) found = true;
            CHECK(found);
            J *m = obj(response), *content = arr(response), *p = obj(response);
            put(response, m, "type", js(response, "message"));
            put(response, m, "role", js(response, "assistant"));
            put(response, m, "content", content);
            put(response, p, "type", js(response, "output_text"));
            char *answer = fmt("The visible trace includes a user request [%s@1:0].", cited_id);
            put(response, p, "text", js(response, answer));
            free(answer);
            add(content, p);
            add(output, m);
        }
        chat_round++;
    }
    J *usage = obj(response), *details = obj(response);
    put(response, r, "usage", usage);
    put(response, usage, "input_tokens", ji(response, 100));
    put(response, usage, "output_tokens", ji(response, 10));
    put(response, usage, "input_tokens_details", details);
    put(response, details, "cached_tokens", ji(response, mode == 3 ? 10 : 0));
    put(response, details, "cache_write_tokens", ji(response, 0));
    char *serialized = json(r);
    doc_free(response);
    doc_free(request);
    return (Transport){.status = 200, .body = serialized};
}
static void test_normalizers(App *a, const char *cp, const char *xp) {
    Trace *c = norm(cp, "claude", NULL), *x = norm(xp, "codex", NULL);
    CHECK(valid_id(c->id));
    CHECK(!strcmp(sget(meta(c), "cwd"), "/work/app"));
    CHECK(!strcmp(sget(meta(x), "cwd"), "/work/app"));
    CHECK(!contains(c, "Old streaming text"));
    CHECK(contains(c, "Updated streaming text"));
    CHECK(!contains(c, "SANITIZED_IMAGE_BYTES"));
    CHECK(!contains(x, "SANITIZED_ENCRYPTED_BYTES"));
    J *cs = get(meta(c), "stats"), *xs = get(meta(x), "stats");
    CHECK(integer(get(cs, "human_turns")) == 2);
    CHECK(integer(get(get(cs, "tool_results"), "failure")) == 1);
    CHECK(integer(get(cs, "compactions")) == 1);
    CHECK(integer(get(get(cs, "usage"), "input_tokens")) == 210);
    CHECK(integer(get(get(cs, "usage"), "output_tokens")) == 45);
    CHECK(integer(get(xs, "human_turns")) == 1);
    CHECK(integer(get(xs, "messages")) == 4);
    CHECK(integer(get(get(xs, "usage"), "input_tokens")) == 320);
    CHECK(integer(get(get(xs, "usage"), "total_tokens")) == 385);
    CHECK(integer(get(get(xs, "usage"), "resets")) == 1);
    CHECK(integer(get(get(xs, "tool_results"), "unknown")) == 1);
    CHECK(integer(get(get(xs, "tool_results"), "success")) == 1);
    size_t before = yyjson_mut_arr_size(get(root(x->doc), "events"));
    append(xp, "{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\","
               "\"content\":\"A partial line\"}}");
    Trace *partial = norm(xp, "codex", x);
    CHECK(yyjson_mut_arr_size(get(root(partial->doc), "events")) == before);
    CHECK(integer(get(meta(partial), "revision")) == 1);
    append(xp, "\n");
    Trace *complete = norm(xp, "codex", partial);
    CHECK(integer(get(meta(complete), "revision")) == 2);
    CHECK(contains(complete, "A partial line"));
    trace_release(partial);
    trace_release(complete);
    fixture("tests/fixtures/codex.jsonl", xp);
    Trace *same = norm(xp, "codex", x);
    CHECK(integer(get(meta(same), "revision")) == 1);
    trace_release(same);
    char *e = NULL;
    CHECK(trace_save(a, c, &e));
    CHECK(trace_save(a, x, &e));
    char *path = fmt("%s/traces/%s.jsonl", a->cfg.data_dir, c->id);
    Trace *loaded = trace_load(path, &e);
    CHECK(loaded);
    CHECK(!strcmp(loaded->id, c->id));
    CHECK(contains(loaded, "Updated streaming text"));
    free(path);
    trace_release(loaded);
    traces_push(&a->traces, c);
    traces_push(&a->traces, x);
    strcpy(cited_id, c->id);
    char *bad = fmt("%s/bad.jsonl", a->cfg.claude_root);
    fixture("tests/fixtures/claude.jsonl", bad);
    append(bad, "this is not json\n{\"type\":\"new_unknown_variant\"}\n");
    Trace *malformed = norm(bad, "claude", NULL);
    CHECK(boolean(get(get(meta(malformed), "diagnostics"), "coverage_incomplete")));
    CHECK(integer(get(get(meta(malformed), "diagnostics"), "malformed_lines")) == 1);
    trace_release(malformed);
    unlink(bad);
    free(bad);
    Trace *context = norm("tests/fixtures/context.jsonl", "codex", NULL);
    CHECK(integer(get(get(meta(context), "stats"), "context_limits")) == 1);
    trace_release(context);
    char *huge = fmt("%s/huge.jsonl", a->cfg.codex_root);
    int fd = open(huge, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, MAX_NATIVE + 1) == 0);
    close(fd);
    Trace *oversize = norm(huge, "codex", NULL);
    CHECK(*sget(get(meta(oversize), "diagnostics"), "ingestion_error"));
    trace_release(oversize);
    unlink(huge);
    free(huge);
}
static void test_tools(App *a) {
    Snapshot s = {0};
    Doc *scope = parse("{\"type\":\"directory\",\"cwd\":\"/work/app\"}");
    char *e = NULL;
    CHECK(snapshot_take(a, root(scope), &s, &e));
    CHECK(s.traces.n == 2);
    Doc *args = parse("{\"offset\":0,\"limit\":1}"), *r = tool_run(&s, "list_sessions", root(args));
    CHECK(yyjson_mut_arr_size(get(root(r), "items")) == 1);
    CHECK(integer(get(root(r), "next_offset")) == 1);
    doc_free(r);
    doc_free(args);
    args = parse("{\"group_by\":\"provider\",\"offset\":1,\"limit\":1}");
    r = tool_run(&s, "get_stats", root(args));
    CHECK(integer(get(get(root(r), "stats"), "traces")) == 2);
    CHECK(integer(get(get(root(r), "stats"), "overlapping_session_spans")) == 1);
    CHECK(yyjson_mut_arr_size(get(root(r), "items")) == 1);
    doc_free(r);
    doc_free(args);
    args = parse("{\"query\":\"make test\",\"trace_id\":null,\"offset\":0,\"limit\":1}");
    r = tool_run(&s, "search_events", root(args));
    CHECK(integer(get(root(r), "total_matches")) == 2);
    CHECK(boolean(get(root(r), "truncated")));
    doc_free(r);
    doc_free(args);
    args = parse("{\"trace_id\":\"00000000000000000000000000000000\",\"start_seq\":0,\"limit\":1,"
                 "\"text_offset\":0}");
    r = tool_run(&s, "read_events", root(args));
    CHECK(!strcmp(sget(get(root(r), "error"), "code"), "outside_scope"));
    doc_free(r);
    doc_free(args);
    args = parse("{\"offset\":-1,\"limit\":50}");
    r = tool_run(&s, "list_sessions", root(args));
    CHECK(get(root(r), "error"));
    doc_free(r);
    doc_free(args);
    args = parse("{\"group_by\":\"evil\",\"offset\":0,\"limit\":1}");
    r = tool_run(&s, "get_stats", root(args));
    CHECK(get(root(r), "error"));
    doc_free(r);
    doc_free(args);
    snapshot_free(&s);
    doc_free(scope);
    Trace *t = a->traces.v[0];
    Doc *d = doc_clone(t->doc);
    J *events = get(root(d), "events"), *ev = yyjson_mut_arr_get(events, 0);
    char *longtext = xmalloc(60001);
    memset(longtext, 'x', 60000);
    longtext[60000] = 0;
    put(d, ev, "text", js(d, longtext));
    free(longtext);
    Trace local = {.doc = d};
    strcpy(local.id, t->id);
    r = events_page(&local, 0, 1, 0);
    char *serialized = json(root(r));
    CHECK(strlen(serialized) <= MAX_TOOL);
    free(serialized);
    J *item = yyjson_mut_arr_get(get(root(r), "items"), 0);
    CHECK(integer(get(item, "next_text_offset")) > 0);
    size_t offset = (size_t)integer(get(item, "next_text_offset"));
    doc_free(r);
    r = events_page(&local, 0, 1, offset);
    CHECK(integer(get(yyjson_mut_arr_get(get(root(r), "items"), 0), "text_offset")) ==
          (int64_t)offset);
    doc_free(r);
    char *escaped = xmalloc(60001);
    memset(escaped, 1, 60000);
    escaped[60000] = 0;
    put(d, ev, "text", js(d, escaped));
    free(escaped);
    r = events_page(&local, 0, 1, 0);
    serialized = json(root(r));
    CHECK(strlen(serialized) <= MAX_TOOL);
    CHECK(yyjson_mut_arr_size(get(root(r), "items")) == 1);
    CHECK(integer(get(yyjson_mut_arr_get(get(root(r), "items"), 0), "next_text_offset")) > 0);
    free(serialized);
    doc_free(r);
    doc_free(d);
}
static void test_projection_limits(App *a) {
    Trace *original = a->traces.v[0];
    Trace local = {.doc = doc_clone(original->doc), .bytes = original->bytes};
    strcpy(local.id, original->id);
    J *events = get(root(local.doc), "events"), *e = yyjson_mut_arr_get(events, 0);
    put(local.doc, e, "kind", js(local.doc, "message"));
    char *large = xmalloc(MAX_REQUEST + 1);
    memset(large, 'x', MAX_REQUEST);
    large[MAX_REQUEST] = 0;
    put(local.doc, e, "text", js(local.doc, large));
    char *body = NULL, *error = NULL;
    Doc *coverage = projection(a, &local, &body, &error);
    CHECK(!coverage && !body && error && strstr(error, "too_large"));
    free(error);
    error = NULL;
    put(local.doc, e, "kind", js(local.doc, "tool_result"));
    coverage = projection(a, &local, &body, &error);
    CHECK(coverage && body && !error);
    CHECK(yyjson_mut_arr_size(get(root(coverage), "shortened_results")) == 1);
    CHECK(strstr(body, "original UTF-8 bytes"));
    CHECK(strlen(sget(e, "text")) == MAX_REQUEST);
    free(body);
    doc_free(coverage);
    free(large);
    doc_free(local.doc);
    Doc *invalid = parse("{\"answers\":[],\"usage\":{\"input_tokens\":5}}");
    Doc *values =
        interpret_answers(root(invalid), get(root(a->rubric), "questions"), false, &error);
    CHECK(!values && error);
    free(error);
    doc_free(invalid);
}
static void test_ai(App *a) {
    Snapshot snapshot = {0};
    Doc *scope = parse("{\"type\":\"all\"}"), *selection = parse("{\"all\":true}");
    char *e = NULL;
    CHECK(snapshot_take(a, root(scope), &snapshot, &e));
    Doc *q = estimate_build(a, &snapshot, selection, &e);
    CHECK(q);
    CHECK(calls == 0);
    CHECK(integer(get(root(q), "eligible")) == 2);
    CHECK(number(get(root(q), "estimated_usd")) > 0);
    J *entry = yyjson_mut_arr_get(get(root(q), "entries"), 0);
    char *frozen = xstrdup(sget(entry, "request_body"));
    Doc *result = analyze_entry(a, root(q), entry, &a->analyses, &e);
    CHECK(result && !get(root(result), "error"));
    CHECK(calls == 1);
    CHECK(!strcmp(sent_body, frozen));
    CHECK(fabs(number(get(get(get(root(result), "values"), "completion_evidence"), "value")) -
               2.75) < .001);
    CHECK(yyjson_mut_is_null(get(get(get(root(result), "values"), "human_verification"), "value")));
    CHECK(yyjson_mut_arr_size(get(get(get(root(result), "values"), "activities"), "uncertain")) ==
          1);
    CHECK(fabs(number(get(root(result), "cost_usd")) - .00001) < .000000001);
    docs_push(&a->analyses, result);
    result = analyze_entry(a, root(q), entry, &a->analyses, &e);
    CHECK(boolean(get(root(result), "reused")));
    CHECK(calls == 1);
    doc_free(result);
    snapshot_free(&snapshot);
    CHECK(snapshot_take(a, root(scope), &snapshot, &e));
    Doc *again = estimate_build(a, &snapshot, selection, &e);
    CHECK(integer(get(root(again), "cached")) == 1);
    doc_free(again);
    Doc *incomplete = interpret_answers(get(root(a->analyses.v[0]), "response"),
                                        get(root(a->rubric), "questions"), true, &e);
    CHECK(yyjson_mut_is_null(get(get(root(incomplete), "failed_tool_call"), "value")));
    doc_free(incomplete);
    J *second = yyjson_mut_arr_get(get(root(q), "entries"), 1);
    mode = 1;
    result = analyze_entry(a, root(q), second, &a->analyses, &e);
    CHECK(get(root(result), "error"));
    CHECK(boolean(get(root(result), "outcome_unknown")));
    doc_free(result);
    mode = 2;
    result = analyze_entry(a, root(q), second, &a->analyses, &e);
    CHECK(get(root(result), "error"));
    doc_free(result);
    mode = 3;
    result = analyze_entry(a, root(q), second, &a->analyses, &e);
    CHECK(boolean(get(root(result), "pricing_unresolved")));
    CHECK(yyjson_mut_is_null(get(root(result), "cost_usd")));
    doc_free(result);
    mode = 0;
    char *original = xstrdup(a->cfg.data_dir);
    free(a->cfg.data_dir);
    a->cfg.data_dir = xstrdup("/dev/null/unwritable");
    result = analyze_entry(a, root(q), second, &a->analyses, &e);
    CHECK(boolean(get(root(result), "result_not_saved")));
    docs_push(&a->analyses, result);
    int before = calls;
    result = analyze_entry(a, root(q), second, &a->analyses, &e);
    CHECK(calls == before);
    doc_free(result);
    free(a->cfg.data_dir);
    a->cfg.data_dir = original;
    /* Quote body remains immutable when the catalog advances. */
    Trace *tr = trace_find(&a->traces, sget(entry, "trace_id"));
    int64_t revision = integer(get(meta(tr), "revision"));
    put(tr->doc, meta(tr), "revision", ji(tr->doc, revision + 1));
    Doc *summary = trace_summary(tr, &a->analyses);
    CHECK(boolean(get(get(root(summary), "analysis"), "outdated")));
    doc_free(summary);
    CHECK(!strcmp(frozen, sget(entry, "request_body")));
    put(tr->doc, meta(tr), "revision", ji(tr->doc, revision));
    docs_push(&a->quotes, doc_clone(q));
    int status;
    uv_loop_init(&a->loop);
    Doc *ack = start_analysis(a, sget(root(q), "id"), &status);
    CHECK(status == 202);
    char *jid = xstrdup(sget(root(ack), "job_id"));
    doc_free(ack);
    ack = start_analysis(a, sget(root(q), "id"), &status);
    CHECK(!strcmp(jid, sget(root(ack), "job_id")));
    doc_free(ack);
    before = calls;
    uv_run(&a->loop, UV_RUN_DEFAULT);
    CHECK(calls == before);
    Doc *job = docs_find(&a->jobs, "id", jid);
    CHECK(!strcmp(sget(root(job), "state"), "completed"));
    free(jid);
    uv_loop_close(&a->loop);
    snapshot_free(&snapshot);
    doc_free(scope);
    doc_free(selection);
    doc_free(q);
    free(frozen);
}
static void test_chat_restart(App *a) {
    Doc *scope = parse("{\"type\":\"directory\",\"cwd\":\"/work/app\"}");
    int status;
    Doc *ack = create_chat(a, root(scope), &status);
    CHECK(status == 200);
    char *id = xstrdup(sget(root(ack), "chat_id"));
    doc_free(ack);
    Doc *chat = docs_find(&a->chats, "id", id);
    Snapshot s = {0};
    char *e = NULL;
    CHECK(snapshot_take(a, root(scope), &s, &e));
    int before = calls;
    Doc *result = chat_turn(a, chat, &s, "What do the traces show?", &e);
    CHECK(calls == before + 3);
    CHECK(yyjson_mut_arr_size(get(root(result), "messages")) == 2);
    CHECK(strstr(sget(root(result), "last_answer"), cited_id));
    docs_replace(&a->chats, "id", result);
    snapshot_free(&s);
    doc_free(scope);
    char *path = fmt("%s/chats/%s.jsonl", a->cfg.data_dir, id);
    size_t n;
    char *audit = read_file(path, MAX_SNAPSHOT, &n, &e);
    CHECK(audit && strstr(audit, "\"kind\":\"snapshot\"") && strstr(audit, "\"kind\":\"tool\"") &&
          strstr(audit, "function_call_output"));
    free(audit);
    append(path, "{incomplete");
    Doc *tail = parse("{\"kind\":\"tail_test\"}");
    CHECK(audit_append(a, id, tail, &e));
    doc_free(tail);
    audit = read_file(path, MAX_SNAPSHOT, &n, &e);
    CHECK(!strstr(audit, "{incomplete"));
    free(audit);
    free(path);
    Doc *job = parse("{\"id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"state\":\"running\"}");
    CHECK(save_doc(a, "jobs", sget(root(job), "id"), job, &e));
    doc_free(job);
    char *directory = xstrdup(a->cfg.data_dir);
    app_destroy(a);
    CHECK(app_init(a, &e));
    CHECK(docs_find(&a->chats, "id", id));
    job = docs_find(&a->jobs, "id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    CHECK(!strcmp(sget(root(job), "state"), "interrupted"));
    CHECK(a->analyses.n >= 2);
    CHECK(a->traces.n >= 2);
    CHECK(!strcmp(directory, a->cfg.data_dir));
    free(directory);
    free(id);
}
typedef struct {
    App *app;
    uv_timer_t timer;
    int ticks;
    bool both;
} Concurrent;
static void concurrent_tick(uv_timer_t *timer) {
    Concurrent *c = timer->data;
    c->ticks++;
    if (c->ticks == 1) {
        schedule_discovery(c->app);
        c->both = c->app->ai_busy && c->app->ingest_busy;
    }
    Doc *state = state_doc(c->app);
    CHECK(state);
    doc_free(state);
    if (c->ticks == 20) {
        uv_timer_stop(timer);
        uv_close((uv_handle_t *)timer, NULL);
    }
}
static void test_concurrency(App *a, const char *cp) {
    char *old_model = a->cfg.decision_model;
    a->cfg.decision_model = xstrdup("fixture-alternate-model");
    Snapshot s = {0};
    Doc *scope = parse("{\"type\":\"all\"}"), *selection = doc_new();
    J *ids = arr(selection);
    add(ids, js(selection, cited_id));
    put(selection, root(selection), "trace_ids", ids);
    char *error = NULL;
    CHECK(snapshot_take(a, root(scope), &s, &error));
    Doc *quote = estimate_build(a, &s, selection, &error);
    CHECK(quote);
    snapshot_free(&s);
    CHECK(integer(get(root(quote), "eligible")) == 1);
    docs_push(&a->quotes, doc_clone(quote));
    char *frozen =
        xstrdup(sget(yyjson_mut_arr_get(get(root(quote), "entries"), 0), "request_body"));
    append(cp, "{\"type\":\"user\",\"uuid\":\"new-turn\",\"sessionId\":\"claude-session\","
               "\"message\":{\"role\":\"user\",\"content\":\"New event after quote\"}}\n");
    uv_loop_init(&a->loop);
    Concurrent concurrent = {.app = a};
    uv_timer_init(&a->loop, &concurrent.timer);
    concurrent.timer.data = &concurrent;
    uv_timer_start(&concurrent.timer, concurrent_tick, 10, 20);
    int status, before = calls;
    mode = 4;
    Doc *ack = start_analysis(a, sget(root(quote), "id"), &status);
    CHECK(status == 202);
    uv_run(&a->loop, UV_RUN_DEFAULT);
    CHECK(concurrent.both && concurrent.ticks == 20);
    CHECK(calls == before + 1);
    CHECK(!strcmp(sent_body, frozen));
    Doc *summary = trace_summary(trace_find(&a->traces, cited_id), &a->analyses);
    CHECK(boolean(get(get(root(summary), "analysis"), "outdated")));
    doc_free(summary);
    uv_loop_close(&a->loop);
    mode = 0;
    free(frozen);
    free(a->cfg.decision_model);
    a->cfg.decision_model = old_model;
    doc_free(scope);
    doc_free(selection);
    doc_free(quote);
    doc_free(ack);
}
int main(void) {
    char tmp[] = "/tmp/traceview-c-test-XXXXXX";
    CHECK(mkdtemp(tmp));
    char *claude = fmt("%s/claude", tmp), *codex = fmt("%s/codex", tmp),
         *data = fmt("%s/data", tmp), *e = NULL;
    CHECK(mkdir_private(claude, &e));
    CHECK(mkdir_private(codex, &e));
    setenv("DATA_DIR", data, 1);
    setenv("CLAUDE_ROOT", claude, 1);
    setenv("CODEX_ROOT", codex, 1);
    setenv("OPENAI_API_KEY", "test-only", 1);
    App a;
    test_app = &a;
    CHECK(app_init(&a, &e));
    test_transport = fake;
    char *cp = fmt("%s/main.jsonl", claude), *xp = fmt("%s/main.jsonl", codex);
    fixture("tests/fixtures/claude.jsonl", cp);
    fixture("tests/fixtures/codex.jsonl", xp);
    char *normalized = absolute_path("/a//b/./c/../");
    CHECK(!strcmp(normalized, "/a/b"));
    free(normalized);
    double seconds;
    CHECK(timestamp("2026-11-08T10:00:00+01:00", &seconds));
    CHECK(!timestamp("2026-02-30T00:00:00Z", &seconds));
    CHECK(!valid_id("../../.env"));
    test_normalizers(&a, cp, xp);
    test_tools(&a);
    test_projection_limits(&a);
    test_ai(&a);
    test_chat_restart(&a);
    test_concurrency(&a, cp);
    Trace *real_c = norm("tests/fixtures/claude-sanitized-native.jsonl", "claude", NULL),
          *real_x = norm("tests/fixtures/codex-sanitized-native.jsonl", "codex", NULL);
    CHECK(integer(get(get(meta(real_c), "stats"), "events")) > 0);
    CHECK(integer(get(get(meta(real_x), "stats"), "events")) > 0);
    CHECK(!strcmp(sget(meta(real_c), "cwd"), "/work/app"));
    CHECK(!strcmp(sget(meta(real_x), "cwd"), "/work/app"));
    trace_release(real_c);
    trace_release(real_x);
    Traces out = {0};
    discover(&a, &a.traces, &out, &e);
    CHECK(!e);
    Trace *c = trace_find(&out, a.traces.v[0]->id);
    CHECK(c);
    traces_free(&a.traces);
    a.traces = out;
    memset(&out, 0, sizeof(out));
    discover(&a, &a.traces, &out, &e);
    CHECK(!e);
    CHECK(trace_find(&out, a.traces.v[0]->id) == a.traces.v[0]);
    traces_free(&out);
    CHECK(unlink(cp) == 0);
    discover(&a, &a.traces, &out, &e);
    CHECK(!e);
    bool missing = false;
    for (size_t i = 0; i < out.n; i++)
        if (boolean(get(meta(out.v[i]), "source_missing")))
            missing = true;
    CHECK(missing);
    traces_free(&out);
    app_destroy(&a);
    free(sent_body);
    free(cp);
    free(xp);
    free(claude);
    free(codex);
    free(data);
    free(e);
    printf("PASS: %d C checks; %d fake API calls; no paid requests\n", checks, calls);
    return 0;
}
