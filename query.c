#include "app.h"
#include <stdlib.h>
#include <string.h>

static J *meta(Trace *t) {
    return get(root(t->doc), "meta");
}
Doc *analysis_index(Doc *analysis) {
    Doc *d = doc_clone(analysis);
    if (boolean(get(root(d), "result_not_saved")))
        return d;
    const char *keys[] = {"request",  "request_body",   "raw_response",
                          "response", "rubric_content", "estimate", "rounds", "detection_response"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++)
        yyjson_mut_obj_remove_str(root(d), keys[i]);
    yyjson_mut_obj_remove_str(get(root(d), "coverage"), "input_event_ids");
    return d;
}
Doc *analysis_best(Docs *a, Trace *t) {
    Doc *best = NULL;
    bool exact = false;
    int64_t rev = integer(get(meta(t), "revision"));
    for (size_t i = 0; i < a->n; i++) {
        J *r = root(a->v[i]);
        if (get(r,"error") || strcmp(sget(r,"engine"),"symbiosic-v1") || strcmp(sget(r, "trace_id"), t->id))
            continue;
        bool match =
            integer(get(r, "revision")) == rev &&
            integer(get(r, "normalizer_version")) == integer(get(meta(t), "normalizer_version"));
        if (!best || (match && !exact) ||
            (match == exact &&
             strcmp(sget(r, "created_at"), sget(root(best), "created_at")) >= 0)) {
            best = a->v[i];
            exact = match;
        }
    }
    return best;
}
static J *analysis_summary(Doc *d, Doc *analysis, Trace *t) {
    if (!analysis)
        return js(d, NULL);
    J *r = root(analysis), *v = obj(d);
    const char *keys[] = {
        "id",       "revision",           "created_at",       "model",   "values", "usage",
        "cost_usd", "pricing_unresolved", "result_not_saved", "coverage", "result", "domain", "domain_name", "model_config", "ai_provider", "estimated_usd", "maximum_usd", "engine", "calls"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++)
        put(d, v, keys[i], copy(d, get(r, keys[i])));
    /* Projection event IDs stay in saved analyses, not every compact catalog row. */
    J *cov = get(v, "coverage");
    if (yyjson_mut_is_obj(cov))
        yyjson_mut_obj_remove_str(cov, "input_event_ids");
    put(d, v, "outdated",
        jb(d, integer(get(r, "revision")) != integer(get(meta(t), "revision")) ||
                  integer(get(r, "normalizer_version")) !=
                      integer(get(meta(t), "normalizer_version"))));
    return v;
}
Doc *trace_summary(Trace *t, Docs *analyses) {
    Doc *d = doc_new();
    J *m = meta(t);
    const char *keys[] = {"trace_id",
                          "revision",
                          "provider",
                          "native_session_id", "title", "model", "thinking", "model_provider", "context_window",
                          "cwd",
                          "parent_trace_id",
                          "parent_native_session_id",
                          "subagent",
                          "source_missing",
                          "stats",
                          "diagnostics"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++)
        put(d, root(d), keys[i], copy(d, get(m, keys[i])));
    put(d, root(d), "analysis", analysis_summary(d, analysis_best(analyses, t), t));
    return d;
}
static void inc(Doc *d, J *o, const char *k, int64_t n) {
    J *old = get(o, k);
    if (yyjson_mut_is_null(old))
        return;
    int64_t value = integer(old);
    put(d, o, k, n > 0 && value > INT64_MAX - n ? js(d, NULL) : ji(d, value + n));
}
static J *empty_stats(Doc *d) {
    J *r = obj(d);
    const char *counts[] = {"traces",
                            "events",
                            "human_turns",
                            "compactions",
                            "context_limits",
                            "known_tool_results",
                            "failed_tool_results",
                            "unknown_tool_results",
                            "analysis_current",
                            "analysis_outdated",
                            "coverage_incomplete",
                            "explicit_subagents",
                            "overlapping_session_spans"};
    for (size_t i = 0; i < sizeof(counts) / sizeof(*counts); i++)
        put(d, r, counts[i], ji(d, 0));
    put(d, r, "provider_usage", obj(d));
    put(d, r, "categories", obj(d));
    put(d, r, "scores", obj(d));
    return r;
}
static void accumulate(Doc *d, J *r, Trace *t, Docs *analyses) {
    J *m = meta(t), *s = get(m, "stats");
    inc(d, r, "traces", 1);
    const char *cs[] = {"events", "human_turns", "compactions", "context_limits"};
    for (size_t i = 0; i < 4; i++)
        inc(d, r, cs[i], integer(get(s, cs[i])));
    J *tr = get(s, "tool_results");
    inc(d, r, "known_tool_results", integer(get(tr, "success")) + integer(get(tr, "failure")));
    inc(d, r, "failed_tool_results", integer(get(tr, "failure")));
    inc(d, r, "unknown_tool_results", integer(get(tr, "unknown")));
    if (boolean(get(get(m, "diagnostics"), "coverage_incomplete")))
        inc(d, r, "coverage_incomplete", 1);
    if (*sget(m, "parent_trace_id") || *sget(m, "parent_native_session_id"))
        inc(d, r, "explicit_subagents", 1);
    J *providers = get(r, "provider_usage");
    const char *provider = sget(m, "provider");
    J *u = get(providers, provider);
    if (!u) {
        u = obj(d);
        put(d, providers, provider, u);
    }
    J *usage = get(s, "usage");
    inc(d, u, "traces", 1);
    if (!boolean(get(usage, "known")))
        inc(d, u, "unknown_usage_traces", 1);
    const char *components[] = {"input_tokens",
                                "output_tokens",
                                "cache_read_input_tokens",
                                "cache_creation_input_tokens",
                                "cached_input_tokens",
                                "reasoning_output_tokens",
                                "total_tokens"};
    for (size_t i = 0; i < 7; i++) {
        J *v = get(usage, components[i]);
        if (yyjson_mut_is_int(v)) {
            inc(d, u, components[i], integer(v));
            char *k = fmt("%s_samples", components[i]);
            inc(d, u, k, 1);
            free(k);
        }
    }
    Doc *an = analysis_best(analyses, t);
    if (!an)
        return;
    bool outdated =
        integer(get(root(an), "revision")) != integer(get(m, "revision")) ||
        integer(get(root(an), "normalizer_version")) != integer(get(m, "normalizer_version"));
    inc(d, r, outdated ? "analysis_outdated" : "analysis_current", 1);
    if (outdated)
        return;
    J *values = get(root(an), "values"), *cats = get(r, "categories"), *scores = get(r, "scores");
    size_t i, max;
    J *key, *v;
    yyjson_mut_obj_foreach(values, i, max, key, v) {
        const char *name = str(key);
        J *value = get(v, "value");
        if (yyjson_mut_is_num(value)) {
            J *sc = get(scores, name);
            if (!sc) {
                sc = obj(d);
                put(d, scores, name, sc);
            }
            put(d, sc, "sum", jnum(d, number(get(sc, "sum")) + number(value)));
            inc(d, sc, "n", 1);
        } else if (yyjson_mut_is_str(value) || yyjson_mut_is_bool(value) ||
                   (yyjson_mut_is_null(value) && strcmp(sget(v, "type"), "score") &&
                    strcmp(name, "activities"))) {
            J *ca = get(cats, name);
            if (!ca) {
                ca = obj(d);
                put(d, cats, name, ca);
            }
            const char *label = yyjson_mut_is_bool(value)   ? (boolean(value) ? "true" : "false")
                                : yyjson_mut_is_null(value) ? "unknown"
                                                            : str(value);
            inc(d, ca, label, 1);
        }
    }
}
static void finish_stats(Doc *d, J *r) {
    int64_t denom = integer(get(r, "known_tool_results"));
    put(d, r, "known_error_rate",
        denom ? jnum(d, (double)integer(get(r, "failed_tool_results")) / denom) : js(d, NULL));
    J *scores = get(r, "scores");
    size_t i, max;
    J *key, *v;
    yyjson_mut_obj_foreach(scores, i, max, key, v) {
        (void)key;
        put(d, v, "mean", jnum(d, number(get(v, "sum")) / (double)integer(get(v, "n"))));
        yyjson_mut_obj_remove_str(v, "sum");
    }
    put(d, r, "category_note",
        js(d, "Only current analyses; multi-label counts can exceed trace count. Unknown scores "
              "excluded."));
}
static bool shorten_field(Doc *d, J *o, const char *key, size_t max) {
    const char *text = sget(o, key);
    size_t n = strlen(text);
    if (n <= max)
        return false;
    put(d, o, key, yyjson_mut_strncpy(d, text, utf8_end(text, n, max)));
    return true;
}
static J *reference(Doc *d, Trace *t) {
    J *r = obj(d);
    put(d, r, "trace_id", js(d, t->id));
    put(d, r, "revision", copy(d, get(meta(t), "revision")));
    return r;
}
static void coverage(Doc *d, Snapshot *s) {
    size_t incomplete = 0;
    for (size_t i = 0; i < s->traces.n; i++)
        if (boolean(get(get(meta(s->traces.v[i]), "diagnostics"), "coverage_incomplete")))
            incomplete++;
    J *c = obj(d);
    put(d, c, "scoped_traces", ji(d, (int64_t)s->traces.n));
    put(d, c, "incomplete_traces", ji(d, (int64_t)incomplete));
    put(d, root(d), "coverage", c);
}
/* Trim complete items only. The cursor always describes the next omitted item. */
static void bound_items(Doc *d, size_t offset, size_t total) {
    J *items = get(root(d), "items");
    size_t n = yyjson_mut_arr_size(items);
    for (;;) {
        put(d, root(d), "truncated", jb(d, offset + n < total));
        put(d, root(d), "next_offset",
            offset + n < total ? ji(d, (int64_t)(offset + n)) : js(d, NULL));
        char *s = json(root(d));
        size_t bytes = strlen(s);
        free(s);
        if (bytes <= MAX_TOOL - 64 || !n)
            break;
        yyjson_mut_arr_remove_last(items);
        n--;
    }
}
Doc *aggregate(Snapshot *s, const char *group_by, size_t offset, size_t limit) {
    Doc *d = doc_new();
    J *all = empty_stats(d), *groups = obj(d), *items = arr(d);
    put(d, root(d), "stats", all);
    put(d, root(d), "items", items);
    J *refs = arr(d);
    put(d, root(d), "revision_references", refs);
    size_t ref_limit = s->traces.n < 50 ? s->traces.n : 50;
    for (size_t ri = 0; ri < ref_limit; ri++)
        add(refs, reference(d, s->traces.v[ri]));
    put(d, root(d), "revision_references_truncated", jb(d, s->traces.n > ref_limit));
    put(d, root(d), "revision_reference_note",
        js(d, "Snapshot references shown for the first 50 sessions; paginate list_sessions for "
              "every reference. Totals cover the entire scope."));
    coverage(d, s);
    for (size_t i = 0; i < s->traces.n; i++) {
        Trace *t = s->traces.v[i];
        accumulate(d, all, t, &s->analyses);
        const char *name =
            !strcmp(group_by, "provider") ? sget(meta(t), "provider") : sget(meta(t), "cwd");
        if (!*name)
            name = "Unknown directory";
        if (strcmp(group_by, "none")) {
            J *group = get(groups, name);
            if (!group) {
                group = empty_stats(d);
                put(d, groups, name, group);
            }
            accumulate(d, group, t, &s->analyses);
        }
    }
    typedef struct {
        double start, end;
        bool valid;
    } Span;
    Span *spans = xcalloc(s->traces.n, sizeof(*spans));
    for (size_t i = 0; i < s->traces.n; i++) {
        J *metrics = get(meta(s->traces.v[i]), "stats");
        spans[i].valid = timestamp(sget(metrics, "first_ts"), &spans[i].start) &&
                         timestamp(sget(metrics, "last_ts"), &spans[i].end);
    }
    for (size_t i = 0; i < s->traces.n; i++)
        for (size_t j = i + 1; j < s->traces.n; j++)
            if (spans[i].valid && spans[j].valid && spans[i].start < spans[j].end &&
                spans[j].start < spans[i].end) {
                inc(d, all, "overlapping_session_spans", 1);
                if (strcmp(group_by, "none")) {
                    const char *key = !strcmp(group_by, "provider") ? "provider" : "cwd";
                    const char *a = sget(meta(s->traces.v[i]), key),
                               *b = sget(meta(s->traces.v[j]), key);
                    if (!strcmp(a, b))
                        inc(d, get(groups, *a ? a : "Unknown directory"),
                            "overlapping_session_spans", 1);
                }
            }
    free(spans);
    finish_stats(d, all);
    size_t i, max;
    J *key, *v;
    yyjson_mut_obj_foreach(groups, i, max, key, v) {
        finish_stats(d, v);
        if (i >= offset && yyjson_mut_arr_size(items) < limit) {
            J *item = obj(d);
            put(d, item, "group", copy(d, key));
            if (limit <= 50 && shorten_field(d, item, "group", 256))
                put(d, item, "group_shortened", jb(d, true));
            put(d, item, "stats", copy(d, v));
            add(items, item);
        }
    }
    if (limit <= 50)
        bound_items(d, offset, yyjson_mut_obj_size(groups));
    else {
        put(d, root(d), "truncated", jb(d, false));
        put(d, root(d), "next_offset", js(d, NULL));
    }
    return d;
}
static char *event_view(J *ev) {
    Buf b = {0};
    buf_add(&b, sget(ev, "text"));
    if (*sget(ev, "tool")) {
        if (b.n)
            buf_add(&b, "\n");
        buf_add(&b, "Tool: ");
        buf_add(&b, sget(ev, "tool"));
    }
    if (get(ev, "args")) {
        char *a = json(get(ev, "args"));
        if (b.n)
            buf_add(&b, "\n");
        buf_add(&b, "Arguments: ");
        buf_add(&b, a);
        free(a);
    }
    const char *fields[] = {"call_id", "ts"};
    for (size_t i = 0; i < 2; i++)
        if (strlen(sget(ev, fields[i])) > 256) {
            buf_add(&b, "\n");
            buf_add(&b, fields[i]);
            buf_add(&b, ": ");
            buf_add(&b, sget(ev, fields[i]));
        }
    const char *native = sget(get(ev, "source"), "native_id");
    if (strlen(native) > 256) {
        buf_add(&b, "\nNative ID: ");
        buf_add(&b, native);
    }
    if (get(ev, "usage")) {
        char *raw = json(get(ev, "usage"));
        if (strlen(raw) > 512) {
            buf_add(&b, "\nRaw usage: ");
            buf_add(&b, raw);
        }
        free(raw);
    }
    return b.s ? b.s : xstrdup("");
}
Doc *events_page(Trace *t, size_t start, size_t limit, size_t text_offset) {
    if (!limit)
        limit = 1;
    if (limit > 50)
        limit = 50;
    Doc *d = doc_new();
    J *items = arr(d);
    put(d, root(d), "items", items);
    put(d, root(d), "reference", reference(d, t));
    put(d, root(d), "coverage", copy(d, get(meta(t), "diagnostics")));
    J *events = get(root(t->doc), "events");
    size_t total = yyjson_mut_arr_size(events);
    if (text_offset && limit != 1) {
        doc_free(d);
        return error_doc("bad_arguments", "text_offset requires limit=1");
    }
    for (size_t i = start; i < total && i - start < limit; i++) {
        J *original = yyjson_mut_arr_get(events, i), *e = copy(d, original);
        char *view = event_view(original);
        size_t n = strlen(view);
        if (text_offset > n ||
            (text_offset < n && ((unsigned char)view[text_offset] & 0xc0) == 0x80)) {
            free(view);
            doc_free(d);
            return error_doc("bad_arguments",
                             "text_offset must be a UTF-8 boundary within event text_view");
        }
        size_t begin = limit == 1 ? text_offset : 0,
               end = utf8_end(view, n, begin + (limit == 1 ? 8192 : 512));
        char save = view[end];
        view[end] = 0;
        yyjson_mut_obj_remove_str(e, "text");
        yyjson_mut_obj_remove_str(e, "args");
        bool metadata_shortened = shorten_field(d, e, "tool", 256);
        metadata_shortened = shorten_field(d, e, "call_id", 256) || metadata_shortened;
        metadata_shortened = shorten_field(d, e, "ts", 256) || metadata_shortened;
        metadata_shortened =
            shorten_field(d, get(e, "source"), "native_id", 256) || metadata_shortened;
        if (get(e, "usage")) {
            char *raw = json(get(e, "usage"));
            if (strlen(raw) > 512) {
                yyjson_mut_obj_remove_str(e, "usage");
                metadata_shortened = true;
            }
            free(raw);
        }
        put(d, e, "metadata_shortened", jb(d, metadata_shortened));
        put(d, e, "text_view", js(d, view + begin));
        view[end] = save;
        put(d, e, "text_bytes", ji(d, (int64_t)n));
        put(d, e, "text_offset", ji(d, (int64_t)begin));
        put(d, e, "shortened", jb(d, begin > 0 || end < n));
        put(d, e, "next_text_offset", end < n ? ji(d, (int64_t)end) : js(d, NULL));
        put(d, e, "trace_id", js(d, t->id));
        put(d, e, "revision", copy(d, get(meta(t), "revision")));
        add(items, e);
        /* JSON escaping can multiply text size. Shrink a single slice, never the item. */
        if (limit == 1) {
            for (;;) {
                char *serialized = json(root(d));
                size_t bytes = strlen(serialized);
                free(serialized);
                if (bytes < MAX_TOOL - 160 || end <= begin)
                    break;
                end = utf8_end(view, n, begin + (end - begin) / 2);
                put(d, e, "text_view", yyjson_mut_strncpy(d, view + begin, end - begin));
                put(d, e, "shortened", jb(d, true));
                put(d, e, "next_text_offset", ji(d, (int64_t)end));
            }
        }
        free(view);
    }
    bound_items(d, start, total);
    put(d, root(d), "next_seq", copy(d, get(root(d), "next_offset")));
    return d;
}
static bool whole(J *v) {
    return yyjson_mut_is_int(v) && integer(v) >= 0;
}
static bool exact_keys(J *args, const char **keys, size_t n) {
    if (!yyjson_mut_is_obj(args) || yyjson_mut_obj_size(args) != n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!get(args, keys[i]))
            return false;
    return true;
}
Doc *tool_run(Snapshot *s, const char *name, J *args) {
    const char *listkeys[] = {"offset", "limit"}, *statkeys[] = {"group_by", "offset", "limit"},
               *searchkeys[] = {"query", "trace_id", "offset", "limit"},
               *readkeys[] = {"trace_id", "start_seq", "limit", "text_offset"};
    bool list = !strcmp(name, "list_sessions"), stats = !strcmp(name, "get_stats"),
         search = !strcmp(name, "search_events"), read = !strcmp(name, "read_events");
    if (!(list || stats || search || read))
        return error_doc("unknown_tool", "Tool is not available");
    if (!exact_keys(args,
                    list     ? listkeys
                    : stats  ? statkeys
                    : search ? searchkeys
                             : readkeys,
                    list    ? 2
                    : stats ? 3
                            : 4) ||
        !whole(get(args, "limit")) || !whole(get(args, read ? "start_seq" : "offset")))
        return error_doc("bad_arguments",
                         "Required fields, integer limits and nonnegative offsets are required");
    size_t offset = (size_t)integer(get(args, read ? "start_seq" : "offset")),
           limit = (size_t)integer(get(args, "limit"));
    if (limit < 1)
        limit = 1;
    if (limit > 50)
        limit = 50;
    if (read) {
        if (!yyjson_mut_is_str(get(args, "trace_id")) || !whole(get(args, "text_offset")))
            return error_doc("bad_arguments", "Invalid trace_id or text_offset");
        Trace *t = trace_find(&s->traces, sget(args, "trace_id"));
        if (!t)
            return error_doc("outside_scope", "Trace is not in this frozen scope");
        return events_page(t, offset, limit, (size_t)integer(get(args, "text_offset")));
    }
    if (stats) {
        const char *group = sget(args, "group_by");
        if (strcmp(group, "none") && strcmp(group, "provider") && strcmp(group, "directory"))
            return error_doc("bad_arguments", "group_by must be none, provider or directory");
        return aggregate(s, group, offset, limit);
    }
    if (search) {
        J *id = get(args, "trace_id");
        if (!yyjson_mut_is_str(get(args, "query")) || strlen(sget(args, "query")) > 256 ||
            (!yyjson_mut_is_null(id) && !yyjson_mut_is_str(id)))
            return error_doc(
                "bad_arguments",
                "query is a literal string up to 256 bytes; trace_id is string or null");
        if (yyjson_mut_is_str(id) && !trace_find(&s->traces, str(id)))
            return error_doc("outside_scope", "Trace is not in this frozen scope");
    }
    Doc *d = doc_new();
    J *items = arr(d);
    put(d, root(d), "items", items);
    coverage(d, s);
    size_t found = 0;
    for (size_t i = 0; i < s->traces.n; i++) {
        Trace *t = s->traces.v[i];
        if (list) {
            if (found >= offset && yyjson_mut_arr_size(items) < limit) {
                Doc *summary = trace_summary(t, &s->analyses);
                J *item = copy(d, root(summary));
                bool compacted = false;
                const char *fields[] = {"cwd", "native_session_id", "parent_native_session_id"};
                for (size_t fi = 0; fi < 3; fi++)
                    compacted = shorten_field(d, item, fields[fi], 256) || compacted;
                put(d, item, "metadata_shortened", jb(d, compacted));
                J *metrics = get(item, "stats");
                const char *omit[] = {"event_counts", "tools", "span_label",
                                      "context_limit_detection"};
                for (size_t oi = 0; oi < 4; oi++)
                    yyjson_mut_obj_remove_str(metrics, omit[oi]);
                J *a = get(item, "analysis");
                if (yyjson_mut_is_obj(a)) {
                    yyjson_mut_obj_remove_str(a, "coverage");
                    yyjson_mut_obj_remove_str(a, "usage");
                    J *values = get(a, "values");
                    size_t vi, vn;
                    J *key, *value;
                    yyjson_mut_obj_foreach(values, vi, vn, key, value) {
                        (void)key;
                        yyjson_mut_obj_remove_str(value, "answer");
                    }
                }
                add(items, item);
                doc_free(summary);
            }
            found++;
            continue;
        }
        if (yyjson_mut_is_str(get(args, "trace_id")) && strcmp(t->id, sget(args, "trace_id")))
            continue;
        J *events = get(root(t->doc), "events");
        size_t j, max;
        J *ev;
        yyjson_mut_arr_foreach(events, j, max, ev) {
            char *view = event_view(ev);
            char *match = strstr(view, sget(args, "query"));
            if (match) {
                if (found >= offset && yyjson_mut_arr_size(items) < limit) {
                    J *item = reference(d, t);
                    put(d, item, "seq", copy(d, get(ev, "seq")));
                    put(d, item, "id", copy(d, get(ev, "id")));
                    put(d, item, "kind", copy(d, get(ev, "kind")));
                    size_t n = strlen(view), pos = (size_t)(match - view),
                           begin = utf8_end(view, n, pos > 80 ? pos - 80 : 0),
                           end = utf8_end(view, n, begin + 400);
                    view[end] = 0;
                    put(d, item, "excerpt", js(d, view + begin));
                    put(d, item, "text_offset", ji(d, (int64_t)begin));
                    add(items, item);
                }
                found++;
            }
            free(view);
        }
    }
    put(d, root(d), "total_matches", ji(d, (int64_t)found));
    bound_items(d, offset, found);
    return d;
}

Doc *trace_overview(Trace *t) {
    Doc *d=doc_new();J *items=arr(d);put(d,root(d),"items",items);size_t i,n;J *v;
    yyjson_mut_arr_foreach(get(root(t->doc),"events"),i,n,v){
        const char *kind=sget(v,"kind");if(strcmp(kind,"message")&&strcmp(kind,"reasoning")&&strcmp(kind,"tool_call")&&strcmp(kind,"tool_result"))continue;
        J *row=obj(d);const char *keys[]={"seq","role","kind","tool","ts","tokens","duration_seconds"};for(size_t k=0;k<7;k++)put(d,row,keys[k],copy(d,get(v,keys[k])));
        add(items,row);
    }
    return d;
}
