#include "app.h"
#include <llhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

typedef struct Conn {
    uv_tcp_t tcp;
    uv_write_t write;
    llhttp_t parser;
    App *app;
    struct Conn *next;
    Buf url, body, field, value;
    char *host, *origin, *content_type, *response;
    unsigned refs;
    size_t header_bytes, parsed_header_bytes;
    char tail[4];
    unsigned tail_n;
    bool header_done, complete, value_seen, closed, responded;
    int failure;
} Conn;
static llhttp_settings_t settings;
static void release(Conn *c) {
    if (--c->refs)
        return;
    buf_free(&c->url);
    buf_free(&c->body);
    buf_free(&c->field);
    buf_free(&c->value);
    free(c->host);
    free(c->origin);
    free(c->content_type);
    free(c->response);
    free(c);
}
static void closed(uv_handle_t *h) {
    Conn *c = h->data;
    Conn **p = (Conn **)&c->app->connections;
    while (*p && *p != c)
        p = &(*p)->next;
    if (*p)
        *p = c->next;
    release(c);
}
static void close_conn(Conn *c) {
    if (!c->closed) {
        c->closed = true;
        uv_read_stop((uv_stream_t *)&c->tcp);
        uv_close((uv_handle_t *)&c->tcp, closed);
    }
}
static void written(uv_write_t *w, int status) {
    (void)status;
    Conn *c = w->data;
    close_conn(c);
    release(c);
}
static void send_bytes(Conn *c, int status, const char *type, const char *body) {
    if (c->closed || c->responded)
        return;
    c->responded = true;
    uv_read_stop((uv_stream_t *)&c->tcp);
    const char *reason = status == 200   ? "OK"
                         : status == 202 ? "Accepted"
                         : status == 400 ? "Bad Request"
                         : status == 403 ? "Forbidden"
                         : status == 404 ? "Not Found"
                         : status == 409 ? "Conflict"
                         : status == 413 ? "Content Too Large"
                         : status == 415 ? "Unsupported Media Type"
                         : status == 502 ? "Bad Gateway"
                         : status == 503 ? "Service Unavailable"
                                         : "Internal Server Error";
    c->response =
        fmt("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: "
            "close\r\nCache-Control: no-store\r\nX-Content-Type-Options: "
            "nosniff\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src "
            "'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; "
            "frame-ancestors 'none'; base-uri 'none'; form-action 'self'\r\n\r\n%s",
            status, reason, type, strlen(body), body);
    uv_buf_t b = uv_buf_init(c->response, (unsigned int)strlen(c->response));
    c->write.data = c;
    c->refs++;
    if (uv_write(&c->write, (uv_stream_t *)&c->tcp, &b, 1, written)) {
        release(c);
        close_conn(c);
    }
}
static void reply(void *ctx, int status, Doc *d) {
    Conn *c = ctx;
    char *s = json(root(d));
    doc_free(d);
    send_bytes(c, status, "application/json; charset=utf-8", s);
    free(s);
    release(c);
}
static void reject(Conn *c, int status, const char *code, const char *message) {
    c->refs++;
    reply(c, status, error_doc(code, message));
}
static void header_commit(Conn *c) {
    if (!c->field.n)
        return;
    for (size_t i = 0; i < c->field.n; i++)
        c->field.s[i] = (char)tolower((unsigned char)c->field.s[i]);
    char **target = NULL;
    if (!strcmp(c->field.s, "host"))
        target = &c->host;
    else if (!strcmp(c->field.s, "origin"))
        target = &c->origin;
    else if (!strcmp(c->field.s, "content-type"))
        target = &c->content_type;
    if (target) {
        if (*target)
            c->failure = 400;
        else
            *target = xstrdup(c->value.s ? c->value.s : "");
    }
    buf_free(&c->field);
    buf_free(&c->value);
    c->value_seen = false;
}
static int on_url(llhttp_t *p, const char *at, size_t n) {
    Conn *c = p->data;
    if (c->url.n + n > 2048) {
        c->failure = 413;
        return HPE_USER;
    }
    buf_addn(&c->url, at, n);
    return 0;
}
static int on_field(llhttp_t *p, const char *at, size_t n) {
    Conn *c = p->data;
    c->parsed_header_bytes += n;
    if (c->parsed_header_bytes > 8192) {
        c->failure = 413;
        return HPE_USER;
    }
    if (c->value_seen)
        header_commit(c);
    buf_addn(&c->field, at, n);
    return 0;
}
static int on_value(llhttp_t *p, const char *at, size_t n) {
    Conn *c = p->data;
    c->parsed_header_bytes += n;
    if (c->parsed_header_bytes > 8192) {
        c->failure = 413;
        return HPE_USER;
    }
    c->value_seen = true;
    buf_addn(&c->value, at, n);
    return 0;
}
static int on_headers(llhttp_t *p) {
    Conn *c = p->data;
    header_commit(c);
    if (p->content_length > 16384) {
        c->failure = 413;
        return HPE_USER;
    }
    return 0;
}
static int on_body(llhttp_t *p, const char *at, size_t n) {
    Conn *c = p->data;
    if (c->body.n + n > 16384) {
        c->failure = 413;
        return HPE_USER;
    }
    buf_addn(&c->body, at, n);
    return 0;
}
static int on_complete(llhttp_t *p) {
    Conn *c = p->data;
    c->complete = true;
    return HPE_PAUSED;
}
static void alloc_read(uv_handle_t *h, size_t size, uv_buf_t *buf) {
    (void)h;
    (void)size;
    buf->base = xmalloc(16384);
    buf->len = 16384;
}
static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    Conn *c = stream->data;
    if (nread <= 0) {
        free(buf->base);
        if (nread < 0)
            close_conn(c);
        return;
    }
    for (ssize_t i = 0; i < nread && !c->header_done; i++) {
        c->header_bytes++;
        if (c->tail_n < 4)
            c->tail[c->tail_n++] = buf->base[i];
        else {
            memmove(c->tail, c->tail + 1, 3);
            c->tail[3] = buf->base[i];
        }
        if (c->tail_n == 4 && !memcmp(c->tail, "\r\n\r\n", 4))
            c->header_done = true;
        if (c->header_bytes > 8192) {
            c->failure = 413;
            break;
        }
    }
    llhttp_errno_t result =
        c->failure ? HPE_USER : llhttp_execute(&c->parser, buf->base, (size_t)nread);
    free(buf->base);
    if (c->failure || (!c->complete && result != HPE_OK)) {
        reject(c, c->failure ? c->failure : 400, c->failure == 413 ? "too_large" : "bad_http",
               c->failure == 413 ? "Headers exceed 8 KiB or API body exceeds 16 KiB"
                                 : "Malformed HTTP request");
        return;
    }
    if (!c->complete)
        return;
    char host[64], origin[80];
    snprintf(host, sizeof(host), "127.0.0.1:%d", c->app->cfg.port);
    snprintf(origin, sizeof(origin), "http://%s", host);
    if (!c->host || strcmp(c->host, host) || (c->origin && strcmp(c->origin, origin))) {
        reject(c, 403, "origin_rejected",
               "Only the exact configured local Host and Origin are accepted");
        return;
    }
    const char *method = llhttp_method_name(llhttp_get_method(&c->parser));
    if (!strcmp(method, "POST") &&
        (!c->content_type || strncmp(c->content_type, "application/json", 16) ||
         (c->content_type[16] && c->content_type[16] != ';'))) {
        reject(c, 415, "content_type", "POST requests require application/json");
        return;
    }
    if (c->app->stopping) {
        reject(c, 503, "shutting_down", "Application is stopping");
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(c->url.s, "/")) {
        send_bytes(c, 200, "text/html; charset=utf-8", c->app->html);
        return;
    }
    uv_read_stop(stream);
    c->refs++;
    api_route(c->app, method, c->url.s, c->body.s ? c->body.s : "", reply, c);
}
static void connected(uv_stream_t *server, int status) {
    if (status < 0)
        return;
    App *a = server->data;
    Conn *c = xcalloc(1, sizeof(*c));
    c->app = a;
    c->refs = 1;
    uv_tcp_init(&a->loop, &c->tcp);
    c->tcp.data = c;
    c->next = a->connections;
    a->connections = c;
    if (uv_accept(server, (uv_stream_t *)&c->tcp)) {
        close_conn(c);
        return;
    }
    llhttp_init(&c->parser, HTTP_REQUEST, &settings);
    c->parser.data = c;
    uv_read_start((uv_stream_t *)&c->tcp, alloc_read, read_cb);
}
bool http_start(App *a, char **error) {
    llhttp_settings_init(&settings);
    settings.on_url = on_url;
    settings.on_header_field = on_field;
    settings.on_header_value = on_value;
    settings.on_headers_complete = on_headers;
    settings.on_body = on_body;
    settings.on_message_complete = on_complete;
    uv_tcp_init(&a->loop, &a->server);
    a->server.data = a;
    struct sockaddr_in addr;
    int rc = uv_ip4_addr("127.0.0.1", a->cfg.port, &addr);
    if (!rc)
        rc = uv_tcp_bind(&a->server, (const struct sockaddr *)&addr, 0);
    if (!rc)
        rc = uv_listen((uv_stream_t *)&a->server, 32, connected);
    if (rc) {
        *error = fmt("Cannot listen: %s", uv_strerror(rc));
        uv_close((uv_handle_t *)&a->server, NULL);
        return false;
    }
    return true;
}
void http_stop(App *a) {
    if (!uv_is_closing((uv_handle_t *)&a->server))
        uv_close((uv_handle_t *)&a->server, NULL);
    for (Conn *c = a->connections; c; c = c->next)
        close_conn(c);
}
static int hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static char *decode(const char *s) {
    Buf b = {0};
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '%') {
            if (!s[i + 1] || !s[i + 2] || hex(s[i + 1]) < 0 || hex(s[i + 2]) < 0) {
                buf_free(&b);
                return NULL;
            }
            c = (char)(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
            if (!c) {
                buf_free(&b);
                return NULL;
            }
        } else if (c == '+')
            c = ' ';
        buf_addn(&b, &c, 1);
    }
    return b.s ? b.s : xstrdup("");
}
static Doc *query_parse(char *url) {
    Doc *d = doc_new();
    char *q = strchr(url, '?');
    if (!q)
        return d;
    *q++ = 0;
    char *save = NULL;
    for (char *v = strtok_r(q, "&", &save); v; v = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(v, '=');
        if (!eq) {
            doc_free(d);
            return NULL;
        }
        *eq++ = 0;
        char *key = decode(v), *val = decode(eq);
        if (!key || !val || get(root(d), key)) {
            free(key);
            free(val);
            doc_free(d);
            return NULL;
        }
        put(d, root(d), key, js(d, val));
        free(key);
        free(val);
    }
    return d;
}
static bool query_int(J *q, const char *key, size_t def, size_t *value) {
    const char *s = sget(q, key);
    if (!get(q, key)) {
        *value = def;
        return true;
    }
    if (!*s || *s == '-' || *s == '+')
        return false;
    for (const char *p = s; *p; p++)
        if (!isdigit((unsigned char)*p))
            return false;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || *end || v > SIZE_MAX)
        return false;
    *value = (size_t)v;
    return true;
}
static Doc *public_chat(Doc *chat) {
    Doc *d = doc_clone(chat);
    yyjson_mut_obj_remove_str(root(d), "unsaved_audit");
    return d;
}
void api_route(App *a, const char *method, const char *url, const char *body, Reply send,
               void *ctx) {
    char *path = xstrdup(url);
    Doc *query = query_parse(path), *input = NULL, *out = NULL;
    int status = 200;
    if (!query) {
        status = 400;
        out = error_doc("bad_query", "Malformed query string");
        goto done;
    }
    J *q = root(query);
    bool post = !strcmp(method, "POST"), get_method = !strcmp(method, "GET");
    if (post) {
        input = doc_parse(body, strlen(body));
        if (!input || !yyjson_mut_is_obj(root(input))) {
            status = 400;
            out = error_doc("bad_json", "Expected a JSON object");
            goto done;
        }
    }
    J *in = root(input);
    if (get_method && !strcmp(path, "/api/state"))
        out = state_doc(a);
    else if (get_method && !strcmp(path, "/api/trace")) {
        size_t start, limit, offset;
        const char *id = sget(q, "id");
        if (!valid_id(id) || !query_int(q, "start_seq", 0, &start) ||
            !query_int(q, "limit", 25, &limit) || !query_int(q, "text_offset", 0, &offset)) {
            status = 400;
            out = error_doc("bad_arguments", "Invalid trace ID or pagination");
            goto done;
        }
        Trace *t = trace_find(&a->traces, id);
        if (!t) {
            status = 404;
            out = error_doc("not_found", "Trace not found");
            goto done;
        }
        out = events_page(t, start, limit, offset);
        if (get(root(out), "error")) {
            status = 400;
            goto done;
        }
        put(out, root(out), "meta", copy(out, get(root(t->doc), "meta")));
        Doc *summary = trace_summary(t, &a->analyses);
        put(out, root(out), "analysis", copy(out, get(root(summary), "analysis")));
        doc_free(summary);
    } else if (post && !strcmp(path, "/api/estimate")) {
        J *ids = get(in, "trace_ids");
        bool all = yyjson_mut_is_true(get(in, "all"));
        if (yyjson_mut_obj_size(in) != 1 || (!all && !yyjson_mut_is_arr(ids)) ||
            (!all && (yyjson_mut_arr_size(ids) < 1 || yyjson_mut_arr_size(ids) > MAX_SOURCES))) {
            status = 400;
            out = error_doc("bad_arguments", "Send {all:true} or a nonempty trace_ids array");
            goto done;
        }
        size_t i, n;
        J *id;
        yyjson_mut_arr_foreach(ids, i, n, id) if (!valid_id(str(id))) {
            status = 400;
            out = error_doc("bad_arguments",
                            "trace_ids must contain opaque 32-digit hexadecimal IDs");
            goto done;
        }
        enqueue_estimate(a, input, send, ctx);
        input = NULL;
        doc_free(query);
        free(path);
        return;
    } else if (post && !strcmp(path, "/api/analyze")) {
        if (!valid_id(sget(in, "quote_id"))) {
            status = 400;
            out = error_doc("bad_arguments", "Invalid quote_id");
        } else
            out = start_analysis(a, sget(in, "quote_id"), &status);
    } else if (get_method && !strcmp(path, "/api/job")) {
        if (!valid_id(sget(q, "id"))) {
            status = 400;
            out = error_doc("bad_arguments", "Invalid job ID");
        } else {
            Doc *d = docs_find(&a->jobs, "id", sget(q, "id"));
            if (d) {
                out = doc_clone(d);
                J *rows = get(root(out), "results");
                size_t i, n;
                J *r;
                yyjson_mut_arr_foreach(rows, i, n, r) yyjson_mut_obj_remove_str(r, "raw_response");
            } else {
                status = 404;
                out = error_doc("not_found", "Job not found");
            }
        }
    } else if (post && !strcmp(path, "/api/chat"))
        out = create_chat(a, get(in, "scope"), &status);
    else if (get_method && !strcmp(path, "/api/chat")) {
        if (!valid_id(sget(q, "id"))) {
            status = 400;
            out = error_doc("bad_arguments", "Invalid chat ID");
        } else {
            Doc *d = docs_find(&a->chats, "id", sget(q, "id"));
            if (d)
                out = public_chat(d);
            else {
                status = 404;
                out = error_doc("not_found", "Chat not found");
            }
        }
    } else if (get_method && !strcmp(path, "/api/chats")) {
        out = doc_new();
        J *list = arr(out);
        put(out, root(out), "items", list);
        for (size_t i = 0; i < a->chats.n; i++) {
            J *r = root(a->chats.v[i]), *v = obj(out);
            const char *keys[] = {"id", "scope", "created_at", "updated_at", "latest_job"};
            for (size_t j = 0; j < 5; j++)
                put(out, v, keys[j], copy(out, get(r, keys[j])));
            add(list, v);
        }
    } else if (post && !strcmp(path, "/api/chat/message")) {
        if (!valid_id(sget(in, "chat_id")) || !yyjson_mut_is_str(get(in, "text")) ||
            !*sget(in, "text") || strlen(sget(in, "text")) > 12000) {
            status = 400;
            out = error_doc("bad_arguments",
                            "Send a valid chat_id and question of 1–12,000 UTF-8 bytes");
        } else
            out = start_chat(a, sget(in, "chat_id"), sget(in, "text"), &status);
    } else {
        status = 404;
        out = error_doc("not_found", "No such fixed route");
    }
done:
    doc_free(query);
    doc_free(input);
    free(path);
    send(ctx, status, out ? out : error_doc("internal_error", "Operation failed"));
}
