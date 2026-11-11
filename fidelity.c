#include "app.h"
#include <tiktoken.h>
#include <stdlib.h>
#include <string.h>

/* Counting is local and independent of the selected provider. The single AI lane
 * owns lazy trace caches; the tokenizer is initialized before workers start. */
static CoreBPE *bpe;
bool tokenizer_init(char **error) {
    if (!bpe)
        bpe = tiktoken_o200k_base();
    if (bpe)
        return true;
    *error = xstrdup("Unable to initialize tiktoken-c o200k_base");
    return false;
}
void tokenizer_destroy(void) {
    if (bpe)
        tiktoken_destroy_corebpe(bpe);
    bpe = NULL;
}
size_t token_count(const char *text) {
    /* Ordinary encoding deliberately treats token-looking text as evidence. */
    return tiktoken_corebpe_count_ordinary(bpe, text ? text : "");
}
size_t token_json(J *value) {
    char *text = json(value);
    size_t n = token_count(text);
    free(text);
    return n;
}
size_t model_budget(ModelConfig *m) {
    size_t context = m->context_limit > m->output_limit ? m->context_limit - m->output_limit : 0;
    size_t limit = m->input_limit < context ? m->input_limit : context;
    /* Covers message framing and section-boundary BPE differences. Still a
     * heuristic for other providers, never a claim about their tokenizer. */
    return limit > 1024 ? limit - 1024 : 0;
}
typedef struct {
    J *event;
    int minimum;
    char *full, *clipped;
    size_t tokens, clipped_tokens;
} Section;
struct Fidelity {
    Section *sections;
    size_t n, totals[6];
    char *preamble, *labels[6];
};
static int minimum_level(J *ev) {
    const char *kind = sget(ev, "kind"), *role = sget(ev, "role");
    if (!strcmp(kind, "message")) {
        if (!strcmp(role, "system") || !strcmp(role, "developer"))
            return 6;
        if (!strcmp(role, "user") || !strcmp(role, "assistant"))
            return 1;
    }
    if (!strcmp(kind, "tool_result"))
        return 2;
    if (!strcmp(kind, "tool_call"))
        return 3;
    if (!strcmp(kind, "reasoning"))
        return 5;
    return 0; /* Telemetry is represented by deterministic facts, not dialogue. */
}
/* 2,500 Unicode code points of retained content (1,250 head + 1,250 tail).
 * The omission marker is extra; its tokens are counted with the section. */
