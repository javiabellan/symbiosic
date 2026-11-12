#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

typedef struct {
    Doc *d;
    size_t offset, line;
    bool superseded;
} Row;
typedef struct {
    Doc *d;
    J *events, *meta;
    const char *provider;
    size_t seq, block, offset, line;
    const char *ts, *native_id;
    size_t malformed, unsupported, uncertain, mirror_only;
    bool recognized, has_responses;
    J *usage_ids, *mirror_contents;
} Normal;
enum { TITLE_BYTES = 100 };

static void title_record(Normal *n, const char *title, const char *kind) {
    if (!*title) {
        return;
    }

    /* A generated summary must not replace a user's explicit session name. */
    if (!strcmp(sget(n->meta, "title_kind"), "custom") && strcmp(kind, "custom")) {
        return;
    }

    put(n->d, n->meta, "title", js(n->d, title));
    put(n->d, n->meta, "title_kind", js(n->d, kind));
}
static bool is(J *v, const char *s) {
    return !strcmp(str(v), s);
}
static void setstr(Doc *d, J *o, const char *k, const char *s) {
    put(d, o, k, js(d, s && *s ? s : NULL));
}
static J *event(Normal *n, const char *role, const char *kind) {
    J *e = obj(n->d), *src = obj(n->d);
    char *id = fmt("b%zu:%zu", n->offset, n->block++);
    put(n->d, e, "v", ji(n->d, 1));
    put(n->d, e, "id", js(n->d, id));
    free(id);
    put(n->d, e, "seq", ji(n->d, (int64_t)n->seq++));
    setstr(n->d, e, "ts", n->ts);
    put(n->d, e, "role", js(n->d, role));
    put(n->d, e, "kind", js(n->d, kind));
    put(n->d, src, "offset", ji(n->d, (int64_t)n->offset));
    put(n->d, src, "line", ji(n->d, (int64_t)n->line));
    setstr(n->d, src, "native_id", n->native_id);
    put(n->d, e, "source", src);
    add(n->events, e);
    return e;
}
static void text_field(Normal *n, J *e, const char *text) {
    if (!text) {
        return;
    }
    put(n->d, e, "text", js(n->d, text));
}
static char *content_text(J *content, Normal *normal) {
    if (yyjson_mut_is_str(content)) {
        return xstrdup(str(content));
    }
    Buf b = {0};
    size_t i, max;
    J *v;
    yyjson_mut_arr_foreach(content, i, max, v) {
        const char *type = sget(v, "type");
        const char *s = NULL;
        if (!strcmp(type, "text") || !strcmp(type, "input_text") || !strcmp(type, "output_text") ||
            !strcmp(type, "summary_text")) {
            s = sget(v, "text");
        } else if (!strcmp(type, "image") || !strcmp(type, "input_image") ||
                   !strcmp(type, "image_url")) {
            s = "[image omitted]";
        }
        if (!s && normal) {
            normal->unsupported++;
        }
        if (s) {
            if (b.n) {
                buf_add(&b, "\n");
            }
            buf_add(&b, s);
        }
    }
    if (!b.s) {
        b.s = xstrdup("");
    }
    return b.s;
}
static bool injected(const char *s) {
    const char *markers[] = {
        "<environment_context>", "<user_instructions>",    "<permissions instructions>",
        "<turn_context>",        "<system-reminder>",      "<local-command-caveat>",
        "<command-name>",        "<local-command-stdout>", "<task-notification>",
        "<external_codex_apps_"};
    while (*s == ' ' || *s == '\n' || *s == '\t') {
        s++;
    }
    for (size_t i = 0; i < sizeof(markers) / sizeof(*markers); i++) {
        if (!strncmp(s, markers[i], strlen(markers[i]))) {
            return true;
        }
    }
    return false;
}
static void message(Normal *n, const char *role, const char *s, bool synthetic, bool uncertain) {
    if (!s || !*s) {
        return;
    }
    bool user = !strcmp(role, "user");
    if (user && (synthetic || injected(s))) {
        role = "system";
    }
    J *e = event(n, role, "message");
    text_field(n, e, s);
    if (user) {
        put(n->d, e, "human_attribution",
            js(n->d, !strcmp(role, "system") ? "system"
                     : uncertain             ? "uncertain"
                                             : "attributed"));
        if (uncertain) {
            n->uncertain++;
        }
    }
}
static void cwd_record(Normal *n, J *p) {
    const char *cwd = sget(p, "cwd");
    if (*cwd) {
        char *full = absolute_path(cwd);
        if (!*sget(n->meta, "cwd")) {
            put(n->d, n->meta, "cwd", js(n->d, full));
        }
        free(full);
    }
}
static void metadata(Normal *n, J *p) {
    J *e = event(n, "system", "metadata"), *a = obj(n->d);
    const char *keys[] = {"cwd", "model", "git_branch", "agentId", "sessionId", "id", "source"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
        J *v = get(p, keys[i]);
        if (v) {
            put(n->d, a, keys[i], copy(n->d, v));
        }
    }
    put(n->d, e, "args", a);
    const char *title = *sget(p, "title")         ? sget(p, "title")
                        : *sget(p, "thread_name") ? sget(p, "thread_name")
                                                  : sget(p, "name");
    title_record(n, title, "native");
    if (*sget(p, "model")) {
        put(n->d, n->meta, "model", copy(n->d, get(p, "model")));
    }
    if (*sget(p, "model_provider")) {
        put(n->d, n->meta, "model_provider", copy(n->d, get(p, "model_provider")));
    }
    const char *effort = *sget(p, "effort") ? sget(p, "effort") : sget(p, "reasoning_effort");
    if (*effort) {
        put(n->d, n->meta, "thinking", js(n->d, effort));
    }
}
static void usage(Normal *n, J *u, const char *identity) {
    if (!yyjson_mut_is_obj(u)) {
        return;
    }
    if (identity && *identity) {
        J *last = get(n->usage_ids, identity);
        if (last && (size_t)integer(last) != n->offset) {
            return;
        }
    }
    J *e = event(n, "assistant", "usage");
    put(n->d, e, "usage", copy(n->d, u));
}
static void args_field(Normal *n, J *e, J *args) {
    if (!args) {
        return;
    }
    put(n->d, e, "args", copy(n->d, args));
}
static void claude(Normal *n, J *r) {
    const char *type = sget(r, "type");
    if (!strcmp(type, "custom-title") || !strcmp(type, "summary")) {
        const char *title = *sget(r, "customTitle") ? sget(r, "customTitle") : sget(r, "summary");
        title_record(n, title, !strcmp(type, "custom-title") ? "custom" : "summary");
        return;
    }
    if (strcmp(type, "user") && strcmp(type, "assistant") && strcmp(type, "system")) {
        if (!strcmp(type, "queue-operation") || !strcmp(type, "file-history-snapshot") ||
            !strcmp(type, "progress")) {
            return;
        }
        n->unsupported++;
        return;
    }
    J *m = get(r, "message");
    if (*sget(m, "model")) {
        put(n->d, n->meta, "model", copy(n->d, get(m, "model")));
        put(n->d, n->meta, "model_provider", js(n->d, "anthropic"));
    }
    if ((!strcmp(type, "user") || !strcmp(type, "assistant")) && !yyjson_mut_is_obj(m)) {
        n->unsupported++;
        return;
    }
    n->recognized = true;
    cwd_record(n, r);
    if (!*sget(n->meta, "native_session_id")) {
        setstr(n->d, n->meta, "native_session_id", sget(r, "sessionId"));
    }
    if (*sget(r, "agentId")) {
        put(n->d, n->meta, "native_agent_id", copy(n->d, get(r, "agentId")));
    }
    if (*sget(r, "parentSessionId")) {
        put(n->d, n->meta, "parent_native_session_id", copy(n->d, get(r, "parentSessionId")));
    }
    if (boolean(get(r, "isSidechain"))) {
        put(n->d, n->meta, "subagent", jb(n->d, true));
        if (*sget(r, "sessionId")) {
            put(n->d, n->meta, "parent_native_session_id", copy(n->d, get(r, "sessionId")));
        }
    }
    if (!strcmp(type, "system")) {
        if (is(get(r, "subtype"), "compact_boundary")) {
            J *e = event(n, "system", "compaction");
            text_field(n, e, sget(r, "content"));
        } else {
            J *e = event(n, "system", *sget(r, "content") ? "message" : "metadata");
            text_field(n, e, sget(r, "content"));
            args_field(n, e, get(r, "compactMetadata"));
        }
        return;
    }
    if (get(r, "cwd")) {
        metadata(n, r);
    }
    if (is(get(r, "error"), "context_length_exceeded")) {
        event(n, "system", "context_limit");
    }
    J *content = get(m, "content");
    bool synthetic = boolean(get(r, "isMeta"));
    bool uncertain = get(r, "userType") && !is(get(r, "userType"), "external");
    if (yyjson_mut_is_str(content)) {
        message(n, type, str(content), synthetic, uncertain);
    } else {
        size_t i, max;
        J *b;
        yyjson_mut_arr_foreach(content, i, max, b) {
            const char *bt = sget(b, "type");
            if (!strcmp(bt, "text")) {
                message(n, type, sget(b, "text"), synthetic, uncertain);
            } else if (!strcmp(bt, "thinking")) {
                if (*sget(b, "thinking")) {
                    text_field(n, event(n, "assistant", "reasoning"), sget(b, "thinking"));
                }
            } else if (!strcmp(bt, "tool_use")) {
                J *e = event(n, "assistant", "tool_call");
                setstr(n->d, e, "call_id", sget(b, "id"));
                setstr(n->d, e, "tool", sget(b, "name"));
                args_field(n, e, get(b, "input"));
            } else if (!strcmp(bt, "tool_result")) {
                J *e = event(n, "tool", "tool_result");
                setstr(n->d, e, "call_id", sget(b, "tool_use_id"));
                char *s = content_text(get(b, "content"), n);
                text_field(n, e, s);
                free(s);
                if (yyjson_mut_is_bool(get(b, "is_error"))) {
                    put(n->d, e, "ok", jb(n->d, !boolean(get(b, "is_error"))));
                }
            } else if (!strcmp(bt, "image")) {
                message(n, type, "[image omitted]", synthetic, uncertain);
            } else if (strcmp(bt, "redacted_thinking")) {
                n->unsupported++;
            }
        }
    }
    usage(n, get(m, "usage"), sget(m, "id"));
}
static void codex(Normal *n, J *r) {
    const char *type = sget(r, "type");
    J *p = get(r, "payload");
    if (!yyjson_mut_is_obj(p)) {
        n->unsupported++;
        return;
    }
    if (!strcmp(type, "session_meta")) {
        n->recognized = true;
        cwd_record(n, p);
        if (!*sget(n->meta, "native_session_id")) {
            setstr(n->d, n->meta, "native_session_id", sget(p, "id"));
        }
        J *spawn = get(get(get(p, "source"), "subagent"), "thread_spawn");
        if (*sget(spawn, "parent_thread_id")) {
            put(n->d, n->meta, "parent_native_session_id",
                copy(n->d, get(spawn, "parent_thread_id")));
        }
        metadata(n, p);
        J *base = get(p, "base_instructions");
        const char *base_text = yyjson_mut_is_str(base) ? str(base) : sget(base, "text");
        if (*base_text) {
            message(n, "system", base_text, false, false);
        }
        return;
    }
    if (!strcmp(type, "turn_context")) {
        n->recognized = true;
        cwd_record(n, p);
        metadata(n, p);
        return;
    }
    if (!strcmp(type, "compacted")) {
        n->recognized = true;
        text_field(n, event(n, "system", "compaction"), sget(p, "message"));
        return;
    }
    if (!strcmp(type, "event_msg")) {
        const char *pt = sget(p, "type");
        if (!strcmp(pt, "token_count")) {
            n->recognized = true;
            J *info = get(p, "info");
            if (integer(get(info, "model_context_window")) > 0) {
                put(n->d, n->meta, "context_window", copy(n->d, get(info, "model_context_window")));
            }
            if (yyjson_mut_is_obj(info)) {
                usage(n, info, NULL);
            }
            return;
        }
        if (!strcmp(pt, "user_message") || !strcmp(pt, "agent_message") ||
            !strcmp(pt, "agent_reasoning")) {
            const char *role = !strcmp(pt, "user_message")    ? "user"
                               : !strcmp(pt, "agent_message") ? "assistant"
                                                              : "reasoning";
            const char *text = sget(p, !strcmp(pt, "agent_reasoning") ? "text" : "message");
            char *key = fmt("%s:%s", role, text);
            if (!get(n->mirror_contents, key)) {
                n->mirror_only++;
                n->unsupported++;
            }
            free(key);
            return;
        }
        if (!strcmp(pt, "error") && is(get(p, "error_code"), "context_length_exceeded")) {
            event(n, "system", "context_limit");
            return;
        }
        if (!strcmp(pt, "task_started") || !strcmp(pt, "task_complete") ||
            !strcmp(pt, "turn_aborted")) {
            metadata(n, p);
            return;
        }
        n->unsupported++;
        return;
    }
    if (strcmp(type, "response_item")) {
        n->unsupported++;
        return;
    }
    n->recognized = true;
    const char *pt = sget(p, "type");
    if (!strcmp(pt, "message")) {
        const char *role = sget(p, "role");
        bool unknown = strcmp(role, "user") && strcmp(role, "assistant") &&
                       strcmp(role, "system") && strcmp(role, "developer");
        if (unknown) {
            n->unsupported++;
            return;
        }
        if (!strcmp(role, "developer")) {
            role = "system";
        }
        char *s = content_text(get(p, "content"), n);
        message(n, role, s, false, false);
        free(s);
    } else if (!strcmp(pt, "function_call") || !strcmp(pt, "custom_tool_call")) {
        J *e = event(n, "assistant", "tool_call");
        setstr(n->d, e, "call_id", sget(p, "call_id"));
        setstr(n->d, e, "tool", sget(p, "name"));
        J *args = get(p, !strcmp(pt, "function_call") ? "arguments" : "input");
        Doc *parsed = NULL;
        if (!strcmp(pt, "function_call") && yyjson_mut_is_str(args)) {
            parsed = doc_parse(str(args), strlen(str(args)));
        }
        args_field(n, e, parsed ? root(parsed) : args);
        doc_free(parsed);
    } else if (!strcmp(pt, "function_call_output") || !strcmp(pt, "custom_tool_call_output")) {
        J *e = event(n, "tool", "tool_result");
        setstr(n->d, e, "call_id", sget(p, "call_id"));
        J *output = get(p, "output");
        char *s = yyjson_mut_is_str(output) ? xstrdup(str(output)) : json(output);
        text_field(n, e, s);
        free(s); /* Only explicit structured native outcomes, never string heuristics. */
        J *exit_code = get(output, "exit_code");
        if (yyjson_mut_is_int(exit_code)) {
            put(n->d, e, "ok", jb(n->d, integer(exit_code) == 0));
        } else if (yyjson_mut_is_bool(get(p, "success"))) {
            put(n->d, e, "ok", copy(n->d, get(p, "success")));
        }
    } else if (!strcmp(pt, "reasoning")) {
        char *s = content_text(get(p, "summary"), n);
        if (!*s && get(p, "content")) {
            free(s);
            s = content_text(get(p, "content"), n);
        }
        if (*s) {
            text_field(n, event(n, "assistant", "reasoning"), s);
        }
        free(s);
    } else {
        n->unsupported++;
    }
}
static void increment(Doc *d, J *o, const char *k, int64_t n) {
    put(d, o, k, ji(d, integer(get(o, k)) + n));
}
static void stats(Normal *n) {
    Doc *d = n->d;
    J *s = obj(d), *counts = obj(d), *tools = obj(d), *out = obj(d), *u = obj(d);
    put(d, n->meta, "stats", s);
    put(d, s, "event_counts", counts);
    put(d, s, "tools", tools);
    put(d, s, "tool_results", out);
    put(d, s, "usage", u);
    put(d, out, "success", ji(d, 0));
    put(d, out, "failure", ji(d, 0));
    put(d, out, "unknown", ji(d, 0));
    int64_t messages = 0, human = 0, compaction = 0, context = 0, usage_n = 0, resets = 0,
            last_human_offset = -1;
    double first = 0, last = 0;
    const char *firsts = NULL, *lasts = NULL;
    bool have = false, reliable = true;
    const char *ck[] = {"input_tokens", "output_tokens", "cache_read_input_tokens",
                        "cache_creation_input_tokens"};
    int64_t totals[4] = {0}, known[4] = {0};
    const char *kx[] = {"input_tokens", "output_tokens", "cached_input_tokens",
                        "reasoning_output_tokens", "total_tokens"};
    int64_t prev[5] = {0}, segments[5] = {0};
    bool codex_have = false;
    size_t i, max;
    J *e;
    yyjson_mut_arr_foreach(n->events, i, max, e) {
        const char *kind = sget(e, "kind");
        increment(d, counts, kind, 1);
        if (!strcmp(kind, "message")) {
            messages++;
            if (is(get(e, "human_attribution"), "attributed")) {
                int64_t off = integer(get(get(e, "source"), "offset"));
                if (off != last_human_offset) {
                    human++;
                }
                last_human_offset = off;
            }
        }
        if (!strcmp(kind, "compaction")) {
            compaction++;
        }
        if (!strcmp(kind, "context_limit")) {
            context++;
        }
        if (!strcmp(kind, "tool_call")) {
            increment(d, tools, sget(e, "tool"), 1);
        }
        if (!strcmp(kind, "tool_result")) {
            J *ok = get(e, "ok");
            increment(d, out,
                      yyjson_mut_is_bool(ok) ? (boolean(ok) ? "success" : "failure") : "unknown",
                      1);
        }
        double t;
        if (timestamp(sget(e, "ts"), &t)) {
            if (!have || t < first) {
                first = t;
                firsts = sget(e, "ts");
            }
            if (!have || t > last) {
                last = t;
                lasts = sget(e, "ts");
            }
            have = true;
        }
        if (strcmp(kind, "usage")) {
            continue;
        }
        usage_n++;
        J *raw = get(e, "usage");
        if (!strcmp(n->provider, "claude")) {
            for (int k = 0; k < 4; k++) {
                J *v = get(raw, ck[k]);
                if (yyjson_mut_is_int(v) && integer(v) >= 0) {
                    if (known[k] < 0) {
                        continue;
                    }
                    if (totals[k] > INT64_MAX - integer(v)) {
                        known[k] = -1;
                        continue;
                    }
                    totals[k] += integer(v);
                    known[k]++;
                }
            }
        } else {
            J *total = get(raw, "total_token_usage");
            bool valid = yyjson_mut_is_obj(total);
            for (int k = 0; k < 5; k++) {
                if (!yyjson_mut_is_int(get(total, kx[k])) || integer(get(total, kx[k])) < 0) {
                    valid = false;
                }
            }
            if (!valid) {
                reliable = false;
                continue;
            }
            int64_t next[5];
            bool reset = false;
            for (int k = 0; k < 5; k++) {
                next[k] = integer(get(total, kx[k]));
                if (codex_have && next[k] < prev[k]) {
                    reset = true;
                }
            }
            if (reset) {
                resets++;
                for (int k = 0; k < 5; k++) {
                    if (segments[k] > INT64_MAX - prev[k]) {
                        reliable = false;
                    } else {
                        segments[k] += prev[k];
                    }
                }
            }
            for (int k = 0; k < 5; k++) {
                prev[k] = next[k];
            }
            codex_have = true;
        }
    }
    put(d, s, "events", ji(d, (int64_t)n->seq));
    put(d, s, "messages", ji(d, messages));
    put(d, s, "human_turns", ji(d, human));
    put(d, s, "compactions", ji(d, compaction));
    put(d, s, "context_limits", ji(d, context));
    put(d, s, "context_limit_detection",
        js(d, context ? "observed" : "supported explicit fields only; otherwise unknown"));
    setstr(d, s, "first_ts", firsts);
    setstr(d, s, "last_ts", lasts);
    put(d, s, "span_seconds", have ? jnum(d, last - first) : js(d, NULL));
    put(d, s, "span_label", js(d, "Observed wall-clock span (includes idle time)"));
    put(d, u, "provider", js(d, n->provider));
    put(d, u, "telemetry_events", ji(d, usage_n));
    put(d, u, "resets", ji(d, resets));
    put(d, u, "semantics",
        js(d, !strcmp(n->provider, "claude") ? "deduplicated message components"
                                             : "sum of latest totals in monotonic segments"));
    if (!strcmp(n->provider, "claude")) {
        for (int k = 0; k < 4; k++) {
            put(d, u, ck[k], usage_n && known[k] == usage_n ? ji(d, totals[k]) : js(d, NULL));
        }
        put(d, u, "known", jb(d, usage_n && known[0] == usage_n && known[1] == usage_n));
    } else {
        for (int k = 0; k < 5; k++) {
            if (segments[k] > INT64_MAX - prev[k]) {
                reliable = false;
            }
        }
        for (int k = 0; k < 5; k++) {
            put(d, u, kx[k], reliable && codex_have ? ji(d, segments[k] + prev[k]) : js(d, NULL));
        }
        put(d, u, "known", jb(d, reliable && codex_have));
    }
}
static bool signatures_match(struct stat *a, struct stat *b) {
#ifdef __APPLE__
    struct timespec x = a->st_mtimespec, y = b->st_mtimespec;
#else
    struct timespec x = a->st_mtim, y = b->st_mtim;
#endif
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           x.tv_sec == y.tv_sec && x.tv_nsec == y.tv_nsec;
}
Trace *normalize(const char *path, const char *provider, Trace *old, char **error) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        *error = fmt("Cannot open source: %s", strerror(errno));
        return NULL;
    }
    struct stat st, after, current;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        *error = xstrdup("Source is not a regular file");
        return NULL;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > MAX_FILE_BYTES) {
        close(fd);
        *error = xstrdup("Source cannot be represented in this process address space");
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    char *bytes = xmalloc(size + 1);
    size_t got = 0;
    while (got < size) {
        ssize_t r = read(fd, bytes + got, size - got);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    bytes[got] = 0;
    bool consistent = got == size && !fstat(fd, &after) && !lstat(path, &current) &&
                      signatures_match(&st, &after) && st.st_dev == current.st_dev &&
                      st.st_ino == current.st_ino;
    close(fd);
    if (!consistent) {
        free(bytes);
        *error = xstrdup("Source changed while reading; deferred to next poll");
        return NULL;
    }
    Trace *t = xcalloc(1, sizeof(*t));
    atomic_init(&t->refs, 1);
    t->doc = doc_new();
    t->path = xstrdup(path);
    if (old) {
        strcpy(t->id, old->id);
    } else if (!new_id(t->id)) {
        free(bytes);
        trace_release(t);
        *error = xstrdup("Random ID generation failed");
        return NULL;
    }
    Normal n = {.d = t->doc, .provider = provider, .recognized = old != NULL};
    n.events = arr(n.d);
    n.meta = obj(n.d);
    n.usage_ids = obj(n.d);
    n.mirror_contents = obj(n.d);
    put(n.d, root(n.d), "meta", n.meta);
    put(n.d, root(n.d), "events", n.events);
    put(n.d, n.meta, "v", ji(n.d, 1));
    put(n.d, n.meta, "kind", js(n.d, "meta"));
    put(n.d, n.meta, "trace_id", js(n.d, t->id));
    put(n.d, n.meta, "provider", js(n.d, provider));
    put(n.d, n.meta, "normalizer_version", ji(n.d, TV_VERSION));
    put(n.d, n.meta, "source_path", js(n.d, path));
    put(n.d, n.meta, "cwd", js(n.d, NULL));
    put(n.d, n.meta, "native_session_id", js(n.d, NULL));
    put(n.d, n.meta, "parent_trace_id", js(n.d, NULL));
    Row *rows = NULL;
    const char *detected = NULL;
    size_t nr = 0, cap = 0, offset = 0, line = 1;
    while (offset < got) {
        char *nl = memchr(bytes + offset, '\n', got - offset);
        if (!nl) {
            break;
        }
        size_t len = (size_t)(nl - (bytes + offset));
        Doc *d = doc_parse(bytes + offset, len);
        if (!d) {
            n.malformed++;
        } else {
            if (nr == cap) {
                cap = cap ? cap * 2 : 128;
                rows = xrealloc(rows, cap * sizeof(*rows));
            }
            rows[nr] = (Row){d, offset, line, false};
            J *r = root(d);
            if (!detected) {
                const char *type = sget(r, "type");
                if ((!strcmp(type, "user") || !strcmp(type, "assistant")) &&
                    yyjson_mut_is_obj(get(r, "message"))) {
                    detected = "claude";
                } else if (!strcmp(type, "session_meta") || !strcmp(type, "turn_context") ||
                           !strcmp(type, "response_item") || !strcmp(type, "compacted")) {
                    detected = "codex";
                } else if (!strcmp(type, "system") &&
                           !strcmp(sget(r, "subtype"), "compact_boundary")) {
                    detected = "claude";
                }
            }
            if (is(get(r, "type"), "response_item")) {
                J *p = get(r, "payload");
                bool msg = is(get(p, "type"), "message"), reason = is(get(p, "type"), "reasoning");
                if (msg || reason) {
                    char *text = content_text(get(p, msg ? "content" : "summary"), NULL);
                    char *key = fmt("%s:%s", msg ? sget(p, "role") : "reasoning", text);
                    put(n.d, n.mirror_contents, key, jb(n.d, true));
                    free(key);
                    free(text);
                }
            }
            if (*sget(r, "uuid")) {
                for (size_t k = nr; k > 0; k--) {
                    if (!strcmp(sget(root(rows[k - 1].d), "uuid"), sget(r, "uuid"))) {
                        rows[k - 1].superseded = true;
                        break;
                    }
                }
            }
            nr++;
        }
        offset += (len + 1);
        line++;
    }
    if (detected) {
        provider = detected;
        n.provider = detected;
        put(n.d, n.meta, "provider", js(n.d, detected));
    }
    /* Grouping follows the first recorded cwd, including an earlier UUID revision. */
    for (size_t k = 0; k < nr && !*sget(n.meta, "cwd"); k++) {
        J *r = root(rows[k].d);
        const char *type = sget(r, "type");
        if (!strcmp(provider, "claude") &&
            (!strcmp(type, "user") || !strcmp(type, "assistant") || !strcmp(type, "system"))) {
            cwd_record(&n, r);
        } else if (!strcmp(provider, "codex") &&
                   (!strcmp(type, "session_meta") || !strcmp(type, "turn_context"))) {
            cwd_record(&n, get(r, "payload"));
        }
    }
    /* Usage is emitted at the last native message occurrence, preserving source order. */
    if (!strcmp(provider, "claude")) {
        for (size_t k = 0; k < nr; k++) {
            J *m = get(root(rows[k].d), "message");
            if (!rows[k].superseded && *sget(m, "id") && yyjson_mut_is_obj(get(m, "usage"))) {
                put(n.d, n.usage_ids, sget(m, "id"), ji(n.d, (int64_t)rows[k].offset));
            }
        }
    }
    for (size_t k = 0; k < nr; k++) {
        if (rows[k].superseded) {
            continue;
        }
        n.offset = rows[k].offset;
        n.line = rows[k].line;
        n.block = 0;
        J *r = root(rows[k].d);
        n.ts = sget(r, "timestamp");
        n.native_id = sget(r, "uuid");
        if (!strcmp(provider, "claude")) {
            claude(&n, r);
        } else {
            codex(&n, r);
        }
    }
    size_t ei, en;
    J *ev;
    double previous = 0;
    bool has_previous = false;
    yyjson_mut_arr_foreach(n.events, ei, en, ev) {
        Buf text = {0};
        buf_add(&text, sget(ev, "text"));
        if (get(ev, "args")) {
            char *args = json(get(ev, "args"));
            buf_add(&text, args);
            free(args);
        }
        put(n.d, ev, "tokens", ji(n.d, (int64_t)token_count(text.s ? text.s : "")));
        buf_free(&text);
        if (!*sget(n.meta, "title") && !strcmp(sget(ev, "role"), "user") &&
            !strcmp(sget(ev, "kind"), "message")) {
            const char *prompt = sget(ev, "text");
            size_t len = utf8_end(prompt, strlen(prompt), TITLE_BYTES);
            char *title = xmalloc(len + 1);
            memcpy(title, prompt, len);
            title[len] = 0;
            for (char *p = title; *p; p++) {
                if (*p == '\n' || *p == '\r') {
                    *p = ' ';
                }
            }
            title_record(&n, title, "prompt");
            free(title);
        }
        double ts;
        if (timestamp(sget(ev, "ts"), &ts)) {
            if (has_previous && ts >= previous &&
                (!strcmp(sget(ev, "role"), "assistant") ||
                 !strcmp(sget(ev, "kind"), "tool_result"))) {
                double begin = previous;
                if (!strcmp(sget(ev, "kind"), "tool_result") && *sget(ev, "call_id")) {
                    for (size_t k = ei; k > 0; k--) {
                        J *call = yyjson_mut_arr_get(n.events, k - 1);
                        if (!strcmp(sget(call, "kind"), "tool_call") &&
                            !strcmp(sget(call, "call_id"), sget(ev, "call_id"))) {
                            double start;
                            if (timestamp(sget(call, "ts"), &start)) {
                                begin = start;
                            }
                            break;
                        }
                    }
                }
                put(n.d, ev, "duration_seconds", jnum(n.d, ts - begin));
            }
            previous = ts;
            has_previous = true;
        }
    }
    if (!*sget(n.meta, "title")) {
        put(n.d, n.meta, "title", js(n.d, "Untitled session"));
    }
    for (size_t k = 0; k < nr; k++) {
        doc_free(rows[k].d);
    }
    free(rows);
    free(bytes);
    if (!n.recognized && !n.malformed) {
        trace_release(t);
        return NULL;
    }
    J *diag = obj(n.d);
    put(n.d, n.meta, "diagnostics", diag);
    put(n.d, diag, "malformed_lines", ji(n.d, (int64_t)n.malformed));
    put(n.d, diag, "unsupported_records", ji(n.d, (int64_t)n.unsupported));
    put(n.d, diag, "uncertain_speakers", ji(n.d, (int64_t)n.uncertain));
    put(n.d, diag, "mirror_only_records", ji(n.d, (int64_t)n.mirror_only));
    put(n.d, diag, "pending_final_bytes", ji(n.d, (int64_t)(got - offset)));
    put(n.d, diag, "coverage_incomplete", jb(n.d, n.malformed || n.unsupported || n.uncertain));
    const char *ingest_error = !n.recognized && n.malformed
                                   ? "No usable transcript records; malformed complete lines"
                                   : NULL;
    if (ingest_error) {
        put(n.d, diag, "ingestion_error",
            js(n.d, ingest_error)); /* No partial giant trace masquerades as usable evidence. */
        n.events = arr(n.d);
        put(n.d, root(n.d), "events", n.events);
        n.seq = 0;
        if (old) {
            J *om = get(root(old->doc), "meta");
            put(n.d, n.meta, "cwd", copy(n.d, get(om, "cwd")));
            put(n.d, n.meta, "native_session_id", copy(n.d, get(om, "native_session_id")));
        }
    }
    stats(&n);
    t->dev = (uint64_t)st.st_dev;
    t->ino = (uint64_t)st.st_ino;
    t->size = (uint64_t)st.st_size;
#ifdef __APPLE__
    t->mtime_sec = st.st_mtimespec.tv_sec;
    t->mtime_nsec = st.st_mtimespec.tv_nsec;
#else
    t->mtime_sec = st.st_mtim.tv_sec;
    t->mtime_nsec = st.st_mtim.tv_nsec;
#endif
    /* Build comparable objects in the same key order; source metadata is not a revision. */
    bool changed = true;
    if (old) {
        Doc *a = doc_new(), *b = doc_new();
        const char *keys[] = {"provider",
                              "cwd",
                              "native_session_id",
                              "native_agent_id",
                              "subagent",
                              "parent_native_session_id",
                              "stats"};
        J *om = get(root(old->doc), "meta");
        for (size_t k = 0; k < sizeof(keys) / sizeof(*keys); k++) {
            put(a, root(a), keys[k], copy(a, get(n.meta, keys[k])));
            put(b, root(b), keys[k], copy(b, get(om, keys[k])));
        }
        put(a, root(a), "events", copy(a, n.events));
        put(b, root(b), "events", copy(b, get(root(old->doc), "events")));
        J *ad = copy(a, diag), *bd = copy(b, get(om, "diagnostics"));
        yyjson_mut_obj_remove_str(ad, "pending_final_bytes");
        yyjson_mut_obj_remove_str(bd, "pending_final_bytes");
        put(a, root(a), "diagnostics", ad);
        put(b, root(b), "diagnostics", bd);
        char *sa = json(root(a)), *sb = json(root(b));
        changed = strcmp(sa, sb) != 0;
        free(sa);
        free(sb);
        doc_free(a);
        doc_free(b);
    }
    int64_t rev = old ? integer(get(get(root(old->doc), "meta"), "revision")) : 0;
    put(n.d, n.meta, "revision", ji(n.d, rev + (changed ? 1 : 0)));
    put(n.d, n.meta, "source_missing", jb(n.d, false));
    J *sig = obj(n.d);
    put(n.d, n.meta, "source_signature", sig);
    put(n.d, sig, "dev", ji(n.d, (int64_t)t->dev));
    put(n.d, sig, "ino", ji(n.d, (int64_t)t->ino));
    put(n.d, sig, "size", ji(n.d, (int64_t)t->size));
    put(n.d, sig, "mtime_sec", ji(n.d, t->mtime_sec));
    put(n.d, sig, "mtime_nsec", ji(n.d, t->mtime_nsec));
    char *serialized = json(root(n.d));
    t->bytes = strlen(serialized);
    free(serialized);
    return t;
}
