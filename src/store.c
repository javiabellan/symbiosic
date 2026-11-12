#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#if defined(__MSYS__) || defined(__CYGWIN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <sys/cygwin.h>

/* MSYS noacl mounts ignore chmod; use an inheritable owner-only NTFS ACL. */
static bool private_acl(const char *path, char **error) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        *error = fmt("Cannot read Windows identity (%lu)", GetLastError());
        return false;
    }

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    TOKEN_USER *user = xmalloc(size);
    if (!GetTokenInformation(token, TokenUser, user, size, &size)) {
        *error = fmt("Cannot read Windows identity (%lu)", GetLastError());
        CloseHandle(token);
        free(user);
        return false;
    }
    CloseHandle(token);

    EXPLICIT_ACCESSW entry = {0};
    entry.grfAccessPermissions = FILE_ALL_ACCESS;
    entry.grfAccessMode = SET_ACCESS;
    entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
    entry.Trustee.ptstrName = (LPWSTR)user->User.Sid;
    PACL acl = NULL;
    DWORD status = SetEntriesInAclW(1, &entry, NULL, &acl);
    wchar_t *native = cygwin_create_path(CCP_POSIX_TO_WIN_W, path);
    if (status == ERROR_SUCCESS && native) {
        status = SetNamedSecurityInfoW(
            native, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            NULL, NULL, acl, NULL);
    } else if (!native) {
        status = ERROR_INVALID_NAME;
    }
    free(native);
    LocalFree(acl);
    free(user);
    if (status != ERROR_SUCCESS) {
        *error = fmt("Cannot make Windows data private (%lu)", status);
        return false;
    }
    return true;
}
#endif

typedef enum { DOCS_FLAT, DOCS_NESTED } DocLayout;

