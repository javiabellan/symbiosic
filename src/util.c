#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#if defined(__MSYS__) || defined(__CYGWIN__)
#include <sys/cygwin.h>
#endif

static void oom(void) {
    fputs("symbiosic: allocation failed; exiting without replacing unfinished files\n", stderr);
    exit(1);
}
void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        oom();
    }
    return p;
}
void *xcalloc(size_t n, size_t s) {
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) {
        oom();
    }
    return p;
}
void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) {
        oom();
    }
    return p;
}
char *xstrdup(const char *s) {
    if (!s) {
        s = "";
    }
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}
char *fmt(const char *format, ...) {
    va_list a, b;
    va_start(a, format);
    va_copy(b, a);
    int n = vsnprintf(NULL, 0, format, b);
    va_end(b);
    if (n < 0) {
        oom();
    }
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, format, a);
    va_end(a);
    return s;
}
void buf_addn(Buf *b, const char *s, size_t n) {
    if (n > SIZE_MAX - b->n - 1) {
        oom();
    }
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = xrealloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
void buf_add(Buf *b, const char *s) {
    if (s) {
        buf_addn(b, s, strlen(s));
    }
}
void buf_free(Buf *b) {
    free(b->s);
    memset(b, 0, sizeof(*b));
}
size_t utf8_end(const char *s, size_t n, size_t end) {
    if (end >= n) {
        return n;
    }
    while (end > 0 && ((unsigned char)s[end] & 0xc0) == 0x80) {
        end--;
    }
    return end;
}
static void *jm(void *ctx, size_t n) {
    (void)ctx;
    return xmalloc(n);
}
static void *jr(void *ctx, void *p, size_t old, size_t n) {
    (void)ctx;
    (void)old;
    return xrealloc(p, n);
}
static void jf(void *ctx, void *p) {
    (void)ctx;
    free(p);
}
static const yyjson_alc allocator = {jm, jr, jf, NULL};
Doc *doc_new(void) {
    Doc *d = yyjson_mut_doc_new(&allocator);
    if (!d) {
        oom();
    }
    yyjson_mut_doc_set_root(d, yyjson_mut_obj(d));
    return d;
}
Doc *doc_parse(const char *s, size_t n) {
    yyjson_doc *r = yyjson_read_opts((char *)s, n, 0, &allocator, NULL);
    if (!r) {
        return NULL;
    }
    Doc *d = yyjson_doc_mut_copy(r, &allocator);
    yyjson_doc_free(r);
    return d;
}
Doc *doc_clone(Doc *d) {
    return yyjson_mut_doc_mut_copy(d, &allocator);
}
J *root(Doc *d) {
    return d ? yyjson_mut_doc_get_root(d) : NULL;
}
J *get(J *o, const char *k) {
    return yyjson_mut_obj_get(o, k);
}
const char *str(J *v) {
    const char *s = yyjson_mut_get_str(v);
    return s ? s : "";
}
const char *sget(J *o, const char *k) {
    return str(get(o, k));
}
int64_t integer(J *v) {
    return yyjson_mut_get_sint(v);
}
double number(J *v) {
    return yyjson_mut_get_num(v);
}
bool boolean(J *v) {
    return yyjson_mut_get_bool(v);
}
J *obj(Doc *d) {
    return yyjson_mut_obj(d);
}
J *arr(Doc *d) {
    return yyjson_mut_arr(d);
}
J *js(Doc *d, const char *s) {
    return s ? yyjson_mut_strcpy(d, s) : yyjson_mut_null(d);
}
J *ji(Doc *d, int64_t n) {
    return yyjson_mut_sint(d, n);
}
J *jb(Doc *d, bool b) {
    return yyjson_mut_bool(d, b);
}
J *jnum(Doc *d, double n) {
    return isfinite(n) ? yyjson_mut_real(d, n) : yyjson_mut_null(d);
}
J *copy(Doc *d, J *v) {
    return v ? yyjson_mut_val_mut_copy(d, v) : yyjson_mut_null(d);
}
void put(Doc *d, J *o, const char *k, J *v) {
    if (!v || !yyjson_mut_obj_put(o, js(d, k), v)) {
        oom();
    }
}
void add(J *a, J *v) {
    if (!v || !yyjson_mut_arr_append(a, v)) {
        oom();
    }
}
char *json(J *v) {
    if (!v) {
        return xstrdup("null");
    }
    char *s = yyjson_mut_val_write_opts(v, 0, &allocator, NULL, NULL);
    if (!s) {
        oom();
    }
    return s;
}
void doc_free(Doc *d) {
    yyjson_mut_doc_free(d);
}
Doc *error_doc(const char *code, const char *message) {
    Doc *d = doc_new();
    J *e = obj(d);
    put(d, root(d), "error", e);
    put(d, e, "code", js(d, code));
    put(d, e, "message", js(d, message));
    return d;
}
void docs_push(Docs *a, Doc *d) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->v = xrealloc(a->v, a->cap * sizeof(*a->v));
    }
    a->v[a->n++] = d;
}
void docs_free(Docs *a) {
    for (size_t i = 0; i < a->n; i++) {
        doc_free(a->v[i]);
    }
    free(a->v);
    memset(a, 0, sizeof(*a));
}
Doc *docs_find(Docs *a, const char *key, const char *id) {
    for (size_t i = 0; i < a->n; i++) {
        if (!strcmp(sget(root(a->v[i]), key), id)) {
            return a->v[i];
        }
    }
    return NULL;
}
void docs_replace(Docs *a, const char *key, Doc *d) {
    const char *id = sget(root(d), key);
    for (size_t i = 0; i < a->n; i++) {
        if (!strcmp(sget(root(a->v[i]), key), id)) {
            doc_free(a->v[i]);
            a->v[i] = d;
            return;
        }
    }
    docs_push(a, d);
}
char *now_iso(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    struct tm tm;
    gmtime_r(&t.tv_sec, &tm);
    char s[32];
    strftime(s, sizeof(s), "%Y-%m-%dT%H:%M:%S", &tm);
    return fmt("%s.%09ldZ", s, t.tv_nsec);
}
/* UTC conversion independent of the host timezone; reject unknown/malformed dates. */
bool timestamp(const char *s, double *out) {
    int y, m, d, h, mi;
    double sec;
    int used = 0;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%lf%n", &y, &m, &d, &h, &mi, &sec, &used) != 6) {
        return false;
    }
    if (y < 1970 || m < 1 || m > 12 || d < 1 || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 ||
        sec >= 60) {
        return false;
    }
    int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) {
        days[1] = 29;
    }
    if (d > days[m - 1]) {
        return false;
    }
    int off = 0;
    if (s[used] == 'Z' && !s[used + 1]) {
    } else {
        int oh, om, n = 0;
        if ((s[used] != '+' && s[used] != '-') ||
            sscanf(s + used + 1, "%d:%d%n", &oh, &om, &n) != 2 || s[used + 1 + n] || oh > 23 ||
            om > 59) {
            return false;
        }
        off = (oh * 60 + om) * 60 * (s[used] == '+' ? 1 : -1);
    }
    int yy = y - (m <= 2);
    int era = yy / 400;
    unsigned yo = (unsigned)(yy - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yo * 365 + yo / 4 - yo / 100 + doy;
    *out = ((double)era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60 + sec - off;
    return true;
}
bool valid_id(const char *s) {
    if (!s || strlen(s) != 32) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}
bool new_id(char out[33]) {
    unsigned char r[16];
    if (uv_random(NULL, NULL, r, sizeof(r), 0, NULL)) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        snprintf(out + i * 2, 3, "%02x", r[i]);
    }
    return true;
}
char *absolute_path(const char *p) {
    if (!p || !*p) {
        return xstrdup("");
    }
    char *expanded;
    if (p[0] == '~' && p[1] == '/') {
        expanded = fmt("%s/%s", getenv("HOME") ? getenv("HOME") : "", p + 2);
    } else {
        expanded = xstrdup(p);
    }
#if defined(__MSYS__) || defined(__CYGWIN__)
    /* Native Windows transcripts record drive paths; map them into the POSIX runtime. */
    if ((isalpha((unsigned char)expanded[0]) && expanded[1] == ':') ||
        (expanded[0] == '\\' && expanded[1] == '\\')) {
        ssize_t size = cygwin_conv_path(CCP_WIN_A_TO_POSIX, expanded, NULL, 0);
        if (size < 0) {
            free(expanded);
            return xstrdup("");
        }
        char *converted = xmalloc((size_t)size);
        int result = cygwin_conv_path(CCP_WIN_A_TO_POSIX, expanded, converted, (size_t)size);
        free(expanded);
        if (result) {
            free(converted);
            return xstrdup("");
        }
        expanded = converted;
    }
#endif
    char *full;
    if (expanded[0] != '/') {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) {
            free(expanded);
            return xstrdup("");
        }
        full = fmt("%s/%s", cwd, expanded);
        free(expanded);
    } else {
        full = expanded;
    }
    char **parts = xcalloc(strlen(full) + 1, sizeof(char *));
    size_t n = 0;
    char *save = NULL;
    for (char *q = strtok_r(full, "/", &save); q; q = strtok_r(NULL, "/", &save)) {
        if (!strcmp(q, ".")) {
            continue;
        }
        if (!strcmp(q, "..")) {
            if (n) {
                n--;
            }
            continue;
        }
        parts[n++] = q;
    }
    Buf b = {0};
