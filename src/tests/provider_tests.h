/* Exercise the wire round trip rather than only the flattened response. */
static void test_provider_replay(App *a) {
    const char *fixtures[] = {
        "{\"output\":[{\"type\":\"reasoning\",\"id\":\"r1\",\"encrypted_content\":\"opaque\","
        "\"summary\":[]},{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"read_content\","
        "\"arguments\":\"{}\"}]}",
        "{\"content\":[{\"type\":\"thinking\",\"thinking\":\"visible\",\"signature\":\"signed\"},{"
        "\"type\":\"redacted_thinking\",\"data\":\"opaque\"},{\"type\":\"text\",\"text\":"
        "\"checking\"},{\"type\":\"tool_use\",\"id\":\"c1\",\"name\":\"read_content\",\"input\":{}}"
        ",{\"type\":\"tool_use\",\"id\":\"c2\",\"name\":\"read_content\",\"input\":{}}]}",
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"reasoning_details\":"
        "[{\"type\":\"reasoning.encrypted\",\"data\":\"opaque\",\"signature\":\"signed\"}],\"tool_"
        "calls\":[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"read_content\","
        "\"arguments\":\"{}\"}}]}}]}",
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
        "\"id\":\"c1\",\"type\":\"function\",\"extra_content\":{\"google\":{\"thought_signature\":"
        "\"signed\"}},\"function\":{\"name\":\"read_content\",\"arguments\":\"{}\"}}]}}]}"};
    ModelConfig saved = a->cfg.chat;
    for (int p = AI_OPENAI; p < AI_PROVIDERS; p++) {
        a->cfg.chat.provider = (Provider)p;
        const char *endpoint = p == AI_OPENAI      ? "responses"
                               : p == AI_ANTHROPIC ? "messages"
                                                   : "chat/completions";
        Doc *raw = parse(fixtures[p]);
        Doc *response = provider_response((Provider)p, false, endpoint, root(raw));
        Doc *request =
            parse("{\"model\":\"fixture\",\"instructions\":\"Review\",\"input\":[],\"tools\":[]}");
        J *input = get(root(request), "input");
        size_t i, n;
        J *item;
        yyjson_mut_arr_foreach(get(root(response), "output"), i, n, item) {
            add(input, copy(request, item));
        }
        const int results = p == AI_ANTHROPIC ? 2 : 1;
        for (int k = 0; k < results; k++) {
            J *result = obj(request);
            put(request, result, "type", js(request, "function_call_output"));
            put(request, result, "call_id", js(request, k ? "c2" : "c1"));
            put(request, result, "output", js(request, "evidence"));
            add(input, result);
        }
        Doc *wire = provider_request(&a->cfg, false, root(request), &endpoint);
        J *messages = get(root(wire), p == AI_OPENAI ? "input" : "messages");
        J *native = yyjson_mut_arr_get(messages, p == AI_OPENAI ? 0 : p == AI_ANTHROPIC ? 0 : 1);
        J *expected = p == AI_OPENAI ? yyjson_mut_arr_get(get(root(raw), "output"), 0)
                      : p == AI_ANTHROPIC
                          ? get(root(raw), "content")
                          : get(yyjson_mut_arr_get(get(root(raw), "choices"), 0), "message");
        char *before = json(expected);
        char *after = json(p == AI_ANTHROPIC ? get(native, "content") : native);
        CHECK(!strcmp(before, after));
        CHECK(yyjson_mut_arr_size(messages) == (size_t)(p == AI_OPENAI      ? 3
                                                        : p == AI_ANTHROPIC ? 2
                                                                            : 3));
        if (p == AI_ANTHROPIC) {
            CHECK(yyjson_mut_arr_size(get(yyjson_mut_arr_get_last(messages), "content")) == 2);
        }
        char *body = json(root(wire));
        CHECK(!strstr(body, "_provider_"));
        free(body);
        free(before);
        free(after);
        doc_free(wire);
        doc_free(request);
        doc_free(response);
        doc_free(raw);
    }
    a->cfg.chat = saved;
}