static void syserror(char **e, const char *what) {
    if (e && !*e) {
        *e = fmt("%s: %s", what, strerror(errno));
    }
}
char *read_file(const char *path, size_t max, size_t *len, char **error) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        syserror(error, "Cannot open file");
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > max) {
        if (error) {
            *error = xstrdup("File is not regular or exceeds size limit");
        }
        close(fd);
        return NULL;
    }
    size_t n = (size_t)st.st_size;
    char *s = xmalloc(n + 1);
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, s + got, n - got);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            if (error) {
                *error = xstrdup("File changed while reading or read failed; deferred");
            }
            free(s);
            close(fd);
            return NULL;
        }
        got += (size_t)r;
    }
    s[n] = 0;
    close(fd);
    if (len) {
        *len = n;
    }
    return s;
}
bool mkdir_private(const char *path, char **error) {
    char *p = xstrdup(path);
    for (char *q = p + 1;; q++) {
        if (*q == '/' || !*q) {
            char c = *q;
            *q = 0;
            if (mkdir(p, 0700) && errno != EEXIST) {
                syserror(error, "Cannot create private directory");
                free(p);
                return false;
            }
            struct stat st;
            if ((c ? stat(p, &st) : lstat(p, &st)) || !S_ISDIR(st.st_mode)) {
                if (error) {
                    *error = xstrdup("Storage directory must not be a symlink");
                }
                free(p);
                return false;
            }
            *q = c;
            if (!c) {
                break;
            }
        }
    }
    free(p);
    if (chmod(path, 0700)) {
        syserror(error, "Cannot make data directory private");
        return false;
    }
#if defined(__MSYS__) || defined(__CYGWIN__)
    return private_acl(path, error);
#else
    return true;
#endif
}
bool atomic_write(const char *path, const char *bytes, size_t n, char **error) {
    char *tmp = fmt("%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) {
        syserror(error, "Cannot create temporary file");
        free(tmp);
        return false;
    }
    bool ok = fchmod(fd, 0600) == 0;
    size_t off = 0;
    while (ok && off < n) {
        ssize_t r = write(fd, bytes + off, n - off);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            ok = false;
            break;
        }
        off += (size_t)r;
    }
    if (close(fd)) {
        ok = false;
    }
    if (ok && rename(tmp, path)) {
        ok = false;
    }
    if (!ok) {
        syserror(error, "Cannot replace file atomically");
        unlink(tmp);
    }
    free(tmp);
    return ok;
}
bool save_doc(App *a, const char *dir, const char *id, Doc *d, char **error) {
    char *folder = fmt("%s/%s", a->cfg.data_dir, dir);
    if (!mkdir_private(folder, error)) {
        free(folder);
        return false;
    }
    char *path = fmt("%s/%s.json", folder, id);
    char *s = json(root(d));
    bool ok = atomic_write(path, s, strlen(s), error);
    free(folder);
    free(path);
    free(s);
    return ok;
}
bool audit_append(App *a, const char *id, Doc *d, char **error) {
    char *path = fmt("%s/chats/%s.jsonl", a->cfg.data_dir, id);
    int fd = open(path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    free(path);
    if (fd < 0) {
        syserror(error, "Cannot open chat audit");
        return false;
    }
    off_t end = lseek(fd, 0, SEEK_END), good = end;
    char c;
    while (good > 0) {
        if (pread(fd, &c, 1, good - 1) != 1) {
            close(fd);
            syserror(error, "Cannot read chat audit tail");
            return false;
        }
        if (c == '\n') {
            break;
        }
        good--;
    }
    if (good != end && ftruncate(fd, good)) {
        close(fd);
        syserror(error, "Cannot truncate incomplete audit tail");
        return false;
    }
    lseek(fd, good, SEEK_SET);
    char *s = json(root(d));
    size_t n = strlen(s);
    s = xrealloc(s, n + 2);
    s[n++] = '\n';
    size_t off = 0;
    bool ok = true;
    while (off < n) {
        ssize_t r = write(fd, s + off, n - off);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            ok = false;
            break;
        }
        off += (size_t)r;
    }
    free(s);
    if (close(fd)) {
        ok = false;
    }
    if (!ok) {
        syserror(error, "Cannot append chat audit");
    }
    return ok;
}
void traces_push(Traces *a, Trace *t) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->v = xrealloc(a->v, a->cap * sizeof(*a->v));
    }
    a->v[a->n++] = t;
}
Trace *trace_find(Traces *a, const char *id) {
    for (size_t i = 0; i < a->n; i++) {
        if (!strcmp(a->v[i]->id, id)) {
            return a->v[i];
        }
    }
    return NULL;
}
void trace_retain(Trace *t) {
    atomic_fetch_add(&t->refs, 1);
}
void trace_release(Trace *t) {
    if (t && atomic_fetch_sub(&t->refs, 1) == 1) {
        free(t->path);
        fidelity_free(t->fidelity);
        doc_free(t->doc);
        free(t);
    }
}
void traces_free(Traces *a) {
    for (size_t i = 0; i < a->n; i++) {
        trace_release(a->v[i]);
    }
    free(a->v);
    memset(a, 0, sizeof(*a));
}
Trace *trace_load(const char *path, char **error) {
    size_t n;
    char *s = read_file(path, MAX_FILE_BYTES, &n, error);
    if (!s) {
        return NULL;
    }
    Doc *d = doc_new();
    J *events = arr(d);
    put(d, root(d), "events", events);
    char *p = s, *nl;
    bool first = true;
    while ((nl = memchr(p, '\n', n - (size_t)(p - s)))) {
        Doc *row = doc_parse(p, (size_t)(nl - p));
        if (!row) {
            if (error) {
                *error = xstrdup("Invalid normalized snapshot JSONL");
            }
            goto fail;
        }
        if (first) {
            put(d, root(d), "meta", copy(d, root(row)));
            first = false;
        } else {
            add(events, copy(d, root(row)));
        }
        doc_free(row);
        p = nl + 1;
    }
    J *m = get(root(d), "meta");
    if (first || !valid_id(sget(m, "trace_id"))) {
        if (error) {
            *error = xstrdup("Invalid trace header");
        }
        goto fail;
    }
    Trace *t = xcalloc(1, sizeof(*t));
    atomic_init(&t->refs, 1);
    strcpy(t->id, sget(m, "trace_id"));
    t->path = xstrdup(sget(m, "source_path"));
    t->doc = d;
    t->bytes = n;
    J *sig = get(m, "source_signature");
    t->dev = (uint64_t)integer(get(sig, "dev"));
    t->ino = (uint64_t)integer(get(sig, "ino"));
    t->size = (uint64_t)integer(get(sig, "size"));
    t->mtime_sec = integer(get(sig, "mtime_sec"));
    t->mtime_nsec = integer(get(sig, "mtime_nsec"));
    free(s);
    return t;
fail:
    free(s);
    doc_free(d);
    return NULL;
}
bool trace_save(App *a, Trace *t, char **error) {
    Buf b = {0};
    char *s = json(get(root(t->doc), "meta"));
    buf_add(&b, s);
    buf_add(&b, "\n");
    free(s);
    J *events = get(root(t->doc), "events");
    size_t i, n;
    J *ev;
    yyjson_mut_arr_foreach(events, i, n, ev) {
        s = json(ev);
        buf_add(&b, s);
        buf_add(&b, "\n");
        free(s);
    }
    char *p = fmt("%s/traces/%s.jsonl", a->cfg.data_dir, t->id);
    bool ok = atomic_write(p, b.s, b.n, error);
    t->bytes = b.n;
    free(p);
    buf_free(&b);
    return ok;
}
static bool suffix(const char *s, const char *end) {
    size_t n = strlen(s), m = strlen(end);
    return n >= m && !strcmp(s + n - m, end);
}
static void load_docs(App *a, const char *dir, Docs *out, DocLayout nested) {
    char *path = fmt("%s/%s", a->cfg.data_dir, dir);
    DIR *d = opendir(path);
    if (!d) {
        free(path);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char *p = fmt("%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(p, &st) || S_ISLNK(st.st_mode)) {
            free(p);
            continue;
        }
        if (nested && S_ISDIR(st.st_mode)) {
            char *sub = fmt("%s/%s", dir, e->d_name);
            load_docs(a, sub, out, DOCS_FLAT);
            free(sub);
        } else if (S_ISREG(st.st_mode) && suffix(e->d_name, ".json")) {
            char *err = NULL;
            size_t n;
            char *s = read_file(p, MAX_FILE_BYTES, &n, &err);
            Doc *j = s ? doc_parse(s, n) : NULL;
            if (j) {
                if (!strncmp(dir, "analysis", 8)) {
                    Doc *index = analysis_index(j);
                    doc_free(j);
                    j = index;
                }
                docs_push(out, j);
            } else {
                fprintf(stderr, "symbiosic: cannot load %s: %s\n", p, err ? err : "invalid JSON");
            }
            free(s);
            free(err);
        }
        free(p);
    }
    closedir(d);
    free(path);
}
bool store_open(App *a, char **error) {
    a->lock_fd = -1;
    if (!mkdir_private(a->cfg.data_dir, error)) {
        return false;
    }
    const char *dirs[] = {"traces", "analysis", "quotes", "jobs", "chats"};
    for (size_t i = 0; i < 5; i++) {
        char *p = fmt("%s/%s", a->cfg.data_dir, dirs[i]);
        bool ok = mkdir_private(p, error);
        free(p);
        if (!ok) {
            return false;
        }
    }
    char *p = fmt("%s/instance.lock", a->cfg.data_dir);
    a->lock_fd = open(p, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    free(p);
    struct flock l = {0};
    l.l_type = F_WRLCK;
    l.l_whence = SEEK_SET;
    if (a->lock_fd < 0 || fcntl(a->lock_fd, F_SETLK, &l) < 0) {
        *error = xstrdup("Cannot acquire instance.lock; another symbiosic may use this DATA_DIR");
        return false;
    }
    p = fmt("%s/traces", a->cfg.data_dir);
    DIR *dir = opendir(p);
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        if (!suffix(e->d_name, ".jsonl")) {
            continue;
        }
        char *f = fmt("%s/%s", p, e->d_name);
        char *err = NULL;
        Trace *t = trace_load(f, &err);
        if (t) {
            traces_push(&a->traces, t);
        } else {
            fprintf(stderr, "symbiosic: snapshot ignored: %s (%s)\n", f, err ? err : "invalid");
        }
        free(err);
        free(f);
    }
    if (dir) {
        closedir(dir);
    }
    free(p);
    load_docs(a, "analysis", &a->analyses, DOCS_NESTED);
    load_docs(a, "quotes", &a->quotes, DOCS_FLAT);
    load_docs(a, "jobs", &a->jobs, DOCS_FLAT);
    load_docs(a, "chats", &a->chats, DOCS_FLAT);
    for (size_t i = 0; i < a->jobs.n; i++) {
        Doc *j = a->jobs.v[i];
        const char *state = sget(root(j), "state");
        if (!strcmp(state, "queued") || !strcmp(state, "running")) {
            put(j, root(j), "state", js(j, "interrupted"));
            put(j, root(j), "error",
                js(j, "Process stopped; outcome of any in-flight paid request is unknown. Not "
                      "replayed."));
            char *err = NULL;
            if (!save_doc(a, "jobs", sget(root(j), "id"), j, &err)) {
                fprintf(stderr, "symbiosic: %s\n", err);
                free(err);
            }
        }
    }
    a->catalog_revision = 1;
    return true;
}
void store_close(App *a) {
    traces_free(&a->traces);
    traces_free(&a->ignored_sources);
    docs_free(&a->analyses);
    docs_free(&a->quotes);
    docs_free(&a->jobs);
    docs_free(&a->chats);
    if (a->lock_fd >= 0) {
        close(a->lock_fd);
    }
    a->lock_fd = -1;
}
static Trace *by_path(Traces *a, const char *path) {
    for (size_t i = 0; i < a->n; i++) {
        if (!strcmp(a->v[i]->path, path)) {
            return a->v[i];
        }
    }
    return NULL;
}
static bool same_stat(Trace *t, struct stat *st) {
#ifdef __APPLE__
    struct timespec mt = st->st_mtimespec;
#else
    struct timespec mt = st->st_mtim;
#endif
    return t && t->dev == (uint64_t)st->st_dev && t->ino == (uint64_t)st->st_ino &&
           t->size == (uint64_t)st->st_size && t->mtime_sec == mt.tv_sec &&
           t->mtime_nsec == mt.tv_nsec &&
           !boolean(get(get(root(t->doc), "meta"), "source_missing"));
}
typedef struct {
    App *a;
    Traces *in, *out;
    Traces ignored;
    size_t count;
    char **error;
} Scan;
static Trace *trace_clone(Trace *old) {
    Trace *t = xcalloc(1, sizeof(*t));
    atomic_init(&t->refs, 1);
    strcpy(t->id, old->id);
    t->path = xstrdup(old->path);
    t->doc = doc_clone(old->doc);
    t->dev = old->dev;
    t->ino = old->ino;
    t->size = old->size;
    t->mtime_sec = old->mtime_sec;
    t->mtime_nsec = old->mtime_nsec;
    t->bytes = old->bytes;
    return t;
}
static Trace *ignored_source(const char *path, struct stat *st) {
    Trace *t = xcalloc(1, sizeof(*t));
    atomic_init(&t->refs, 1);
    t->path = xstrdup(path);
    t->doc = doc_new();
    t->dev = (uint64_t)st->st_dev;
    t->ino = (uint64_t)st->st_ino;
    t->size = (uint64_t)st->st_size;
#ifdef __APPLE__
    t->mtime_sec = st->st_mtimespec.tv_sec;
    t->mtime_nsec = st->st_mtimespec.tv_nsec;
#else
    t->mtime_sec = st->st_mtim.tv_sec;
    t->mtime_nsec = st->st_mtim.tv_nsec;
#endif
    return t;
}
static void scan_dir(Scan *s, const char *path, const char *provider, unsigned depth) {
    if (depth > 128) {
        if (!*s->error) {
            *s->error = xstrdup("Discovery directory depth exceeds 128");
        }
        return;
    }
    struct stat st;
    if (lstat(path, &st) || !S_ISDIR(st.st_mode)) {
        return;
    }
    DIR *d = opendir(path);
    if (!d) {
        if (!*s->error) {
            *s->error = fmt("Cannot scan source directory: %s", strerror(errno));
        }
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        char *p = fmt("%s/%s", path, e->d_name);
        if (lstat(p, &st)) {
            free(p);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            scan_dir(s, p, provider, depth + 1);
        } else if (S_ISREG(st.st_mode) && suffix(p, ".jsonl")) {
            if (by_path(s->out, p) || by_path(&s->ignored, p)) {
                free(p);
                continue;
            }
            if (++s->count > MAX_SOURCES) {
                if (!*s->error) {
                    *s->error = xstrdup("Discovery exceeds 1,000 source files; excess files "
                                        "deferred. Narrow configured roots.");
                }
                free(p);
                continue;
            }
            Trace *old = by_path(s->in, p), *ignored = by_path(&s->a->ignored_sources, p);
            if (!old && same_stat(ignored, &st)) {
                trace_retain(ignored);
                traces_push(&s->ignored, ignored);
            } else if (same_stat(old, &st) && integer(get(get(root(old->doc), "meta"),
                                                          "normalizer_version")) == TV_VERSION) {
                trace_retain(old);
                traces_push(s->out, old);
            } else {
                char *err = NULL;
                Trace *t = normalize(p, provider, old, &err);
                if (t) {
                    if (t == old || trace_save(s->a, t, &err)) {
                        traces_push(s->out, t);
                    } else {
                        trace_release(t);
                        t = NULL;
                    }
                }
                if (!t && old) {
                    trace_retain(old);
                    traces_push(s->out, old);
                } else if (!t && !err) {
                    traces_push(&s->ignored, ignored_source(p, &st));
                }
                if (err) {
                    if (!*s->error) {
                        *s->error = fmt("%s: %s", p, err);
                    }
                    free(err);
                }
            }
        }
        free(p);
    }
    closedir(d);
}
static int trace_cmp(const void *a, const void *b) {
    return strcmp((*(Trace *const *)a)->id, (*(Trace *const *)b)->id);
}

