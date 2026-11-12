#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>

typedef enum { REVIEW_ANALYSIS, REVIEW_CHAT } ReviewKind;
typedef enum { EVIDENCE_AGENTIC, EVIDENCE_FULL } EvidenceMode;
typedef enum { TOOLS_DISABLED, TOOLS_ENABLED } ToolMode;

#define REVIEW_CALLS 8
static Doc *read_json(const char *path, char **error) {
    size_t n;
    char *s = read_file(path, MAX_SCHEMA_BYTES, &n, error);
    if (!s) {
        return NULL;
    }
    Doc *d = doc_parse(s, n);
    free(s);
    if (!d) {
        *error = fmt("Invalid schema: %s", path);
    }
    return d;
}
static void msg(Doc *d, J *input, const char *role, const char *text) {
    J *v = obj(d);
    put(d, v, "role", js(d, role));
    put(d, v, "content", js(d, text));
    add(input, v);
}
static char *response_text(J *r) {
    Buf b = {0};
    size_t i, n, j, k;
    J *v, *p;
    yyjson_mut_arr_foreach(get(r, "output"), i, n, v)
        yyjson_mut_arr_foreach(get(v, "content"), j, k, p) {
        if (!strcmp(sget(p, "type"), "output_text")) {
            buf_add(&b, sget(p, "text"));
        }
    }
    return b.s ? b.s : xstrdup("");
}
static bool result_valid(J *r, ReviewKind chat) {
    if (!yyjson_mut_is_obj(r)) {
        return false;
    }
    if (chat) {
        return yyjson_mut_is_str(get(r, "answer")) && *sget(r, "answer");
    }
    const char *keys[] = {"project_description", "issues_summary", "suggested_global_fix"};
    for (size_t i = 0; i < 3; i++) {
        if (!yyjson_mut_is_str(get(r, keys[i]))) {
            return false;
        }
    }
    if (strcmp(sget(r, "coverage"), "full") && strcmp(sget(r, "coverage"), "partial")) {
        return false;
    }
    if (!yyjson_mut_is_arr(get(r, "issues")) || !yyjson_mut_is_arr(get(r, "limitations"))) {
        return false;
    }
    size_t i, n;
    J *v;
    yyjson_mut_arr_foreach(get(r, "issues"), i, n, v) {
        const char *fields[] = {"title", "severity", "evidence", "explanation", "suggested_fix"};
        for (size_t j = 0; j < 5; j++) {
            if (!yyjson_mut_is_str(get(v, fields[j]))) {
                return false;
            }
        }
        if (strcmp(sget(v, "severity"), "low") && strcmp(sget(v, "severity"), "medium") &&
            strcmp(sget(v, "severity"), "high")) {
            return false;
        }
    }
    yyjson_mut_arr_foreach(get(r, "limitations"), i, n, v) if (!yyjson_mut_is_str(v)) return false;
    return true;
}
static Doc *source_list(J *source, size_t offset) {
    Doc *d = doc_new();
    put(d, root(d), "type", copy(d, get(source, "type")));
    if (!strcmp(sget(source, "type"), "trace")) {
        put(d, root(d), "trace_id", copy(d, get(source, "trace_id")));
        put(d, root(d), "revision", copy(d, get(source, "revision")));
        put(d, root(d), "bytes", ji(d, (int64_t)strlen(sget(source, "text"))));
        put(d, root(d), "path", js(d, ""));
        return d;
    }
    J *files = get(source, "files"), *items = arr(d);
    put(d, root(d), "items", items);
    size_t n = yyjson_mut_arr_size(files);
    for (size_t i = offset; i < n && i < offset + 40; i++) {
        J *v = yyjson_mut_arr_get(files, i), *row = obj(d);
        const char *keys[] = {"path", "type", "bytes", "binary", "readable"};
        for (size_t j = 0; j < 5; j++) {
            put(d, row, keys[j], copy(d, get(v, keys[j])));
        }
        add(items, row);
    }
    put(d, root(d), "next_offset", offset + 40 < n ? ji(d, (int64_t)(offset + 40)) : js(d, NULL));
    put(d, root(d), "total_entries", ji(d, (int64_t)n));
    return d;
}
static Doc *source_read(J *source, const char *path, size_t offset, size_t limit) {
    if (!strcmp(sget(source, "type"), "directory")) {
        size_t i, n;
        J *v;
        yyjson_mut_arr_foreach(
            get(source, "files"), i, n,
            v) if (!strcmp(sget(v, "path"), path) &&
                   !strcmp(sget(v, "type"), "file")) return directory_read(sget(source, "cwd"),
                                                                           path, offset, limit, v);
        return error_doc("out_of_scope", "Choose an exact file path from this directory's index");
    }
    const char *s = sget(source, "text");
    size_t n = strlen(s);
    if (*path || offset > n || (offset < n && ((unsigned char)s[offset] & 0xc0) == 0x80)) {
        return error_doc("bad_arguments",
                         "Trace reads require an empty path and a valid UTF-8 byte offset");
    }
    size_t end = utf8_end(s, n, offset + limit);
    char *chunk = xmalloc(end - offset + 1);
    memcpy(chunk, s + offset, end - offset);
    chunk[end - offset] = 0;
    Doc *d = doc_new();
    put(d, root(d), "text", js(d, chunk));
    free(chunk);
    put(d, root(d), "next_offset", end < n ? ji(d, (int64_t)end) : js(d, NULL));
    return d;
}
static Doc *source_tool(J *source, const char *name, J *args, size_t limit) {
    if (!yyjson_mut_is_obj(args) || !yyjson_mut_is_int(get(args, "offset")) ||
        integer(get(args, "offset")) < 0) {
        return error_doc("bad_arguments", "offset must be a nonnegative integer");
    }
    size_t offset = (size_t)integer(get(args, "offset"));
    if (!strcmp(name, "list_content") && yyjson_mut_obj_size(args) == 1) {
        return source_list(source, offset);
    }
    if (!strcmp(name, "read_content") && yyjson_mut_obj_size(args) == 2 &&
        yyjson_mut_is_str(get(args, "path"))) {
        return source_read(source, sget(args, "path"), offset, limit);
    }
    return error_doc("unknown_tool",
                     "Only list_content and read_content within the selected source are allowed");
}
static char *source_full(J *source, size_t budget, bool *full) {
    *full = false;
    if (!strcmp(sget(source, "type"), "trace")) {
        const char *text = sget(source, "text");
        if (token_count(text) > budget) {
            return NULL;
        }
        *full = true;
        return xstrdup(text);
    }
    if (integer(get(source, "binary_files")) || integer(get(source, "unreadable"))) {
        return NULL;
    }
    Buf b = {0};
    buf_add(&b, "DIRECTORY TREE\n");
    size_t i, n;
    J *v;
    yyjson_mut_arr_foreach(get(source, "files"), i, n, v) {
        buf_add(&b, sget(v, "path"));
        buf_add(&b, "\n");
        if (b.n > budget * 8) {
            buf_free(&b);
            return NULL;
        }
    }
    size_t tokens = token_count(b.s ? b.s : "");
    yyjson_mut_arr_foreach(get(source, "files"), i, n, v) {
        if (strcmp(sget(v, "type"), "file")) {
            continue;
        }
        char *marker = fmt("\n--- BEGIN FILE %s ---\n", sget(v, "path"));
        buf_add(&b, marker);
        tokens += token_count(marker);
        free(marker);
        size_t offset = 0;
        do {
            Doc *page = source_read(source, sget(v, "path"), offset, 32768);
            if (get(root(page), "error") || boolean(get(root(page), "binary"))) {
                doc_free(page);
                buf_free(&b);
                return NULL;
            }
            const char *text = sget(root(page), "text");
            tokens += token_count(text);
            if (tokens > budget) {
                doc_free(page);
                buf_free(&b);
                return NULL;
            }
            buf_add(&b, text);
            J *next = get(root(page), "next_offset");
            offset = yyjson_mut_is_int(next) ? (size_t)integer(next) : 0;
            doc_free(page);
        } while (offset);
        marker = fmt("\n--- END FILE %s ---\n", sget(v, "path"));
        tokens += token_count(marker);
        buf_add(&b, marker);
        free(marker);
        if (tokens > budget) {
            buf_free(&b);
            return NULL;
        }
    }
    *full = token_count(b.s ? b.s : "") <= budget;
    if (!*full) {
        buf_free(&b);
        return NULL;
    }
    return b.s ? b.s : xstrdup("Empty directory");
}
static char *initial_evidence(J *source, EvidenceMode full, const char *content) {
    if (full) {
        return fmt("Mode: full.\n%s", content);
    }
    Doc *index = source_list(source, 0);
    char *s = json(root(index));
    char *out = fmt(
        "Mode: agentic. Source content is available through read-only retrieval. Index page 0:\n%s",
        s);
    free(s);
    doc_free(index);
    return out;
}
static char *render_frozen(J *quote, const char *domain, ReviewKind chat, char **error) {
    App frozen = {0};
    Doc *prompts = doc_new(), *vars = doc_new();
    yyjson_mut_doc_set_root(prompts, copy(prompts, get(quote, "prompts")));
    frozen.prompts = prompts;
    J *dom = get(get(quote, "domains"), domain);
    put(vars, root(vars), "domain_prompt", copy(vars, get(dom, "analysis_prompt")));
    const char *kind = chat ? "review-chat" : sget(quote, "scope_type");
    char *text = prompt_render(&frozen, kind, root(vars), error);
    doc_free(vars);
    doc_free(prompts);
    return text;
}
static Doc *request_wire(Config *cfg, J *quote, J *input, const char *prompt,
                         ToolMode tools_enabled, ReviewKind chat, const char **endpoint) {
    Doc *d = doc_new();
    put(d, root(d), "model", copy(d, get(quote, "model")));
    put(d, root(d), "store", jb(d, false));
    put(d, root(d), "instructions", js(d, prompt));
    put(d, root(d), "input", copy(d, input));
    put(d, root(d), "max_output_tokens", ji(d, (int64_t)cfg->chat.output_limit));
    J *reason = obj(d);
    put(d, reason, "effort", js(d, *cfg->chat.thinking ? cfg->chat.thinking : "none"));
    put(d, root(d), "reasoning", reason);
    put(d, root(d), "parallel_tool_calls", jb(d, false));
    if (tools_enabled) {
        put(d, root(d), "tools", copy(d, get(quote, "tools")));
    }
    J *text = obj(d), *format = obj(d);
    put(d, format, "type", js(d, "json_schema"));
    put(d, format, "name", js(d, chat ? "symbiosic_chat" : "symbiosic_analysis"));
    put(d, format, "strict", jb(d, true));
    if (chat) {
        const char *raw = "{\"type\":\"object\",\"properties\":{\"answer\":{\"type\":\"string\"}},"
                          "\"required\":[\"answer\"],\"additionalProperties\":false}";
        Doc *schema = doc_parse(raw, strlen(raw));
        put(d, format, "schema", copy(d, root(schema)));
        doc_free(schema);
    } else {
        put(d, format, "schema", copy(d, get(quote, "schema")));
    }
    put(d, text, "format", format);
    put(d, root(d), "text", text);
    Doc *wire = provider_request(cfg, MODEL_GENERATION, root(d), endpoint);
    doc_free(d);
    return wire;
}
static void totals_add(Doc *d, J *usage, J *incoming) {
    const char *keys[] = {"input_tokens", "output_tokens"};
    for (size_t i = 0; i < 2; i++) {
        J *v = get(incoming, keys[i]);
        if (!yyjson_mut_is_int(v) || integer(v) < 0) {
            put(d, root(d), "pricing_unresolved", jb(d, true));
        } else {
            put(d, usage, keys[i], ji(d, integer(get(usage, keys[i])) + integer(v)));
        }
    }
    if (integer(get(get(incoming, "input_tokens_details"), "cached_tokens")) ||
        integer(get(incoming, "cache_read_input_tokens")) ||
        integer(get(incoming, "cache_creation_input_tokens"))) {
        put(d, root(d), "pricing_unresolved", jb(d, true));
    }
}
Doc *estimate_build(App *a, Snapshot *snap, Doc *selection, char **error) {
    J *sel = root(selection), *ids = get(sel, "trace_ids");
    const char *cwd = sget(sel, "cwd");
    bool directory = *cwd != 0;
    if (get(sel, "all") ||
        (directory ? yyjson_mut_arr_size(ids) != 0 : yyjson_mut_arr_size(ids) != 1)) {
        *error = xstrdup("Analyze exactly one trace or one directory at a time");
        return NULL;
    }
    ModelConfig model = a->cfg.chat;
    const char *model_id = a->cfg.chat_model;
    if (get(sel, "model_config")) {
        if (!model_config_parse(get(sel, "model_config"), &model)) {
            *error = xstrdup("Invalid analysis model configuration");
            return NULL;
        }
        model_id = sget(get(sel, "model_config"), "model");
    }
    const char *domain = *sget(sel, "domain") ? sget(sel, "domain") : "software-engineering";
    bool automatic = !strcmp(domain, "auto");
    if (!automatic && !get(root(a->domains), domain)) {
        *error = xstrdup("Unknown analysis domain");
        return NULL;
    }
    if (automatic &&
        (a->cfg.decision.provider != AI_OPENAI || strcmp(a->cfg.decision_model, "gpt-6-luna"))) {
        *error = xstrdup("Automatic detection requires OpenAI gpt-6-luna Decisions; select a "
                         "domain or update settings");
        return NULL;
    }
    Doc *d = doc_new(), *source = NULL;
    J *r = root(d);
    Trace *trace = NULL;
    if (directory) {
        source = directory_manifest(a, cwd, error);
    } else {
        trace = trace_find(&snap->traces, str(yyjson_mut_arr_get(ids, 0)));
        if (!trace) {
            *error = xstrdup("Trace not found");
            doc_free(d);
            return NULL;
        }
        J *meta = get(root(trace->doc), "meta");
        if (*sget(get(meta, "diagnostics"), "ingestion_error")) {
            *error = xstrdup(sget(get(meta, "diagnostics"), "ingestion_error"));
            doc_free(d);
            return NULL;
        }
        source = doc_new();
        char *text = fidelity_render(trace, 5, NULL);
        put(source, root(source), "text", js(source, text));
        free(text);
        put(source, root(source), "trace_id", js(source, trace->id));
        put(source, root(source), "revision", copy(source, get(meta, "revision")));
        put(source, root(source), "ingestion_incomplete",
            copy(source, get(get(meta, "diagnostics"), "coverage_incomplete")));
    }
    if (!source) {
        doc_free(d);
        return NULL;
    }
    put(source, root(source), "type", js(source, directory ? "directory" : "trace"));
    char id[33];
    if (!new_id(id)) {
        *error = xstrdup("Random ID failed");
        doc_free(d);
        doc_free(source);
        return NULL;
    }
    put(d, r, "id", js(d, id));
    put(d, r, "engine", js(d, "symbiosic-v1"));
    put(d, r, "expires_at_epoch", ji(d, (int64_t)time(NULL) + 600));
    put(d, r, "scope_type", js(d, directory ? "directory" : "trace"));
    put(d, r, "cwd", js(d, directory ? cwd : sget(get(root(trace->doc), "meta"), "cwd")));
    put(d, r, "model", js(d, model_id));
    put(d, r, "ai_provider", js(d, provider_name(model.provider)));
    put(d, r, "model_config", model_config_json(d, &model, model_id));
    put(d, r, "detection_config", model_config_json(d, &a->cfg.decision, a->cfg.decision_model));
    put(d, r, "detection_rate", jnum(d, a->cfg.rate));
    put(d, r, "domain", js(d, domain));
    put(d, r, "domains", copy(d, root(a->domains)));
    put(d, r, "prompts", copy(d, root(a->prompts)));
    Doc *schema = read_json("prompts/analysis.schema.json", error), *tools = NULL;
    if (schema) {
        tools = read_json("prompts/retrieval-tools.json", error);
    }
    if (!schema || !tools) {
        doc_free(schema);
        doc_free(tools);
        doc_free(source);
        doc_free(d);
        return NULL;
    }
    put(d, r, "schema", copy(d, root(schema)));
    put(d, r, "tools", copy(d, root(tools)));
    doc_free(schema);
    doc_free(tools);
    size_t budget = model_budget(&model),
           overhead = token_json(get(r, "schema")) + token_json(get(r, "tools")) + 2048;
    // Reserve enough for the largest possible domain prompt before optional classification.
    size_t di, dn;
    J *dk, *dv;
    yyjson_mut_obj_foreach(root(a->domains), di, dn, dk, dv) {
        size_t t = token_count(sget(dv, "analysis_prompt")) + 2048;
        if (t > overhead) {
            overhead = t;
        }
    }
    if (budget <= overhead + 512) {
        *error = xstrdup("Context budget is too small for the analysis instructions and schema");
        doc_free(source);
        doc_free(d);
        return NULL;
    }
    bool full = false;
    char *content = source_full(root(source), budget - overhead, &full);
    char *evidence = initial_evidence(root(source), (full) ? EVIDENCE_FULL : EVIDENCE_AGENTIC,
                                      content ? content : "");
    free(content);
    put(d, r, "mode", js(d, full ? "full" : "agentic"));
    put(d, r, "max_calls", ji(d, full ? 1 : REVIEW_CALLS));
    J *entries = arr(d), *entry = obj(d), *cov = obj(d);
    add(entries, entry);
    put(d, r, "entries", entries);
    put(d, entry, "trace_id", js(d, trace ? trace->id : id));
    put(d, entry, "revision",
        trace ? copy(d, get(get(root(trace->doc), "meta"), "revision")) : ji(d, 1));
    put(d, entry, "status", js(d, "eligible"));
    put(d, entry, "source", copy(d, root(source)));
    put(d, entry, "evidence", js(d, evidence));
    put(d, entry, "coverage", cov);
    put(d, cov, "level", directory ? js(d, NULL) : ji(d, 5));
    put(d, cov, "mode", js(d, full ? "full" : "agentic"));
    put(d, cov, "input_budget", ji(d, (int64_t)budget));
    put(d, cov, "incomplete", jb(d, !full || boolean(get(root(source), "ingestion_incomplete"))));
    put(d, cov, "ai_provider", js(d, provider_name(model.provider)));
    Config cfg = a->cfg;
    cfg.chat = model;
    cfg.chat_model = (char *)model_id;
    char *prompt =
        render_frozen(r, automatic ? "software-engineering" : domain, REVIEW_ANALYSIS, error);
    Doc *input = doc_new();
    yyjson_mut_doc_set_root(input, arr(input));
    msg(input, root(input), "user", evidence);
    free(evidence);
    const char *endpoint = NULL;
    Doc *wire =
        prompt ? request_wire(&cfg, r, root(input), prompt,
                              (!full) ? TOOLS_ENABLED : TOOLS_DISABLED, REVIEW_ANALYSIS, &endpoint)
               : NULL;
    free(prompt);
    doc_free(input);
    if (!wire) {
        doc_free(source);
        doc_free(d);
        return NULL;
    }
    size_t tokens = token_json(root(wire));
    if (tokens > budget) {
        *error = xstrdup(
            "Prompt and source exceed the selected model context; use a larger context budget");
        doc_free(wire);
        doc_free(source);
        doc_free(d);
        return NULL;
    }
    char *body = json(root(wire));
    put(d, entry, "request_body", js(d, body));
    free(body);
    doc_free(wire);
    put(d, cov, "input_tokens", ji(d, (int64_t)tokens));
    put(d, cov, "endpoint", js(d, endpoint));
    put(d, entry, "tokens_estimated", ji(d, (int64_t)tokens));
    double detection_cost = 0;
    if (automatic) {
        Doc *detect = doc_new();
        put(detect, root(detect), "model", js(detect, a->cfg.decision_model));
        Doc *sample = source_read(root(source), "", 0, 8000);
        if (directory) {
            doc_free(sample);
            sample = source_list(root(source), 0);
        }
        char *sample_text = json(root(sample));
        doc_free(sample);
        put(detect, root(detect), "input", js(detect, sample_text));
        free(sample_text);
        J *qs = arr(detect), *q = obj(detect), *choices = arr(detect);
        add(qs, q);
        put(detect, root(detect), "questions", qs);
        put(detect, q, "type", js(detect, "choice"));
        put(detect, q, "name", js(detect, "domain"));
        put(detect, q, "instructions", copy(detect, get(root(a->prompts), "detect")));
        put(detect, q, "choices", choices);
        yyjson_mut_obj_foreach(root(a->domains), di, dn, dk, dv) {
            J *choice = obj(detect);
            put(detect, choice, "value", copy(detect, dk));
            put(detect, choice, "description", copy(detect, get(dv, "detection_prompt")));
            add(choices, choice);
        }
        size_t dt = token_json(root(detect));
        if (dt > model_budget(&a->cfg.decision)) {
            *error = xstrdup("Domain detection prompt exceeds its model context");
            doc_free(detect);
            doc_free(source);
            doc_free(d);
            return NULL;
        }
        body = json(root(detect));
        put(d, r, "detection_body", js(d, body));
        free(body);
        doc_free(detect);
        detection_cost = dt * a->cfg.rate / 1000000.0;
    }
    put(d, r, "detection_estimated_usd", jnum(d, detection_cost));
    put(d, r, "total", ji(d, 1));
    put(d, r, "eligible", ji(d, 1));
    put(d, r, "tokens_estimated", ji(d, (int64_t)tokens));
    double estimate = tokens * model.input_rate / 1000000.0 + detection_cost;
    double maximum =
        ((full ? tokens : budget) * model.input_rate + model.output_limit * model.output_rate) /
            1000000.0 * (full ? 1 : REVIEW_CALLS) +
        detection_cost;
    put(d, r, "estimated_usd", model.pricing_known ? jnum(d, estimate) : js(d, NULL));
    put(d, r, "maximum_usd", model.pricing_known ? jnum(d, maximum) : js(d, NULL));
    put(d, r, "estimate_note",
        js(d, full ? "One structured analysis call. Output tokens are additional; maximum includes "
                     "the output reserve."
                   : "Read-only investigation, up to 8 model calls. The upper bound reserves a "
                     "full context and output budget per call."));
    char *now = now_iso();
    put(d, r, "created_at", js(d, now));
    free(now);
    doc_free(source);
    if (!save_doc(a, "quotes", id, d, error)) {
        doc_free(d);
        return NULL;
    }
    return d;
}
static Doc *run_review(App *a, J *quote, J *entry, J *extra_history, ReviewKind chat,
                       char **error) {
    ModelConfig model;
    if (!model_config_parse(get(quote, "model_config"), &model)) {
        *error = xstrdup("Invalid frozen model configuration");
        return NULL;
    }
    Config cfg = a->cfg;
    cfg.chat = model;
    cfg.chat_model = (char *)sget(quote, "model");
    size_t budget = model_budget(&model);
    Doc *d = doc_new();
    J *r = root(d), *usage = obj(d), *rounds = arr(d);
    put(d, r, "usage", usage);
    put(d, r, "rounds", rounds);
    const char *fields[] = {
        "engine",       "scope_type", "cwd",           "model",       "ai_provider",     "domain",
        "model_config", "mode",       "estimated_usd", "maximum_usd", "tokens_estimated"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
        put(d, r, fields[i], copy(d, get(quote, fields[i])));
    }
    put(d, r, "trace_id", copy(d, get(entry, "trace_id")));
    put(d, r, "revision", copy(d, get(entry, "revision")));
    put(d, r, "normalizer_version", ji(d, TV_VERSION));
    put(d, r, "coverage", copy(d, get(entry, "coverage")));
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
    put(d, r, "pricing_unresolved", jb(d, !model.pricing_known));
    char *domain = xstrdup(sget(quote, "domain"));
    double detection_cost = 0;
    if (!strcmp(domain, "auto")) {
        Transport t = provider_http(&cfg, AI_OPENAI, "decisions", sget(quote, "detection_body"));
        Doc *raw = t.body ? doc_parse(t.body, strlen(t.body)) : NULL;
        put(d, r, "detection_response", copy(d, root(raw)));
        J *answer = yyjson_mut_arr_get(get(root(raw), "answers"), 0);
        const char *chosen = sget(answer, "choice");
        if (t.error || strcmp(sget(answer, "type"), "choice") ||
            strcmp(sget(answer, "name"), "domain") || !get(get(quote, "domains"), chosen)) {
            put(d, r, "error",
                js(d,
                   t.error
                       ? t.error
                       : "Domain detection did not return a valid domain; choose one explicitly"));
            put(d, r, "outcome_unknown", jb(d, t.ambiguous));
            doc_free(raw);
            transport_free(&t);
            goto finished;
        }
        J *u = get(root(raw), "usage");
        if (yyjson_mut_is_int(get(u, "input_tokens"))) {
            detection_cost =
                integer(get(u, "input_tokens")) * number(get(quote, "detection_rate")) / 1000000.0;
        } else {
            put(d, r, "pricing_unresolved", jb(d, true));
        }
        put(d, r, "detection_usage", copy(d, u));
        put(d, r, "detection_cost_usd", jnum(d, detection_cost));
        free(domain);
        domain = xstrdup(chosen);
        put(d, r, "domain", js(d, domain));
        doc_free(raw);
        transport_free(&t);
    }
    put(d, r, "domain_name", copy(d, get(get(get(quote, "domains"), domain), "name")));
    char *prompt = render_frozen(quote, domain, (chat) ? REVIEW_CHAT : REVIEW_ANALYSIS, error);
    if (!prompt) {
        put(d, r, "error", js(d, *error));
        free(*error);
        *error = NULL;
        goto finished;
    }
    Doc *history = doc_new();
    yyjson_mut_doc_set_root(history, arr(history));
    msg(history, root(history), "user", sget(entry, "evidence"));
    size_t hi, hn;
    J *hv;
    yyjson_mut_arr_foreach(extra_history, hi, hn, hv) add(root(history), copy(history, hv));
    bool agentic = chat || !strcmp(sget(quote, "mode"), "agentic"), force_final = false;
    int max_calls = agentic ? REVIEW_CALLS : 1;
    for (int round = 0; round < max_calls; round++) {
        bool allow_tools = agentic && !force_final && round < max_calls - 1;
        const char *endpoint = NULL;
        Doc *wire = request_wire(&cfg, quote, root(history), prompt,
                                 (allow_tools) ? TOOLS_ENABLED : TOOLS_DISABLED,
                                 (chat) ? REVIEW_CHAT : REVIEW_ANALYSIS, &endpoint);
        size_t tokens = token_json(root(wire));
        if (tokens > budget) {
            doc_free(wire);
            put(d, r, "error",
                js(d, "Analysis context budget reached. No additional request was sent. Choose a "
                      "larger context window."));
            break;
        }
        char *body = json(root(wire));
        J *audit = obj(d);
        put(d, audit, "request", copy(d, root(wire)));
        doc_free(wire);
        add(rounds, audit);
        Transport t = provider_http(&cfg, model.provider, endpoint, body);
        free(body);
        put(d, audit, "raw_response", js(d, t.body));
        Doc *raw = t.body ? doc_parse(t.body, strlen(t.body)) : NULL,
            *response = provider_response(model.provider, MODEL_GENERATION, endpoint, root(raw));
        doc_free(raw);
        if (t.error || !response) {
            put(d, r, "error", js(d, t.error ? t.error : "Provider returned an invalid response"));
            put(d, r, "outcome_unknown", jb(d, t.ambiguous));
            doc_free(response);
            transport_free(&t);
            break;
        }
        totals_add(d, usage, get(root(response), "usage"));
        transport_free(&t);
        bool calls = false;
        size_t i, n;
        J *v;
        yyjson_mut_arr_foreach(get(root(response), "output"), i, n,
                               v) if (!strcmp(sget(v, "type"), "function_call")) calls = true;
        if (!calls) {
            char *text = response_text(root(response));
            Doc *result = doc_parse(text, strlen(text));
            free(text);
            if (!result_valid(root(result), (chat) ? REVIEW_CHAT : REVIEW_ANALYSIS)) {
                put(d, r, "error",
                    js(d, "The provider did not return the required structured result (or "
                          "refused). No automatic retry was made."));
            } else {
                put(d, r, "result", copy(d, root(result)));
                if (!chat) {
                    J *res = get(r, "result");
                    bool incomplete = agentic || boolean(get(get(entry, "coverage"), "incomplete"));
                    if (incomplete) {
                        put(d, res, "coverage", js(d, "partial"));
                        add(get(res, "limitations"),
                            js(d, agentic ? "Read-only investigation sampled the available source; "
                                            "unread content is not verified."
                                          : "The imported trace reports incomplete evidence."));
                    }
                }
            }
            doc_free(result);
            doc_free(response);
            break;
        }
        if (!allow_tools) {
            put(d, r, "error",
                js(d, "The model requested more tools after the investigation limit; no additional "
                      "call was made"));
            doc_free(response);
            break;
        }
        // Preserve output items, including reasoning needed by Responses tool continuations.
        yyjson_mut_arr_foreach(get(root(response), "output"), i, n, v)
            add(root(history), copy(history, v));
        size_t call_count = 0;
        yyjson_mut_arr_foreach(get(root(response), "output"), i, n, v) {
            if (strcmp(sget(v, "type"), "function_call")) {
                continue;
            }
            Doc *args = doc_parse(sget(v, "arguments"), strlen(sget(v, "arguments")));
            size_t room = budget > tokens ? budget - tokens : 0;
            size_t limit = room > 4096 ? 8192 : room > 1024 ? 2048 : 256;
            Doc *result =
                ++call_count > 8
                    ? error_doc("round_limit", "Too many tools in this round")
                    : source_tool(get(entry, "source"), sget(v, "name"), root(args), limit);
            doc_free(args);
            char *out = json(root(result));
            doc_free(result);
            J *output = obj(history);
            put(history, output, "type", js(history, "function_call_output"));
            put(history, output, "call_id", copy(history, get(v, "call_id")));
            put(history, output, "output", js(history, out));
            free(out);
            add(root(history), output);
        }
        doc_free(response);
        Doc *next = request_wire(&cfg, quote, root(history), prompt, TOOLS_ENABLED,
                                 (chat) ? REVIEW_CHAT : REVIEW_ANALYSIS, &endpoint);
        if (token_json(root(next)) > budget - 512) {
            force_final = true;
        }
        doc_free(next);
    }
    if (!get(r, "result") && !get(r, "error")) {
        put(d, r, "error", js(d, "Investigation ended without a structured result"));
    }
    free(prompt);
    doc_free(history);
finished:
    free(domain);
    put(d, r, "cost_usd",
        boolean(get(r, "pricing_unresolved"))
            ? js(d, NULL)
            : jnum(d, (integer(get(usage, "input_tokens")) * model.input_rate +
                       integer(get(usage, "output_tokens")) * model.output_rate) /
                              1000000.0 +
                          detection_cost));
    put(d, r, "calls", ji(d, (int64_t)yyjson_mut_arr_size(rounds)));
    return d;
}
Doc *analyze_entry(App *a, J *quote, J *entry, Docs *cache, char **error) {
    (void)cache;
    if (strcmp(sget(quote, "engine"), "symbiosic-v1")) {
        *error =
            xstrdup("Legacy analysis estimates are retired; create a new single-item estimate");
        return NULL;
    }
    Doc *d = run_review(a, quote, entry, NULL, REVIEW_ANALYSIS, error);
    if (!d) {
        return NULL;
    }
    char *dir = fmt("analysis/%s", sget(entry, "trace_id")), *save_error = NULL;
    if (!save_doc(a, dir, sget(root(d), "id"), d, &save_error)) {
        put(d, root(d), "result_not_saved", jb(d, true));
        put(d, root(d), "save_error", js(d, save_error));
        free(save_error);
    }
    free(dir);
    return d;
}
Doc *analysis_for_scope(App *a, J *scope) {
    Doc *best = NULL;
    const char *type = sget(scope, "type");
    for (size_t i = 0; i < a->analyses.n; i++) {
        J *r = root(a->analyses.v[i]);
        if (strcmp(sget(r, "engine"), "symbiosic-v1") || get(r, "error")) {
            continue;
        }
        bool match = !strcmp(type, "trace") ? !strcmp(sget(r, "scope_type"), "trace") &&
                                                  !strcmp(sget(r, "trace_id"), sget(scope, "id"))
                                            : !strcmp(type, "directory") &&
                                                  !strcmp(sget(r, "scope_type"), "directory") &&
                                                  !strcmp(sget(r, "cwd"), sget(scope, "cwd"));
        if (match &&
            (!best || strcmp(sget(r, "created_at"), sget(root(best), "created_at")) >= 0)) {
            best = a->analyses.v[i];
        }
    }
    return best;
}
Doc *review_chat_turn(App *a, Doc *original, Snapshot *snap, const char *text, char **error) {
    Doc *chat = doc_clone(original);
    J *scope = get(root(chat), "scope");
    Doc *analysis = NULL;
    if (get(root(chat), "analysis_snapshot")) {
        analysis = doc_new();
        yyjson_mut_doc_set_root(analysis, copy(analysis, get(root(chat), "analysis_snapshot")));
    }
    Doc *selection = doc_new();
    if (!strcmp(sget(scope, "type"), "trace")) {
        J *ids = arr(selection);
        add(ids, copy(selection, get(scope, "id")));
        put(selection, root(selection), "trace_ids", ids);
    } else {
        put(selection, root(selection), "cwd", copy(selection, get(scope, "cwd")));
    }
    put(selection, root(selection), "domain",
        js(selection, analysis ? sget(root(analysis), "domain") : "software-engineering"));
    if (analysis) {
        put(selection, root(selection), "model_config",
            copy(selection, get(root(analysis), "model_config")));
    }
    App local = *a;
    local.traces = snap->traces;
    Doc *quote = estimate_build(&local, snap, selection, error);
    doc_free(selection);
    if (!quote) {
        doc_free(analysis);
        doc_free(chat);
        return NULL;
    }
    Doc *extra = doc_new();
    yyjson_mut_doc_set_root(extra, arr(extra));
    if (analysis) {
        char *result = json(get(root(analysis), "result"));
        msg(extra, root(extra), "user", result);
        free(result);
    }
    size_t i, n;
    J *v;
    bool pending = false;
    yyjson_mut_arr_foreach(get(root(chat), "messages"), i, n, v) {
        if (boolean(get(v, "completed")) || i == n - 1) {
            msg(extra, root(extra), sget(v, "role"), sget(v, "text"));
            if (i == n - 1 && !strcmp(sget(v, "role"), "user") && !strcmp(sget(v, "text"), text)) {
                pending = true;
            }
        }
    }
    if (!pending) {
        msg(extra, root(extra), "user", text);
    }
    Doc *result = run_review(a, root(quote), yyjson_mut_arr_get(get(root(quote), "entries"), 0),
                             root(extra), REVIEW_CHAT, error);
    doc_free(extra);
    doc_free(quote);
    doc_free(analysis);
    if (!result) {
        doc_free(chat);
        return NULL;
    }
    J *last = yyjson_mut_arr_get_last(get(root(chat), "messages"));
    if (last) {
        put(chat, last, "completed", jb(chat, true));
    }
    J *message = obj(chat);
    put(chat, message, "role", js(chat, "assistant"));
    const char *failure = sget(root(result), "error");
    put(chat, message, "text",
        js(chat, *failure ? failure : sget(get(root(result), "result"), "answer")));
    put(chat, message, "completed", jb(chat, true));
    add(get(root(chat), "messages"), message);
    if (*failure) {
        put(chat, root(chat), "turn_error", js(chat, failure));
    } else {
        yyjson_mut_obj_remove_str(root(chat), "turn_error");
    }
    J *usage = get(root(chat), "usage");
    if (!usage) {
        usage = arr(chat);
        put(chat, root(chat), "usage", usage);
    }
    add(usage, copy(chat, get(root(result), "usage")));
    char *save_error = NULL;
    if (!audit_append(a, sget(root(chat), "id"), result, &save_error) ||
        !save_doc(a, "chats", sget(root(chat), "id"), chat, &save_error)) {
        put(chat, root(chat), "result_not_saved", jb(chat, true));
        free(save_error);
    }
    doc_free(result);
    return chat;
}
