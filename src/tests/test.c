#include "app.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>

#if defined(__MSYS__) || defined(__CYGWIN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <sys/cygwin.h>
#endif

static int checks = 0, calls = 0, chat_round = 0, mode = 0;
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
#if defined(__MSYS__) || defined(__CYGWIN__)
static void test_windows_acl(App *a) {
    char *path = fmt("%s/ai-settings.json", a->cfg.data_dir);
    wchar_t *native = cygwin_create_path(CCP_POSIX_TO_WIN_W, path);
    PACL acl = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    CHECK(GetNamedSecurityInfoW(native, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &acl,
                                NULL, &descriptor) == ERROR_SUCCESS);
    CHECK(acl && acl->AceCount == 1);
    ACCESS_ALLOWED_ACE *ace = NULL;
    CHECK(GetAce(acl, 0, (void **)&ace));
    CHECK(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
    HANDLE token;
    CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    TOKEN_USER *user = xmalloc(size);
    CHECK(GetTokenInformation(token, TokenUser, user, size, &size));
    CHECK(EqualSid(&ace->SidStart, user->User.Sid));
    free(user);
    CloseHandle(token);
    LocalFree(descriptor);
    free(native);
    free(path);
}
#endif

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
    char *e = NULL, *s = read_file(source, MAX_FILE_BYTES, &n, &e);
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
    if (e) {
        fprintf(stderr, "normalize: %s\n", e);
    }
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
    if (mode == 4) {
        uv_sleep(250);
    }
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
        return (Transport){.status = STATUS_OK, .body = xstrdup("not json")};
    }
    if (!strcmp(sget(get(get(root(request), "text"), "format"), "name"), "symbiosic_analysis")) {
        const char *result =
            "{\"project_description\":\"A parser project\",\"issues_summary\":\"One concrete "
            "issue\",\"suggested_global_fix\":\"Add parser validation and "
            "tests.\",\"issues\":[{\"title\":\"Missing edge-case "
            "test\",\"severity\":\"medium\",\"evidence\":\"Parser test "
            "fixture\",\"explanation\":\"No empty-input test was "
            "observed.\",\"suggested_fix\":\"Add an empty-input regression "
            "test.\"}],\"coverage\":\"full\",\"limitations\":[]}";
        J *output = arr(response), *m = obj(response), *parts = arr(response),
          *part = obj(response);
        put(response, r, "output", output);
        add(output, m);
        put(response, m, "type", js(response, "message"));
        put(response, m, "role", js(response, "assistant"));
        put(response, m, "content", parts);
        add(parts, part);
        put(response, part, "type", js(response, "output_text"));
        put(response, part, "text", js(response, result));
        J *usage = obj(response);
        put(response, r, "usage", usage);
        put(response, usage, "input_tokens", ji(response, 100));
        put(response, usage, "output_tokens", ji(response, 20));
        char *out = json(r);
        doc_free(request);
        doc_free(response);
        return (Transport){.status = STATUS_OK, .body = out};
    }
    {
        CHECK(!strcmp(endpoint, "responses"));
        CHECK(!jbool(get(root(request), "store")));
        CHECK(!jbool(get(root(request), "parallel_tool_calls")));
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
    return (Transport){.status = STATUS_OK, .body = serialized};
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
    CHECK(jbool(get(get(meta(malformed), "diagnostics"), "coverage_incomplete")));
    CHECK(integer(get(get(meta(malformed), "diagnostics"), "malformed_lines")) == 1);
    trace_release(malformed);
    unlink(bad);
    free(bad);
    Trace *context = norm("tests/fixtures/context.jsonl", "codex", NULL);
    CHECK(integer(get(get(meta(context), "stats"), "context_limits")) == 1);
    trace_release(context);
    char *system_path = fmt("%s/system.jsonl", a->cfg.codex_root);
    char *system_error = NULL;
    const char *system_record = "{\"type\":\"session_meta\",\"payload\":{\"id\":\"system-test\","
                                "\"base_instructions\":{\"text\":\"RECORDED_BASE_SYSTEM\"}}}\n";
    CHECK(atomic_write(system_path, system_record, strlen(system_record), &system_error));
    Trace *system_trace = norm(system_path, "codex", NULL);
    CHECK(contains(system_trace, "RECORDED_BASE_SYSTEM"));
    trace_release(system_trace);
    unlink(system_path);
    free(system_path);
    char *huge = fmt("%s/huge.jsonl", a->cfg.codex_root);
    Buf huge_record = {0};
    buf_add(&huge_record, "{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":"
                          "\"user\",\"content\":\"");
    char *huge_text = xmalloc(17u * 1024 * 1024 + 1);
    memset(huge_text, 'x', 17u * 1024 * 1024);
    huge_text[17u * 1024 * 1024] = 0;
    buf_add(&huge_record, huge_text);
    free(huge_text);
    buf_add(&huge_record, "\"}}\n");
    char *write_error = NULL;
    CHECK(atomic_write(huge, huge_record.s, huge_record.n, &write_error));
    buf_free(&huge_record);
    Trace *oversize = norm(huge, "codex", NULL);
    CHECK(!*sget(get(meta(oversize), "diagnostics"), "ingestion_error"));
    CHECK(oversize->bytes > 16u * 1024 * 1024);
    CHECK(trace_save(a, oversize, &write_error));
    char *saved = fmt("%s/traces/%s.jsonl", a->cfg.data_dir, oversize->id);
    Trace *reloaded = trace_load(saved, &write_error);
    CHECK(reloaded && reloaded->bytes > 16u * 1024 * 1024);
    trace_release(reloaded);
    unlink(saved);
    free(saved);
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
    CHECK(jbool(get(root(r), "truncated")));
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
static void test_fidelity(App *a) {
    CHECK(token_count("Hello, world!") == 4);
    Doc *d = parse("{\"meta\":{\"revision\":1,\"diagnostics\":{},\"stats\":{}},\"events\":[]}");
    Trace t = {.doc = d};
    strcpy(t.id, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const char *roles[] = {"user", "assistant", "tool", "assistant", "assistant", "system"};
    const char *kinds[] = {"message",   "tool_call", "tool_result",
                           "reasoning", "message",   "message"};
    const char *texts[] = {"USER_MARKER", "", "", "THINK_MARKER", "ANSWER_MARKER", "SYSTEM_MARKER"};
    for (int i = 0; i < 6; i++) {
        J *v = obj(d);
        char *id = fmt("e%d", i);
        put(d, v, "id", js(d, id));
        free(id);
        put(d, v, "seq", ji(d, i));
        put(d, v, "role", js(d, roles[i]));
        put(d, v, "kind", js(d, kinds[i]));
        put(d, v, "text", js(d, texts[i]));
        if (i == 1) {
            put(d, v, "tool", js(d, "CALL_MARKER"));
            put(d, v, "args", js(d, "ARG_MARKER"));
        }
        if (i == 2) {
            Buf text = {0};
            buf_add(&text, "HEAD_MARKER");
            for (int j = 0; j < 4000; j++) {
                buf_add(&text, "漢🙂");
            }
            buf_add(&text, "TAIL_MARKER");
            put(d, v, "text", js(d, text.s));
            buf_free(&text);
        }
        add(get(root(d), "events"), v);
    }
    Fidelity *f = fidelity_prepare(&t);
    CHECK(f == fidelity_prepare(&t));
    size_t totals[6];
    for (int l = 1; l <= 6; l++) {
        totals[l - 1] = fidelity_tokens(f, l);
    }
    for (int l = 1; l <= 6; l++) {
        CHECK(fidelity_select(totals, totals[l - 1]) == l);
        Doc *coverage = NULL;
        char *text = fidelity_render(&t, l, &coverage);
        CHECK(strstr(text, "USER_MARKER") && strstr(text, "ANSWER_MARKER"));
        CHECK((strstr(text, "HEAD_MARKER") != NULL) == (l >= 2));
        CHECK((strstr(text, "CALL_MARKER") != NULL) == (l >= 3));
        CHECK((strstr(text, "middle omitted") != NULL) == (l == 2 || l == 3));
        CHECK((strstr(text, "THINK_MARKER") != NULL) == (l >= 5));
        CHECK((strstr(text, "SYSTEM_MARKER") != NULL) == (l == 6));
        if (l == 6) {
            char *sys = strstr(text, "SYSTEM_MARKER");
            CHECK(sys < strstr(text, "USER_MARKER"));
            CHECK(!strstr(sys + 1, "SYSTEM_MARKER"));
        }
        CHECK(jbool(get(root(coverage), "incomplete")) == (l < 6));
        CHECK(token_count(text) <= totals[l - 1] + 8);
        free(text);
        doc_free(coverage);
    }
    CHECK(!fidelity_select(totals, totals[0] - 1));
    size_t nonmonotonic[] = {10, 30, 20, 25, 30, 30};
    CHECK(fidelity_select(nonmonotonic, 26) == 4);
    CHECK(fidelity_select(nonmonotonic, 30) == 6);
    fidelity_free(f);
    doc_free(d);
    (void)a;
}
static int catalog_page;
static Transport catalog_fake(const char *endpoint, const char *body, const char *key) {
    (void)key;
    CHECK(!body);
    const char *json_text;
    if (!strcmp(endpoint, "models?limit=1000")) {
        catalog_page++;
        json_text = "{\"data\":[{\"id\":\"claude-first\",\"max_input_tokens\":200000}],\"has_"
                    "more\":true,\"last_id\":\"claude-first\"}";
    } else if (!strcmp(endpoint, "models?limit=1000&after_id=claude-first")) {
        catalog_page++;
        json_text = "{\"data\":[{\"id\":\"claude-second\",\"max_input_tokens\":1000000}],\"has_"
                    "more\":false}";
    } else {
        CHECK(!strcmp(endpoint, "models"));
        json_text =
            "{\"data\":[{\"id\":\"gpt-6-luna\"},{\"id\":\"unknown-limits\"},{\"id\":\"routed-"
            "model\",\"context_length\":1000000,\"top_provider\":{\"context_length\":200000}}]}";
    }
    return (Transport){.status = STATUS_OK, .body = xstrdup(json_text)};
}
static void test_catalog(App *a) {
    TestTransport saved = test_transport;
    test_transport = catalog_fake;
    char *error = NULL;
    Doc *models = provider_models(&a->cfg, AI_OPENAI, &error);
    CHECK(models && !error);
    J *list = get(root(models), "models");
    CHECK(integer(get(yyjson_mut_arr_get(list, 0), "input_limit")) == 1050000);
    CHECK(yyjson_mut_is_null(get(yyjson_mut_arr_get(list, 1), "input_limit")));
    CHECK(integer(get(yyjson_mut_arr_get(list, 2), "context_limit")) == 200000);
    doc_free(models);
    models = provider_models(&a->cfg, AI_ANTHROPIC, &error);
    CHECK(models && !error && catalog_page == 2);
    CHECK(yyjson_mut_arr_size(get(root(models), "models")) == 2);
    doc_free(models);
    test_transport = saved;
}
static void test_providers(App *a) {
    Doc *req = parse("{\"model\":\"fixture\",\"instructions\":\"Investigate\",\"input\":[{\"role\":"
                     "\"user\",\"content\":\"Hello\"},{\"type\":\"function_call\",\"call_id\":"
                     "\"c1\",\"name\":\"get_stats\",\"arguments\":\"{}\"},{\"type\":\"function_"
                     "call_output\",\"call_id\":\"c1\",\"output\":\"evidence\"}],\"tools\":[]}");
    ModelConfig saved = a->cfg.chat;
    for (int p = AI_ANTHROPIC; p < AI_PROVIDERS; p++) {
        a->cfg.chat.provider = (Provider)p;
        const char *endpoint = NULL;
        Doc *wire = provider_request(&a->cfg, MODEL_GENERATION, root(req), &endpoint);
        CHECK(!strcmp(endpoint, p == AI_ANTHROPIC ? "messages" : "chat/completions"));
        char *body = json(root(wire));
        CHECK(strstr(body, "get_stats") && strstr(body, "evidence"));
        free(body);
        doc_free(wire);
        Doc *raw =
            parse(p == AI_ANTHROPIC
                      ? "{\"content\":[{\"type\":\"text\",\"text\":\"answer\"},{\"type\":\"tool_"
                        "use\",\"id\":\"c2\",\"name\":\"read_events\",\"input\":{}}],\"usage\":{"
                        "\"input_tokens\":10,\"output_tokens\":2}}"
                      : "{\"choices\":[{\"message\":{\"content\":\"answer\",\"tool_calls\":[{"
                        "\"id\":\"c2\",\"function\":{\"name\":\"read_events\",\"arguments\":\"{}\"}"
                        "}]}}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2}}");
        Doc *response = provider_response((Provider)p, MODEL_GENERATION, endpoint, root(raw));
        CHECK(yyjson_mut_arr_size(get(root(response), "output")) == 2);
        CHECK(integer(get(get(root(response), "usage"), "input_tokens")) == 10);
        doc_free(response);
        doc_free(raw);
    }
    a->cfg.chat = saved;
    doc_free(req);
    int status;
    Doc *settings = ai_settings(a), *saved_settings = doc_clone(settings);
    CHECK(!strstr(sget(root(settings), "key"), "test-only"));
    put(settings, get(root(settings), "chat"), "context_limit", ji(settings, 1));
    Doc *result = ai_settings_update(a, root(settings), SETTINGS_DEFAULTS, &status);
    CHECK(status == STATUS_BAD_REQUEST);
    doc_free(result);
    a->ai_busy = true;
    result = ai_settings_update(a, root(saved_settings), SETTINGS_DEFAULTS, &status);
    CHECK(status == STATUS_CONFLICT);
    doc_free(result);
    a->ai_busy = false;
    result = ai_settings_update(a, root(saved_settings), SETTINGS_DEFAULTS, &status);
    CHECK(status == STATUS_OK);
    doc_free(result);
    CHECK(ai_settings_load(a, NULL));
    doc_free(saved_settings);
    doc_free(settings);
}
static void test_chat_restart(App *a) {
    Doc *scope = parse("{\"type\":\"directory\",\"cwd\":\"/work/app\"}");
    int status;
    Doc *ack = create_chat(a, root(scope), &status);
    CHECK(status == STATUS_OK);
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
    char *audit = read_file(path, MAX_FILE_BYTES, &n, &e);
    CHECK(audit && strstr(audit, "\"kind\":\"snapshot\"") && strstr(audit, "\"kind\":\"tool\"") &&
          strstr(audit, "function_call_output"));
    free(audit);
    append(path, "{incomplete");
    Doc *tail = parse("{\"kind\":\"tail_test\"}");
    CHECK(audit_append(a, id, tail, &e));
    doc_free(tail);
    audit = read_file(path, MAX_FILE_BYTES, &n, &e);
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
    CHECK(a->analyses.n >= 1);
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
static int budget_calls;
static Transport budget_fake(const char *endpoint, const char *body, const char *key) {
    (void)key;
    CHECK(!strcmp(endpoint, "responses"));
    budget_calls++;
    free(sent_body);
    sent_body = xstrdup(body);
    return (Transport){
        .status = STATUS_OK,
        .body = xstrdup("{\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
                        "\"type\":\"output_text\",\"text\":\"Direct answer, zero tools.\"}]}]}")};
}
static void test_chat_budget(App *a) {
    Trace *original = a->traces.v[0];
    Trace t = {.doc = doc_clone(original->doc)};
    strcpy(t.id, original->id);
    J *e = obj(t.doc);
    put(t.doc, e, "kind", js(t.doc, "tool_result"));
    put(t.doc, e, "role", js(t.doc, "tool"));
    put(t.doc, e, "id", js(t.doc, "large-tool"));
    Buf large = {0};
    for (int i = 0; i < 12000; i++) {
        buf_add(&large, "long tool output with symbols 漢🙂 ");
    }
    put(t.doc, e, "text", js(t.doc, large.s));
    buf_free(&large);
    add(get(root(t.doc), "events"), e);
    Trace *traces[] = {&t};
    Snapshot snapshot = {.traces = {.v = traces, .n = 1}};
    Doc *scope = parse("{\"type\":\"all\"}");
    int status;
    Doc *ack = create_chat(a, root(scope), &status);
    CHECK(status == STATUS_OK);
    Doc *chat = docs_find(&a->chats, "id", sget(root(ack), "chat_id"));
    char *error = NULL;
    TestTransport saved_transport = test_transport;
    test_transport = budget_fake;
    Doc *result = chat_turn(a, chat, &snapshot, "Review the whole trace", &error);
    CHECK(result && !error && !get(root(result), "turn_error"));
    CHECK(budget_calls == 1);
    CHECK(strlen(sent_body) > 128u * 1024);
    CHECK(integer(get(get(root(result), "coverage"), "level")) == 6);
    size_t level3 =
        (size_t)integer(yyjson_mut_arr_get(get(get(root(result), "coverage"), "level_tokens"), 2));
    doc_free(result);
    ModelConfig saved = a->cfg.chat;
    a->cfg.chat.input_limit = level3 + 1024;
    result = chat_turn(a, chat, &snapshot, "Review the whole trace", &error);
    CHECK(result && !error);
    CHECK(integer(get(get(root(result), "coverage"), "level")) == 3);
    CHECK(strstr(sent_body, "middle omitted"));
    doc_free(result);
    put(chat, root(chat), "coverage", obj(chat));
    a->cfg.chat.input_limit = 1025;
    result = chat_turn(a, chat, &snapshot, "Review the whole trace", &error);
    CHECK(result && !error && get(root(result), "turn_error"));
    CHECK(budget_calls == 2);
    CHECK(!get(root(result), "coverage"));
    doc_free(result);
    a->cfg.chat = saved;
    test_transport = saved_transport;
    fidelity_free(t.fidelity);
    doc_free(t.doc);
    doc_free(ack);
    doc_free(scope);
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
    CHECK(status == STATUS_ACCEPTED);
    uv_run(&a->loop, UV_RUN_DEFAULT);
    CHECK(concurrent.both && concurrent.ticks == 20);
    CHECK(calls == before + 1);
    CHECK(!strcmp(sent_body, frozen));
    Doc *summary = trace_summary(trace_find(&a->traces, cited_id), &a->analyses);
    CHECK(jbool(get(get(root(summary), "analysis"), "outdated")));
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
#include "review_tests.h"
#include "provider_tests.h"
#include "title_tests.h"
#include "prompt_tests.h"

int main(void) {
    char tmp[] = "/tmp/symbiosic-c-test-XXXXXX";
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
    bool initialized = app_init(&a, &e);
    if (!initialized) {
        fprintf(stderr, "app_init: %s\n", e ? e : "Unknown initialization error");
    }
    CHECK(initialized);
    test_transport = fake;
    char *cp = fmt("%s/main.jsonl", claude), *xp = fmt("%s/main.jsonl", codex);
    fixture("tests/fixtures/claude.jsonl", cp);
    fixture("tests/fixtures/codex.jsonl", xp);
    char *normalized = absolute_path("/a//b/./c/../");
    CHECK(!strcmp(normalized, "/a/b"));
    free(normalized);
#if defined(__MSYS__) || defined(__CYGWIN__)
    char *native_path = absolute_path("C:\\work\\app\\..\\repo");
    char *slash_path = absolute_path("C:/work/repo");
    CHECK(!strcmp(native_path, slash_path));
    CHECK(native_path[0] == '/' && strstr(native_path, "/work/repo"));
    free(native_path);
    free(slash_path);
#endif
    double seconds;
    CHECK(timestamp("2026-11-08T10:00:00+01:00", &seconds));
    CHECK(!timestamp("2026-02-30T00:00:00Z", &seconds));
    CHECK(!valid_id("../../.env"));
    test_normalizers(&a, cp, xp);
    test_tools(&a);
    test_fidelity(&a);
    test_providers(&a);
#if defined(__MSYS__) || defined(__CYGWIN__)
    test_windows_acl(&a);
#endif
    test_provider_replay(&a);
    test_native_titles(&a);
    test_catalog(&a);
    test_symbiosic(&a);
    test_directory_browsing(&a);
    test_prompt_newlines(&a);
    test_chat_restart(&a);
    test_chat_budget(&a);
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
    for (size_t i = 0; i < out.n; i++) {
        if (jbool(get(meta(out.v[i]), "source_missing"))) {
            missing = true;
        }
    }
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