enum { TITLE_INDEX_BYTES = 8u * 1024 * 1024 };
typedef enum { INDEX_JSON, INDEX_JSONL } IndexFormat;

static void collect_title(Doc *titles, J *row) {
    const char *id = *sget(row, "id") ? sget(row, "id") : sget(row, "sessionId");
    const char *keys[] = {"customTitle", "thread_name", "title", "summary"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
        const char *title = sget(row, keys[i]);
        if (!*id || !*title) {
            continue;
        }

        put(titles, root(titles), id, js(titles, title));
        return;
    }
}

static Doc *read_title_index(const char *path, IndexFormat format) {
    Doc *titles = doc_new();
    char *error = NULL;
    size_t size = 0;
    char *text = read_file(path, TITLE_INDEX_BYTES, &size, &error);
    free(error);
    if (!text) {
        return titles;
    }

    /* Sidecar indexes may end with a complete JSON value without a newline. */
    if (format == INDEX_JSONL) {
        for (char *line = text; *line;) {
            char *end = strchr(line, '\n');
            Doc *row = doc_parse(line, end ? (size_t)(end - line) : strlen(line));
            collect_title(titles, root(row));
            doc_free(row);
            if (!end) {
                break;
            }
            line = end + 1;
        }
    } else {
        Doc *index = doc_parse(text, size);
        size_t i, n;
        J *row;
        yyjson_mut_arr_foreach(get(root(index), "entries"), i, n, row) {
            collect_title(titles, row);
        }
        doc_free(index);
    }

    free(text);
    return titles;
}

