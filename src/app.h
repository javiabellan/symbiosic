#ifndef SYMBIOSIC_APP_H
#define SYMBIOSIC_APP_H
#define _POSIX_C_SOURCE 200809L
#include <uv.h>
#include <yyjson.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include <stdatomic.h>

#define TV_VERSION 4
#define MAX_SOURCES 1000
#define MAX_FILE_BYTES (SIZE_MAX - 1)
#define MAX_SCHEMA_BYTES (128u * 1024)
#define MAX_TOOL (16u * 1024)
#define TITLE_CUSTOM "custom"
#define PRICE_DATE "2026-11-08"
typedef enum {
    STATUS_OK = 200,
    STATUS_ACCEPTED = 202,
    STATUS_REDIRECT = 300,
    STATUS_BAD_REQUEST = 400,
    STATUS_FORBIDDEN = 403,
    STATUS_NOT_FOUND = 404,
    STATUS_CONFLICT = 409,
    STATUS_GONE = 410,
    STATUS_TOO_LARGE = 413,
    STATUS_MEDIA_TYPE = 415,
    STATUS_SERVER_ERROR = 500,
    STATUS_BAD_GATEWAY = 502,
    STATUS_UNAVAILABLE = 503
} HttpStatus;

typedef enum { MODEL_GENERATION, MODEL_DECISION } ModelRole;
typedef enum { SETTINGS_DEFAULTS, SETTINGS_CONNECTION } SettingsKind;

typedef enum { AI_OPENAI, AI_ANTHROPIC, AI_OPENROUTER, AI_GEMINI, AI_PROVIDERS } Provider;
typedef struct {
    Provider provider;
    size_t input_limit, context_limit, output_limit;
    char thinking[16];
    double input_rate, output_rate;
    bool pricing_known;
} ModelConfig;
typedef struct Fidelity Fidelity;
typedef yyjson_mut_doc Doc;
typedef yyjson_mut_val J;
typedef struct {
    char *s;
    size_t n, cap;
} Buf;
typedef struct {
    char *key, *data_dir, *claude_root, *codex_root, *decision_model, *chat_model;
    int port;
    uint64_t poll_ms;
    double rate;
    char *provider_keys[AI_PROVIDERS]; /* OpenAI uses key above; UI keys stay in memory. */
    ModelConfig decision, chat;
} Config;
typedef struct Trace {
    atomic_uint refs;
    char id[33];
    char *path;
    Doc *doc;
    uint64_t dev, ino, size;
    int64_t mtime_sec, mtime_nsec;
    size_t bytes;
    Fidelity *fidelity; /* Lazily prepared by the serialized AI worker. */
} Trace;
typedef struct {
    Trace **v;
    size_t n, cap;
} Traces;
typedef struct {
    Doc **v;
    size_t n, cap;
} Docs;
typedef struct {
    Traces traces;
    Docs analyses; /* immutable references copied at turn/quote creation */
} Snapshot;
typedef struct Task Task;
typedef struct App {
    Config cfg;
    uv_loop_t loop;
    uv_tcp_t server;
    uv_timer_t poll;
    uv_signal_t sigint, sigterm;
    int lock_fd;
    bool stopping, ingest_busy, ingest_pending, ai_busy;
    uint64_t catalog_revision;
    Traces traces, ignored_sources;
    Docs analyses, quotes, jobs, chats;
    Task *head, *tail;
    Doc *tools, *domains, *prompts;
    char *html;
    char *discovery_error;
    void *connections;
} App;
typedef void (*Reply)(void *, int, Doc *); /* owns response document */
typedef struct {
    int status;
    char *body;
    char *error;
    bool ambiguous;
} Transport;
#ifdef SYMBIOSIC_TEST
typedef Transport (*TestTransport)(const char *, const char *, const char *);
extern TestTransport test_transport;
#endif