#if defined(__MSYS__) || defined(__CYGWIN__)
    if (full[0] == '/' && full[1] == '/') {
        buf_add(&b, "/");
    }
#endif
    buf_add(&b, "/");
    for (size_t i = 0; i < n; i++) {
        if (i) {
            buf_add(&b, "/");
        }
        buf_add(&b, parts[i]);
    }
    free(parts);
    free(full);
    return b.s;
}
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = 0;
    }
    return s;
}
bool config_load(Config *c, char **error) {
    memset(c, 0, sizeof(*c));
    FILE *f = fopen(".env", "r");
    if (f) {
        char *line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, f) >= 0) {
            char *s = trim(line);
            if (!*s || *s == '#') {
                continue;
            }
            char *eq = strchr(s, '=');
            if (!eq) {
                *error = xstrdup("Invalid .env line (expected literal KEY=value)");
                free(line);
                fclose(f);
                return false;
            }
            *eq = 0;
            char *k = trim(s), *v = trim(eq + 1);
            bool valid = *k && (isalpha((unsigned char)*k) || *k == '_');
            for (char *p = k; *p; p++) {
                if (!isalnum((unsigned char)*p) && *p != '_') {
                    valid = false;
                }
            }
            if (!valid) {
                *error = xstrdup("Invalid .env key");
                free(line);
                fclose(f);
                return false;
            }
            size_t n = strlen(v);
            if (n >= 2 &&
                ((v[0] == '\"' && v[n - 1] == '\"') || (v[0] == '\'' && v[n - 1] == '\''))) {
                v[n - 1] = 0;
                v++;
            }
            if (!getenv(k)) {
                setenv(k, v, 0);
            }
        }
        free(line);
        fclose(f);
    } else if (errno != ENOENT) {
        *error = xstrdup("Unable to read .env");
        return false;
    }
    const char *v;