static char *parent_dir(const char *path) {
    char *parent = xstrdup(path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        slash[slash == parent ? 1 : 0] = 0;
    }
    return parent;
}

static void apply_titles(App *a, Traces *traces) {
    Doc *indexes = doc_new();
    for (size_t i = 0; i < traces->n; i++) {
        Trace *t = traces->v[i];
        J *meta = get(root(t->doc), "meta");
        if (boolean(get(meta, "subagent")) || !strcmp(sget(meta, "title_kind"), TITLE_CUSTOM)) {
            continue;
        }

        bool codex = !strcmp(sget(meta, "provider"), "codex");
        char *parent = parent_dir(codex ? a->cfg.codex_root : t->path);
        char *path = fmt("%s/%s", parent, codex ? "session_index.jsonl" : "sessions-index.json");
        free(parent);
        J *index = get(root(indexes), path);
        if (!index) {
            Doc *titles = read_title_index(path, codex ? INDEX_JSONL : INDEX_JSON);
            index = copy(indexes, root(titles));
            put(indexes, root(indexes), path, index);
            doc_free(titles);
        }
        free(path);

        const char *title = sget(index, sget(meta, "native_session_id"));
        const char *original =
            get(meta, "source_title") ? sget(meta, "source_title") : sget(meta, "title");
        if (!*title) {
            title = original;
        }
        if (!strcmp(title, sget(meta, "title"))) {
            continue;
        }

        /* Index edits change presentation only; keep evidence revisions and readers stable. */
        Trace *replacement = trace_clone(t);
        J *next = get(root(replacement->doc), "meta");
        put(replacement->doc, next, "source_title", js(replacement->doc, original));
        put(replacement->doc, next, "title", js(replacement->doc, title));
        char *error = NULL;
        if (trace_save(a, replacement, &error)) {
            traces->v[i] = replacement;
            trace_release(t);
        } else {
            trace_release(replacement);
        }
        free(error);
    }
    doc_free(indexes);
}
void discover(App *a, Traces *in, Traces *out, char **error) {
    Scan s = {.a = a, .in = in, .out = out, .error = error};
    scan_dir(&s, a->cfg.claude_root, "claude", 0);
    scan_dir(&s, a->cfg.codex_root, "codex", 0);
    for (size_t i = 0; i < in->n; i++) {
        Trace *old = in->v[i];
        if (by_path(out, old->path)) {
            continue;
        }
        struct stat st;
        int stat_result = lstat(old->path, &st);
        bool missing = (stat_result && errno == ENOENT) || (!stat_result && !S_ISREG(st.st_mode));
        if (!missing || boolean(get(get(root(old->doc), "meta"), "source_missing"))) {
            trace_retain(old);
            traces_push(out, old);
            continue;
        }
        Trace *t = xcalloc(1, sizeof(*t));
        atomic_init(&t->refs, 1);
        strcpy(t->id, old->id);
        t->path = xstrdup(old->path);
        t->doc = doc_clone(old->doc);
        t->dev = old->dev;
        t->ino = old->ino;
        t->size = old->size;
        t->mtime_sec = old->mtime_sec;
        t->mtime_nsec = old->mtime_nsec;
        t->bytes = old->bytes;
        put(t->doc, get(root(t->doc), "meta"), "source_missing", jb(t->doc, true));
        char *err = NULL;
        if (trace_save(a, t, &err)) {
            traces_push(out, t);
        } else {
            trace_release(t);
            trace_retain(old);
            traces_push(out, old);
            if (!*error) {
                *error = err;
            } else {
                free(err);
            }
        }
    }
    /* Resolve only explicit native parent IDs; never infer relationships from timing or paths. */
    for (size_t i = 0; i < out->n; i++) {
        Trace *t = out->v[i];
        J *m = get(root(t->doc), "meta");
        const char *native = sget(m, "parent_native_session_id"), *parent = "";
        if (*native) {
            for (size_t k = 0; k < out->n; k++) {
                J *pm = get(root(out->v[k]->doc), "meta");
                if (k != i && !boolean(get(pm, "subagent")) &&
                    !strcmp(sget(pm, "provider"), sget(m, "provider")) &&
                    !strcmp(sget(pm, "native_session_id"), native)) {
                    parent = out->v[k]->id;
                    break;
                }
            }
        }
        if (strcmp(sget(m, "parent_trace_id"), parent)) {
            Trace *replacement = trace_clone(t);
            put(replacement->doc, get(root(replacement->doc), "meta"), "parent_trace_id",
                js(replacement->doc, *parent ? parent : NULL));
            char *err = NULL;
            if (trace_save(a, replacement, &err)) {
                trace_release(t);
                out->v[i] = replacement;
            } else {
                trace_release(replacement);
                if (!*error) {
                    *error = err;
                } else {
                    free(err);
                }
            }
        }
    }
    apply_titles(a, out);
    qsort(out->v, out->n, sizeof(*out->v), trace_cmp);
    traces_free(&a->ignored_sources);
    a->ignored_sources = s.ignored;
}
void snapshot_free(Snapshot *s) {
    traces_free(&s->traces);
    docs_free(&s->analyses);
}
bool snapshot_take(App *a, J *scope, Snapshot *s, char **error) {
    memset(s, 0, sizeof(*s));
    const char *type = sget(scope, "type");
    (void)error; /* Snapshot references share immutable trace storage. */
    for (size_t i = 0; i < a->traces.n; i++) {
        Trace *t = a->traces.v[i];
        J *m = get(root(t->doc), "meta");
        bool in = !strcmp(type, "all") ||
                  (!strcmp(type, "trace") && !strcmp(t->id, sget(scope, "id"))) ||
                  (!strcmp(type, "directory") && !strcmp(sget(m, "cwd"), sget(scope, "cwd")));
        if (!in) {
            continue;
        }
        trace_retain(t);
        traces_push(&s->traces, t);
    }
    qsort(s->traces.v, s->traces.n, sizeof(*s->traces.v), trace_cmp);
    for (size_t i = 0; i < s->traces.n; i++) {
        Doc *d = analysis_best(&a->analyses, s->traces.v[i]);
        if (!d) {
            continue;
        }
        Doc *index = analysis_index(d);
        docs_push(&s->analyses, index);
    }
    return true;
}
