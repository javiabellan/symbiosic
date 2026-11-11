#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>

static const char *names[] = {"openai", "anthropic", "openrouter", "gemini"};
const char *provider_name(Provider p) {
    return p < AI_PROVIDERS ? names[p] : "unknown";
}
bool provider_parse(const char *name, Provider *p) {
    for (int i = 0; i < AI_PROVIDERS; i++)
        if (!strcmp(name, names[i])) {
            *p = (Provider)i;
            return true;
        }
    return false;
}
const char *provider_key(Config *c, Provider p) {
    const char *key = p == AI_OPENAI ? c->key : c->provider_keys[p];
    return key ? key : "";
}
static J *message(Doc *d, J *messages, const char *role, const char *text) {
    J *m = obj(d);
    put(d, m, "role", js(d, role));
    put(d, m, "content", js(d, text));
    add(messages, m);
    return m;
}
static char *content_text(J *content) {
    if (yyjson_mut_is_str(content))
        return xstrdup(str(content));
    Buf b = {0};
    size_t i, n;
    J *part;
    yyjson_mut_arr_foreach(content, i, n, part) {
        const char *text = sget(part, "text");
        if (!*text)
            text = sget(part, "refusal");
        if (*text) {
            if (b.n)
                buf_add(&b, "\n");
            buf_add(&b, text);
        }
    }
    return b.s ? b.s : xstrdup("");
}
Doc *provider_request(Config *c, bool decision, J *request, const char **endpoint) {
    ModelConfig *model = decision ? &c->decision : &c->chat;
    const char *id = sget(request, "model");
    if (model->provider == AI_OPENAI) {
        Doc *d = doc_new();
        yyjson_mut_doc_set_root(d, copy(d, request));
        *endpoint = decision ? "decisions" : "responses";
        if (!decision && (strcmp(sget(get(request,"reasoning"),"effort"),"default")==0 || (strncmp(id,"gpt-6",5) && strncmp(id,"gpt-5",5) && strncmp(id,"o1",2) && strncmp(id,"o3",2) && strncmp(id,"o4",2))))
            yyjson_mut_obj_remove_str(root(d), "reasoning");
        return d;
    }
    Doc *d = doc_new();
    J *r = root(d), *messages = arr(d), *tools = arr(d);
    put(d, r, "model", js(d, id));
    put(d, r, "messages", messages);
    put(d, r, "max_tokens", ji(d, (int64_t)model->output_limit));
    bool anthropic = model->provider == AI_ANTHROPIC;
    *endpoint = anthropic ? "messages" : "chat/completions";
    Buf system = {0};
    buf_add(&system, sget(request, "instructions"));
    if (anthropic)
        put(d, r, "system", js(d, system.s));
    else
        message(d, messages, "system", system.s);
    buf_free(&system);
    if (decision)
        message(d, messages, "user", sget(request, "input"));
    else {
        size_t i, n;
        J *item;
        yyjson_mut_arr_foreach(get(request, "input"), i, n, item) {
            const char *type = sget(item, "type");
            if (boolean(get(item,"_provider_native"))) {
                if(get(item,"_provider_message")) add(messages,copy(d,get(item,"_provider_message")));
                continue;
            }
            if (!strcmp(type, "function_call")) {
                J *m = yyjson_mut_arr_get_last(messages);
                if (anthropic || strcmp(sget(m, "role"), "assistant"))
                    m = message(d, messages, "assistant", "");
                J *calls = anthropic ? arr(d) : get(m, "tool_calls"), *call = obj(d);
                if (!calls)
                    calls = arr(d);
                if (anthropic) {
                    Doc *args = doc_parse(sget(item, "arguments"), strlen(sget(item, "arguments")));
                    put(d, call, "type", js(d, "tool_use"));
                    put(d, call, "id", copy(d, get(item, "call_id")));
                    put(d, call, "name", copy(d, get(item, "name")));
                    put(d, call, "input", copy(d, root(args)));
                    doc_free(args);
                    add(calls, call);
                    put(d, m, "content", calls);
                } else {
                    J *fn = obj(d);
                    put(d, call, "id", copy(d, get(item, "call_id")));
                    put(d, call, "type", js(d, "function"));
                    put(d, fn, "name", copy(d, get(item, "name")));
                    put(d, fn, "arguments", copy(d, get(item, "arguments")));
                    put(d, call, "function", fn);
                    add(calls, call);
                    put(d, m, "tool_calls", calls);
                }
            } else if (!strcmp(type, "function_call_output")) {
                J *m = message(d, messages, anthropic ? "user" : "tool", sget(item, "output"));
                if (anthropic) {
                    J *parts = arr(d), *part = obj(d);
                    put(d, part, "type", js(d, "tool_result"));
                    put(d, part, "tool_use_id", copy(d, get(item, "call_id")));
                    put(d, part, "content", copy(d, get(item, "output")));
                    add(parts, part);
                    put(d, m, "content", parts);
                } else
                    put(d, m, "tool_call_id", copy(d, get(item, "call_id")));
            } else if (*sget(item, "role")) {
                char *text = content_text(get(item, "content"));
                J *last = yyjson_mut_arr_get_last(messages);
                if (!anthropic && !strcmp(sget(item, "role"), "assistant") &&
                    get(last, "tool_calls")) {
                    char *merged = fmt("%s%s", sget(last, "content"), text);
                    put(d, last, "content", js(d, merged));
                    free(merged);
                } else
                    message(d, messages, sget(item, "role"), text);
                free(text);
            }
        }
        yyjson_mut_arr_foreach(get(request, "tools"), i, n, item) {
            J *tool = obj(d);
            if (anthropic) {
                put(d, tool, "name", copy(d, get(item, "name")));
                put(d, tool, "description", copy(d, get(item, "description")));
                put(d, tool, "input_schema", copy(d, get(item, "parameters")));
                if(get(item,"strict"))put(d,tool,"strict",copy(d,get(item,"strict")));
            } else {
                J *fn = copy(d, item);
                yyjson_mut_obj_remove_str(fn, "type");
                put(d, tool, "type", js(d, "function"));
                put(d, tool, "function", fn);
            }
            add(tools, tool);
        }
        if (yyjson_mut_arr_size(tools))
            put(d, r, "tools", tools);
    }
    J *format = get(get(request, "text"), "format");
    if (format && !decision) {
        J *f = copy(d, format);
        yyjson_mut_obj_remove_str(f, "type");
        if (anthropic) {
            J *config = obj(d), *af = obj(d);
            put(d, af, "type", js(d, "json_schema")); put(d, af, "schema", copy(d, get(format, "schema")));
            put(d, config, "format", af); put(d, r, "output_config", config);
        } else {
            J *rf = obj(d); put(d, rf, "type", js(d, "json_schema")); put(d, rf, "json_schema", f); put(d, r, "response_format", rf);
        }
    }
    const char *effort = sget(get(request,"reasoning"),"effort");
    if (*effort && strcmp(effort,"default")) {
        if (anthropic) {
            J *thinking = obj(d);
            if(!strcmp(effort,"none")) put(d,thinking,"type",js(d,"disabled"));
            else if(strstr(id,"-4-5") || strstr(id,"-4-0") || strstr(id,"-3-7") || strstr(id,"-4-1")) {
                put(d,thinking,"type",js(d,"enabled"));
                int64_t budget=!strcmp(effort,"low")?1024:!strcmp(effort,"medium")?2048:!strcmp(effort,"high")?4096:8192;
                if(budget >= (int64_t)model->output_limit) budget=(int64_t)model->output_limit-1;
                put(d,thinking,"budget_tokens",ji(d,budget));
            } else {
                put(d,thinking,"type",js(d,"adaptive"));
                J *config=get(r,"output_config");if(!config){config=obj(d);put(d,r,"output_config",config);}
                put(d,config,"effort",js(d,effort));
            }
            put(d,r,"thinking",thinking);
        } else put(d,r,"reasoning_effort",js(d,effort));
    }
    /* Compatible endpoints use their own output-token parameter. */
    if (model->provider == AI_OPENAI) {
        yyjson_mut_obj_remove_str(r, "max_tokens");
        put(d, r, "max_completion_tokens", ji(d, (int64_t)model->output_limit));
    }
    return d;
}
Doc *provider_response(Provider p, bool decision, const char *endpoint, J *response) {
    if (!response)
        return NULL;
    if (!strcmp(endpoint, "decisions") || !strcmp(endpoint, "responses")) {
        Doc *d = doc_new();
        yyjson_mut_doc_set_root(d, copy(d, response));
        return d;
    }
    if (p == AI_ANTHROPIC ? !yyjson_mut_is_arr(get(response, "content"))
                          : !yyjson_mut_arr_size(get(response, "choices")))
        return NULL;
    Doc *d = doc_new();
    J *out = arr(d), *usage = obj(d);
    Buf text = {0};
    put(d, root(d), "output", out);
    put(d, root(d), "usage", usage);
    J *u = get(response, "usage");
    put(d, usage, "input_tokens",
        copy(d, get(u, p == AI_ANTHROPIC ? "input_tokens" : "prompt_tokens")));
    put(d, usage, "output_tokens",
        copy(d, get(u, p == AI_ANTHROPIC ? "output_tokens" : "completion_tokens")));
    put(d, usage, "input_tokens_details", copy(d, get(u,"prompt_tokens_details")));
    put(d, usage, "cache_read_input_tokens", copy(d, get(u,"cache_read_input_tokens")));
    put(d, usage, "cache_creation_input_tokens", copy(d, get(u,"cache_creation_input_tokens")));
    size_t i, n;
    J *v;
    if (p == AI_ANTHROPIC) {
        yyjson_mut_arr_foreach(get(response, "content"), i, n, v) {
            if (!strcmp(sget(v, "type"), "text"))
                buf_add(&text, sget(v, "text"));
            if (!strcmp(sget(v, "type"), "tool_use")) {
                J *call = obj(d);
                put(d, call, "type", js(d, "function_call"));
                put(d, call, "call_id", copy(d, get(v, "id")));
                put(d, call, "name", copy(d, get(v, "name")));
                char *args = json(get(v, "input"));
                put(d, call, "arguments", js(d, args));
                free(args);
                add(out, call);
            }
        }
    } else {
        J *m = get(yyjson_mut_arr_get(get(response, "choices"), 0), "message");
        buf_add(&text, sget(m, "content"));
        yyjson_mut_arr_foreach(get(m, "tool_calls"), i, n, v) {
            J *call = obj(d), *fn = get(v, "function");
            put(d, call, "type", js(d, "function_call"));
            put(d, call, "call_id", copy(d, get(v, "id")));
            put(d, call, "name", copy(d, get(fn, "name")));
            put(d, call, "arguments", copy(d, get(fn, "arguments")));
            add(out, call);
        }
    }
    if (decision) {
        const char *start = text.s ? text.s : "";
        if (!strncmp(start, "```json", 7))
            start += 7;
        else if (!strncmp(start, "```", 3))
            start += 3;
        size_t len = strlen(start);
        while (len && (start[len - 1] == '\n' || start[len - 1] == ' '))
            len--;
        if (len >= 3 && !memcmp(start + len - 3, "```", 3))
            len -= 3;
        Doc *parsed = doc_parse(start, len);
        if (parsed)
            put(d, root(d), "answers", copy(d, get(root(parsed), "answers")));
        doc_free(parsed);
    } else if (text.n) {
        J *m = obj(d), *parts = arr(d), *part = obj(d);
        put(d, m, "type", js(d, "message"));
        put(d, m, "role", js(d, "assistant"));
        put(d, part, "type", js(d, "output_text"));
        put(d, part, "text", js(d, text.s));
        add(parts, part);
        put(d, m, "content", parts);
        add(out, m);
    }
    if(!decision && yyjson_mut_arr_size(out)) {
        J *native=obj(d);
        if(p==AI_ANTHROPIC){put(d,native,"role",js(d,"assistant"));put(d,native,"content",copy(d,get(response,"content")));}
        else native=copy(d,get(yyjson_mut_arr_get(get(response,"choices"),0),"message"));
        yyjson_mut_arr_foreach(out,i,n,v) put(d,v,"_provider_native",jb(d,true));
        put(d,yyjson_mut_arr_get(out,0),"_provider_message",native);
    }
    buf_free(&text);
    return d;
}
Doc *provider_models(Config *c, Provider p, char **error) {
    Doc *result = doc_new();
    J *models = arr(result);
    put(result, root(result), "models", models);
    put(result, root(result), "provider", js(result, provider_name(p)));
    char *endpoint = xstrdup(p == AI_ANTHROPIC ? "models?limit=1000" : "models");
    do {
        Transport t = provider_http(c, p, endpoint, NULL);
        free(endpoint);
        endpoint = NULL;
        Doc *d = t.body ? doc_parse(t.body, strlen(t.body)) : NULL;
        if (t.error || !yyjson_mut_is_arr(get(root(d), "data"))) {
            *error = xstrdup(t.error ? t.error : "Provider returned an invalid model list");
            doc_free(d);
            transport_free(&t);
            doc_free(result);
            return NULL;
        }
        size_t i, n;
        J *m;
        yyjson_mut_arr_foreach(get(root(d), "data"), i, n, m) {
            if (!*sget(m, "id") || !strcmp(sget(m, "lifecycle"), "retired"))
                continue;
            J *v = obj(result);
            put(result, v, "id", copy(result, get(m, "id")));
            int64_t limit = integer(get(m, "context_length"));
            int64_t routed = integer(get(get(m, "top_provider"), "context_length"));
            if (routed > 0 && (limit <= 0 || routed < limit))
                limit = routed;
            if (!limit)
                limit = integer(get(m, "max_input_tokens"));
            const char *source = "provider";
            if (!limit && p == AI_OPENAI && !strcmp(sget(m, "id"), "gpt-6-luna")) {
                limit = 1050000;
                source = "OpenAI model documentation (2026-11-10)";
            }
            put(result, v, "context_limit", limit > 0 ? ji(result, limit) : js(result, NULL));
            put(result, v, "input_limit", limit > 0 ? ji(result, limit) : js(result, NULL));
            put(result, v, "limit_source",
                js(result, limit > 0 ? source : "Enter the provider's documented limits"));
            add(models, v);
        }
        if (p == AI_ANTHROPIC && boolean(get(root(d), "has_more"))) {
            const char *last = sget(root(d), "last_id");
            /* IDs are URL data, never paths or arbitrary hosts. */
            bool safe = *last;
            for (const char *q = last; *q; q++)
                if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.",
                            *q))
                    safe = false;
            if (!safe || !n || yyjson_mut_arr_size(models) > 10000) {
                *error = xstrdup("Invalid model-list pagination");
                doc_free(d);
                transport_free(&t);
                doc_free(result);
                return NULL;
            }
            endpoint = fmt("models?limit=1000&after_id=%s", last);
        }
        doc_free(d);
        transport_free(&t);
    } while (endpoint);
    return result;
}
J *model_config_json(Doc *d, ModelConfig *m, const char *id) {
    J *v = obj(d);
    put(d, v, "provider", js(d, provider_name(m->provider)));
    put(d, v, "model", js(d, id));
    put(d, v, "input_limit", ji(d, (int64_t)m->input_limit));
    put(d, v, "context_limit", ji(d, (int64_t)m->context_limit));
    put(d, v, "output_limit", ji(d, (int64_t)m->output_limit));
    put(d, v, "thinking", js(d, *m->thinking ? m->thinking : "none"));
    put(d, v, "input_rate", jnum(d, m->input_rate));
    put(d, v, "output_rate", jnum(d, m->output_rate));
    put(d, v, "pricing_known", jb(d, m->pricing_known));
    return v;
}
Doc *ai_settings(App *a) {
    Doc *d = doc_new();
    J *r = root(d), *providers = arr(d);
    put(d, r, "decision", model_config_json(d, &a->cfg.decision, a->cfg.decision_model));
    put(d, r, "chat", model_config_json(d, &a->cfg.chat, a->cfg.chat_model));
    put(d, r, "decision_input_usd_per_m", jnum(d, a->cfg.rate));
    put(d, r, "providers", providers);
    for (int p = 0; p < AI_PROVIDERS; p++) {
        J *v = obj(d);
        put(d, v, "id", js(d, names[p]));
        put(d, v, "key_available", jb(d, *provider_key(&a->cfg, (Provider)p) != 0));
        add(providers, v);
    }
    return d;
}
bool model_config_parse(J *v, ModelConfig *m) {
    memset(m, 0, sizeof(*m));
    const char *thinking = *sget(v, "thinking") ? sget(v, "thinking") : "none";
    if (strcmp(thinking,"default") && strcmp(thinking,"none") && strcmp(thinking,"low") && strcmp(thinking,"medium") && strcmp(thinking,"high") && strcmp(thinking,"xhigh")) return false;
    snprintf(m->thinking, sizeof(m->thinking), "%s", thinking);
    m->input_rate = number(get(v,"input_rate")); m->output_rate = number(get(v,"output_rate"));
    m->pricing_known = boolean(get(v,"pricing_known"));
    if (!isfinite(m->input_rate) || !isfinite(m->output_rate) || m->input_rate < 0 || m->output_rate < 0) return false;
    if (!provider_parse(sget(v, "provider"), &m->provider) || !*sget(v, "model") ||
        strlen(sget(v, "model")) > 200)
        return false;
    const char *fields[] = {"input_limit", "context_limit", "output_limit"};
    size_t *dest[] = {&m->input_limit, &m->context_limit, &m->output_limit};
    for (int i = 0; i < 3; i++) {
        int64_t n = integer(get(v, fields[i]));
        if (!yyjson_mut_is_int(get(v, fields[i])) || n < 1 || n > 100000000)
            return false;
        *dest[i] = (size_t)n;
    }
    return m->input_limit > 1024 && m->context_limit > m->output_limit + 1024;
}
static bool apply_settings(Config *c, J *in) {
    ModelConfig decision, chat;
    double rate = number(get(in, "decision_input_usd_per_m"));
    if (!model_config_parse(get(in, "decision"), &decision) || !model_config_parse(get(in, "chat"), &chat) ||
        !yyjson_mut_is_num(get(in, "decision_input_usd_per_m")) || !isfinite(rate) || rate < 0)
        return false;
    char *dm = xstrdup(sget(get(in, "decision"), "model")),
         *cm = xstrdup(sget(get(in, "chat"), "model"));
    free(c->decision_model);
    free(c->chat_model);
    c->decision_model = dm;
    c->chat_model = cm;
    c->decision = decision;
    c->chat = chat;
    c->rate = rate;
    return true;
}
Doc *ai_settings_update(App *a, J *in, bool connection, int *status) {
    if (a->ai_busy || a->head) {
        *status = 409;
        return error_doc("ai_busy", "Wait for queued AI work to finish before changing settings");
    }
    if (connection) {
        Provider p;
        const char *key = sget(in, "key");
        if (!provider_parse(sget(in, "provider"), &p) || !yyjson_mut_is_str(get(in, "key")) ||
            strlen(key) > 4096 || strchr(key, '\r') || strchr(key, '\n')) {
            *status = 400;
            return error_doc("bad_settings", "Invalid provider or API key");
        }
        char **dest = p == AI_OPENAI ? &a->cfg.key : &a->cfg.provider_keys[p];
        free(*dest);
        *dest = xstrdup(key);
    } else {
        ModelConfig dm, cm;
        if (!model_config_parse(get(in, "decision"), &dm) || !model_config_parse(get(in, "chat"), &cm) ||
            !yyjson_mut_is_num(get(in, "decision_input_usd_per_m")) ||
            !isfinite(number(get(in, "decision_input_usd_per_m"))) ||
            number(get(in, "decision_input_usd_per_m")) < 0) {
            *status = 400;
            return error_doc("bad_settings", "Set a model, provider input/context limits, output "
                                             "reserve, and nonnegative input rate for both roles");
        }
        Doc *saved = doc_new();
        put(saved, root(saved), "decision",
            model_config_json(saved, &dm, sget(get(in, "decision"), "model")));
        put(saved, root(saved), "chat", model_config_json(saved, &cm, sget(get(in, "chat"), "model")));
        put(saved, root(saved), "decision_input_usd_per_m",
            copy(saved, get(in, "decision_input_usd_per_m")));
        char *path = fmt("%s/ai-settings.json", a->cfg.data_dir), *bytes = json(root(saved)),
             *error = NULL;
        bool ok = atomic_write(path, bytes, strlen(bytes), &error);
        free(path);
        free(bytes);
        if (!ok) {
            *status = 500;
            Doc *d = error_doc("save_failed", error);
            free(error);
            doc_free(saved);
            return d;
        }
        apply_settings(&a->cfg, root(saved));
        doc_free(saved);
    }
    a->catalog_revision++;
    *status = 200;
    return ai_settings(a);
}
bool ai_settings_load(App *a, char **error) {
    char *path = fmt("%s/ai-settings.json", a->cfg.data_dir);
    size_t n;
    if (access(path, F_OK) && errno == ENOENT) {
        free(path);
        return true;
    }
    char *text = read_file(path, 16384, &n, error);
    free(path);
    if (!text)
        return false;
    Doc *d = doc_parse(text, n);
    free(text);
    bool ok = apply_settings(&a->cfg, root(d));
    doc_free(d);
    if (!ok)
        *error = xstrdup("Invalid saved AI settings");
    return ok;
}