static char *clip(const char *text) {
    size_t chars = 0, n = strlen(text), first = 0;
    for (size_t i = 0; i < n; i++) {
        if (((unsigned char)text[i] & 0xc0) != 0x80) {
            if (chars == 1250)
                first = i;
            chars++;
        }
    }
    if (chars <= 2500)
        return NULL;
    size_t last = n, tail = 0;
    while (last && tail < 1250) {
        last--;
        if (((unsigned char)text[last] & 0xc0) != 0x80)
            tail++;
    }
    Buf out = {0};
    buf_addn(&out, text, first);
    char *marker =
        fmt("\n[tool output middle omitted; %zu original characters, %zu original UTF-8 bytes]\n",
            chars, n);
    buf_add(&out, marker);
    buf_add(&out, text + last);
    free(marker);
    return out.s;
}
static char *event_line(J *ev, const char *replacement) {
    Doc *d = doc_new();
    yyjson_mut_doc_set_root(d, copy(d, ev));
    yyjson_mut_obj_remove_str(root(d), "source");
    if (replacement)
        put(d, root(d), "text", js(d, replacement));
    char *line = json(root(d));
    char *out = fmt("%s\n", line);
    free(line);
    doc_free(d);
    return out;
}
Fidelity *fidelity_prepare(Trace *t) {
    if (t->fidelity)
        return t->fidelity;
    Fidelity *f = xcalloc(1, sizeof(*f));
    J *events = get(root(t->doc), "events"), *m = get(root(t->doc), "meta");
    f->n = yyjson_mut_arr_size(events);
    f->sections = xcalloc(f->n, sizeof(*f->sections));
    char *facts = json(get(m, "stats"));
    f->preamble =
        fmt("Trace %s@%lld. "
            "Event reference format: [%s@%lld:seq]. Deterministic facts: %s\n",
            t->id, (long long)integer(get(m, "revision")), t->id,
            (long long)integer(get(m, "revision")), facts);
    free(facts);
    char *diagnostics = json(get(m, "diagnostics"));
    char *preamble = fmt("%sIngestion diagnostics: %s\n", f->preamble, diagnostics);
    free(diagnostics);
    free(f->preamble);
    f->preamble = preamble;
    size_t base = token_count(f->preamble);
    const char *views[] = {"prompts and answers",     "plus clipped tool outputs",
                           "plus tool-call requests", "full tool outputs",
                           "plus recorded thinking",  "plus recorded system prompt"};
    for (int l = 0; l < 6; l++) {
        f->labels[l] =
            fmt("Fidelity level %d/6: %s. %s\n", l + 1, views[l],
                l < 5 ? "Recorded components above this level are omitted."
                      : "All recorded components in the six-level scheme are retained.");
        f->totals[l] = base + token_count(f->labels[l]);
    }
    size_t i, max;
    J *ev;
    yyjson_mut_arr_foreach(events, i, max, ev) {
        Section *s = &f->sections[i];
        s->event = ev;
        s->minimum = minimum_level(ev);
        if (!s->minimum)
            continue;
        s->full = event_line(ev, NULL);
        s->tokens = token_count(s->full);
        if (s->minimum == 2) {
            char *clipped = clip(sget(ev, "text"));
            if (clipped) {
                s->clipped = event_line(ev, clipped);
                s->clipped_tokens = token_count(s->clipped);
                free(clipped);
            }
        }
        for (int l = s->minimum; l <= 6; l++)
            f->totals[l - 1] += l < 4 && s->clipped ? s->clipped_tokens : s->tokens;
    }
    /* A single recorded-system block, before every conversation turn. */
    f->totals[5] += token_count(
        "Recorded system prompt/context (once):\nEnd recorded system prompt/context.\n");
    t->fidelity = f;
    return f;
}
void fidelity_free(Fidelity *f) {
    if (!f)
        return;
    for (size_t i = 0; i < f->n; i++) {
        free(f->sections[i].full);
        free(f->sections[i].clipped);
    }
    free(f->sections);
    free(f->preamble);
    for (int l = 0; l < 6; l++)
        free(f->labels[l]);
    free(f);
}
size_t fidelity_tokens(Fidelity *f, int level) {
    return level >= 1 && level <= 6 ? f->totals[level - 1] : 0;
}
int fidelity_select(const size_t totals[6], size_t budget) {
    int best = 0;
    for (int l = 1; l <= 6; l++)
        if (totals[l - 1] <= budget && (!best || totals[l - 1] >= totals[best - 1]))
            best = l;
    return best; /* Prefer greater fidelity on ties; never assume BPE monotonicity. */
}
char *fidelity_render(Trace *t, int level, Doc **coverage) {
    Fidelity *f = fidelity_prepare(t);
    Doc *d = doc_new();
    J *r = root(d), *ids = arr(d), *shortened = arr(d), *excluded = obj(d), *levels = arr(d);
    J *diag = get(get(root(t->doc), "meta"), "diagnostics");
    put(d, r, "level", ji(d, level));
    put(d, r, "tokenizer", js(d, "tiktoken-c/o200k_base"));
    put(d, r, "clip_characters", ji(d, 2500));
    put(d, r, "input_event_ids", ids);
    put(d, r, "shortened_results", shortened);
    put(d, r, "excluded_kinds", excluded);
    put(d, r, "parse_errors", copy(d, get(diag, "malformed_lines")));
    put(d, r, "ingestion_incomplete", copy(d, get(diag, "coverage_incomplete")));
    put(d, r, "level_tokens", levels);
    for (int l = 1; l <= 6; l++)
        add(levels, ji(d, (int64_t)fidelity_tokens(f, l)));
    put(d, r, "trace_tokens", ji(d, (int64_t)fidelity_tokens(f, level)));
    bool incomplete = boolean(get(diag, "coverage_incomplete"));
    Buf b = {0};
    buf_add(&b, f->preamble);
    buf_add(&b, f->labels[level - 1]);
    if (level == 6) {
        buf_add(&b, "Recorded system prompt/context (once):\n");
        for (size_t i = 0; i < f->n; i++)
            if (f->sections[i].minimum == 6)
                buf_add(&b, f->sections[i].full);
        buf_add(&b, "End recorded system prompt/context.\n");
    }
    for (size_t i = 0; i < f->n; i++) {
        Section *s = &f->sections[i];
        if (!s->minimum || s->minimum > level) {
            const char *kind = sget(s->event, "kind");
            put(d, excluded, kind, ji(d, integer(get(excluded, kind)) + 1));
            if (s->minimum)
                incomplete = true;
            continue;
        }
        add(ids, copy(d, get(s->event, "id")));
        if (s->minimum == 6)
            continue;
        if (level < 4 && s->clipped) {
            incomplete = true;
            J *v = obj(d);
            put(d, v, "id", copy(d, get(s->event, "id")));
            put(d, v, "original_bytes", ji(d, (int64_t)strlen(sget(s->event, "text"))));
            add(shortened, v);
            buf_add(&b, s->clipped);
        } else
            buf_add(&b, s->full);
    }
    put(d, r, "incomplete", jb(d, incomplete));
    put(d, r, "projection_bytes", ji(d, (int64_t)b.n));
    put(d, r, "normalized_bytes", ji(d, (int64_t)t->bytes));
    if (coverage)
        *coverage = d;
    else
        doc_free(d);
    return b.s;
}
