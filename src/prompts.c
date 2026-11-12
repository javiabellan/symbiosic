#include "app.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool prompt_id(const char *s) {
    if (!*s || strlen(s) > 64) {
        return false;
    }
    for (; *s; s++) {
        if (!strchr("abcdefghijklmnopqrstuvwxyz0123456789-", *s)) {
            return false;
        }
    }
    return true;
}
static const char *body_start(const char *s) {
    if (strncmp(s, "---\n", 4)) {
        return s;
    }
    const char *end = strstr(s + 4, "\n---\n");
    return end ? end + 5 : NULL;
}
static bool load_domains(App *a, const char *path, bool optional, char **error) {
    DIR *dir = opendir(path);
    if (!dir) {
        if (optional) {
            return true;
        }
        *error = fmt("Cannot open domain prompts: %s", path);
        return false;
    }
    struct dirent *de;
    while ((de = readdir(dir))) {
        size_t len = strlen(de->d_name);
        if (len < 8 || strcmp(de->d_name + len - 7, ".prompt")) {
            continue;
        }
        char *file = fmt("%s/%s", path, de->d_name);
        size_t n;
        char *text = read_file(file, MAX_SCHEMA_BYTES, &n, error);
        free(file);
        if (!text) {
            closedir(dir);
            return false;
        }
        const char *body = body_start(text), *end = strstr(text + 4, "\n---\n");
        Doc *front = end ? doc_parse(text + 4, (size_t)(end - text - 4)) : NULL;
        const char *id = sget(root(front), "name"), *name = sget(root(front), "description");
        if (!body || !front || !prompt_id(id) || !*name) {
            *error = fmt("Invalid Dotprompt domain file: %s", de->d_name);
            free(text);
            doc_free(front);
            closedir(dir);
            return false;
        }
        J *v = obj(a->domains);
        put(a->domains, v, "id", js(a->domains, id));
        put(a->domains, v, "name", js(a->domains, name));
        put(a->domains, v, "detection_prompt",
            copy(a->domains, get(get(root(front), "metadata"), "detection")));
        put(a->domains, v, "analysis_prompt", js(a->domains, body));
        put(a->domains, root(a->domains), id, v);
        free(text);
        doc_free(front);
    }
    closedir(dir);
    return true;
}
bool prompts_load(App *a, char **error) {
    a->prompts = doc_new();
    a->domains = doc_new();
    const char *names[] = {"trace", "directory", "detect", "chat", "review-chat"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
        char *path = fmt("prompts/%s.prompt", names[i]);
        size_t n;
        char *text = read_file(path, MAX_SCHEMA_BYTES, &n, error);
        free(path);
        if (!text) {
            return false;
        }
        const char *body = body_start(text);
        if (!body) {
            free(text);
            *error = xstrdup("Invalid .prompt frontmatter");
            return false;
        }
        put(a->prompts, root(a->prompts), names[i], js(a->prompts, body));
        free(text);
    }
    if (!load_domains(a, "prompts/domains", false, error)) {
        return false;
    }
    char *path = fmt("%s/prompts/domains", a->cfg.data_dir);
    bool ok = load_domains(a, path, true, error);
    free(path);
    return ok;
}
char *prompt_render(App *a, const char *name, J *vars, char **error) {
    const char *p = sget(root(a->prompts), name);
    if (!*p) {
        *error = fmt("Missing prompt: %s", name);
        return NULL;
    }
    Buf out = {0};
    while (*p) {
        const char *open = strstr(p, "{{");
        if (!open) {
            buf_add(&out, p);
            break;
        }
        buf_addn(&out, p, (size_t)(open - p));
        const char *close = strstr(open + 2, "}}");
        if (!close || close - open > 100) {
            *error = xstrdup("Invalid prompt expression");
            buf_free(&out);
            return NULL;
        }
        char key[101];
        size_t n = (size_t)(close - open - 2);
        memcpy(key, open + 2, n);
        key[n] = 0;
        if (!yyjson_mut_is_str(get(vars, key))) {
            *error = fmt("Missing prompt input: %s", key);
            buf_free(&out);
            return NULL;
        }
        buf_add(&out, sget(vars, key));
        p = close + 2;
    }
    return out.s ? out.s : xstrdup("");
}
Doc *domains_update(App *a, J *in, int *status) {
    if (a->ai_busy || a->head) {
        *status = HTTP_CONFLICT;
        return error_doc("ai_busy", "Wait for AI work to finish before editing prompts");
    }
    const char *id = sget(in, "id"), *name = sget(in, "name"),
               *detect = sget(in, "detection_prompt"), *prompt = sget(in, "analysis_prompt");
    if (!prompt_id(id) || !*name || strlen(name) > 100 || !*detect || !*prompt ||
        strlen(detect) > 2000 || strlen(prompt) > 10000) {
        *status = HTTP_BAD_REQUEST;
        return error_doc("bad_domain", "Use an identifier, a name, a detection description and an "
                                       "analysis prompt (up to 10,000 bytes)");
    }
    Doc *front = doc_new();
    put(front, root(front), "name", js(front, id));
    put(front, root(front), "description", js(front, name));
    J *metadata = obj(front);
    put(front, root(front), "metadata", metadata);
    put(front, metadata, "detection", js(front, detect));
    char *header = json(root(front)), *body = fmt("---\n%s\n---\n%s", header, prompt);
    char *dir = fmt("%s/prompts/domains", a->cfg.data_dir), *path = fmt("%s/%s.prompt", dir, id),
         *error = NULL;
    bool ok = mkdir_private(dir, &error) && atomic_write(path, body, strlen(body), &error);
    free(dir);
    free(path);
    free(header);
    free(body);
    doc_free(front);
    if (!ok) {
        Doc *d = error_doc("save_failed", error);
        free(error);
        *status = HTTP_SERVER_ERROR;
        return d;
    }
    put(a->domains, root(a->domains), id, copy(a->domains, in));
    a->catalog_revision++;
    *status = HTTP_OK;
    return doc_clone(a->domains);
}