#define ENV(k, def) ((v = getenv(k)) ? v : (def))
    c->key = xstrdup(ENV("OPENAI_API_KEY", ""));
    c->data_dir = absolute_path(ENV("DATA_DIR", "./data"));
    c->claude_root = absolute_path(ENV("CLAUDE_ROOT", "~/.claude/projects"));
    c->codex_root = absolute_path(ENV("CODEX_ROOT", "~/.codex/sessions"));
    c->decision_model = xstrdup(ENV("DECISION_MODEL", "gpt-6-luna"));
    c->chat_model = xstrdup(ENV("CHAT_MODEL", "gpt-6-luna"));
    c->decision = (ModelConfig){.provider = AI_OPENAI,
                                .input_limit = 1050000,
                                .context_limit = 1050000,
                                .output_limit = 4096,
                                .thinking = "none"};
    c->chat = (ModelConfig){.provider = AI_OPENAI,
                            .input_limit = 1050000,
                            .context_limit = 1050000,
                            .output_limit = 8192,
                            .thinking = "none"};
    if (strcmp(c->decision_model, "gpt-6-luna")) {
        c->decision.input_limit = c->decision.context_limit = 0;
    }
    if (strcmp(c->chat_model, "gpt-6-luna")) {
        c->chat.input_limit = c->chat.context_limit = 0;
    }
    c->provider_keys[AI_ANTHROPIC] = xstrdup(ENV("ANTHROPIC_API_KEY", ""));
    c->provider_keys[AI_OPENROUTER] = xstrdup(ENV("OPENROUTER_API_KEY", ""));
    c->provider_keys[AI_GEMINI] = xstrdup(ENV("GEMINI_API_KEY", ""));
    char *end;
    const char *s = ENV("PORT", "4317");
    long port = strtol(s, &end, 10);
    if (!*s || *end || port < 1 || port > 65535) {
        goto invalid;
    }
    c->port = (int)port;
    s = ENV("POLL_MS", "2000");
    long poll = strtol(s, &end, 10);
    if (!*s || *end || poll < 100 || poll > 3600000) {
        goto invalid;
    }
    c->poll_ms = (uint64_t)poll;
    s = ENV("DECISION_INPUT_USD_PER_M", "0.10");
    c->rate = strtod(s, &end);
    if (!*s || *end || !isfinite(c->rate) || c->rate < 0) {
        goto invalid;
    }
    if (!*c->decision_model || !*c->chat_model) {
        goto invalid;
    }
    return true;
invalid:
    *error = xstrdup("Invalid PORT, POLL_MS, model, or Decisions rate in configuration");
    return false;
#undef ENV
}
void config_free(Config *c) {
    free(c->key);
    for (int i = 0; i < AI_PROVIDERS; i++) {
        free(c->provider_keys[i]);
    }
    free(c->data_dir);
    free(c->claude_root);
    free(c->codex_root);
    free(c->decision_model);
    free(c->chat_model);
    memset(c, 0, sizeof(*c));
}