/* Small checked allocation, JSON and string helpers. All J pointers belong to a Doc. */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t s);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *fmt(const char *format, ...);
void buf_addn(Buf *b, const char *s, size_t n);
void buf_add(Buf *b, const char *s);
void buf_free(Buf *b);
size_t utf8_end(const char *s, size_t n, size_t end);
Doc *doc_new(void);
Doc *doc_parse(const char *s, size_t n);
Doc *doc_clone(Doc *d);
J *root(Doc *d);
J *get(J *o, const char *k);
const char *str(J *v);
const char *sget(J *o, const char *k);
int64_t integer(J *v);
double number(J *v);
bool boolean(J *v);
J *obj(Doc *d);
J *arr(Doc *d);
J *js(Doc *d, const char *s);
J *ji(Doc *d, int64_t n);
J *jb(Doc *d, bool b);
J *jnum(Doc *d, double n);
J *copy(Doc *d, J *v);
void put(Doc *d, J *o, const char *k, J *v);
void add(J *a, J *v);
char *json(J *v);
void doc_free(Doc *d);
Doc *error_doc(const char *code, const char *message);
void docs_push(Docs *a, Doc *d);
void docs_free(Docs *a);
Doc *docs_find(Docs *a, const char *key, const char *id);
void docs_replace(Docs *a, const char *key, Doc *d);
char *now_iso(void);
bool timestamp(const char *s, double *seconds);
bool valid_id(const char *s);
bool new_id(char out[33]);
char *absolute_path(const char *path);
bool config_load(Config *c, char **error);
void config_free(Config *c);
char *read_file(const char *path, size_t max, size_t *len, char **error);
bool mkdir_private(const char *path, char **error);
bool atomic_write(const char *path, const char *bytes, size_t n, char **error);
bool save_doc(App *app, const char *dir, const char *id, Doc *d, char **error);
bool audit_append(App *app, const char *id, Doc *d, char **error);
bool store_open(App *a, char **error);
void store_close(App *a);
void traces_push(Traces *a, Trace *t);
Trace *trace_find(Traces *a, const char *id);
void trace_retain(Trace *t);
void trace_release(Trace *t);
void traces_free(Traces *a);
Trace *trace_load(const char *path, char **error);
bool trace_save(App *a, Trace *t, char **error);
/* Returns NULL for unrelated files; error snapshots are returned for recognized/known inputs. */
Trace *normalize(const char *path, const char *provider, Trace *old, char **error);
void discover(App *a, Traces *inputs, Traces *outputs, char **error);
void snapshot_free(Snapshot *s);
bool snapshot_take(App *a, J *scope, Snapshot *s, char **error);
Doc *trace_summary(Trace *t, Docs *analyses);
Doc *aggregate(Snapshot *s, const char *group_by, size_t offset, size_t limit);
Doc *tool_run(Snapshot *s, const char *name, J *args);
Doc *events_page(Trace *t, size_t start, size_t limit, size_t text_offset);
Doc *analysis_best(Docs *a, Trace *t);
Doc *analysis_index(Doc *analysis);

Doc *estimate_build(App *a, Snapshot *s, Doc *selection, char **error);
Doc *analyze_entry(App *a, J *quote, J *entry, Docs *cache, char **error);

Transport api_post(Config *cfg, const char *endpoint, const char *body);
Doc *chat_turn(App *a, Doc *chat, Snapshot *s, const char *text, char **error);
void transport_free(Transport *t);
const char *provider_name(Provider p);
bool provider_parse(const char *name, Provider *p);
const char *provider_key(Config *c, Provider p);
Transport provider_http(Config *c, Provider p, const char *endpoint, const char *body);
Doc *provider_request(Config *c, ModelRole decision, J *request, const char **endpoint);
Doc *provider_response(Provider p, ModelRole decision, const char *endpoint, J *response);
Doc *provider_models(Config *c, Provider p, char **error);
Doc *ai_settings(App *a);
Doc *ai_settings_update(App *a, J *in, SettingsKind connection, int *status);
bool ai_settings_load(App *a, char **error);
bool tokenizer_init(char **error);
void tokenizer_destroy(void);
size_t token_count(const char *text);
size_t token_json(J *value);
size_t model_budget(ModelConfig *model);
Fidelity *fidelity_prepare(Trace *t);
void fidelity_free(Fidelity *f);
size_t fidelity_tokens(Fidelity *f, int level);
int fidelity_select(const size_t totals[6], size_t budget);
char *fidelity_render(Trace *t, int level, Doc **coverage);
void enqueue_models(App *a, Provider provider, Reply reply, void *ctx);
bool prompts_load(App *a, char **error);
char *prompt_render(App *a, const char *name, J *vars, char **error);
Doc *domains_update(App *a, J *in, int *status);
bool model_config_parse(J *v, ModelConfig *m);
J *model_config_json(Doc *d, ModelConfig *m, const char *id);
bool directory_allowed(App *a, const char *cwd);
Doc *directory_manifest(App *a, const char *cwd, char **error);
Doc *directory_page(App *a, const char *cwd, const char *path, size_t offset, char **error);
Doc *directory_read(const char *cwd, const char *path, size_t offset, size_t limit, J *expected);
Doc *analysis_for_scope(App *a, J *scope);
Doc *review_chat_turn(App *a, Doc *chat, Snapshot *s, const char *text, char **error);
void enqueue_directory(App *a, Doc *selection, Reply reply, void *ctx);
Doc *trace_overview(Trace *t);

void api_route(App *a, const char *method, const char *url, const char *body, Reply reply,
               void *ctx);
bool http_start(App *a, char **error);
void http_stop(App *a);
void schedule_discovery(App *a);
void ai_kick(App *a);
void app_shutdown(App *a);
void enqueue_estimate(App *a, Doc *selection, Reply reply, void *ctx);
Doc *start_analysis(App *a, const char *quote_id, int *status);
Doc *create_chat(App *a, J *scope, int *status);
Doc *start_chat(App *a, const char *id, const char *text, int *status);
Doc *state_doc(App *a);
bool app_init(App *a, char **error);
void app_destroy(App *a);
#endif
