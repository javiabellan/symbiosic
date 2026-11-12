/* Behavior tests for the domain-aware, single-item analysis engine. */
static int detection_calls, retrieval_calls;
static Transport retrieval_fake(const char *endpoint, const char *body, const char *key) {
    Doc *req = parse(body);
    if (!strcmp(endpoint, "decisions")) {
        detection_calls++;
        CHECK(yyjson_mut_arr_size(get(root(req), "questions")) == 1);
        CHECK(!strcmp(sget(yyjson_mut_arr_get(get(root(req), "questions"), 0), "name"), "domain"));
        doc_free(req);
        return (Transport){
            .status = STATUS_OK,
            .body = xstrdup("{\"answers\":[{\"name\":\"domain\",\"type\":\"choice\",\"choice\":"
                            "\"frontend\",\"confidence\":0.9}],\"usage\":{\"input_tokens\":40}}")};
    }
    CHECK(!strcmp(endpoint, "responses"));
    if (retrieval_calls++ == 0 && yyjson_mut_arr_size(get(root(req), "tools"))) {
        doc_free(req);
        return (Transport){
            .status = STATUS_OK,
            .body = xstrdup("{\"output\":[{\"type\":\"function_call\",\"call_id\":\"read1\","
                            "\"name\":\"read_content\",\"arguments\":\"{\\\"path\\\":\\\"../"
                            "outside\\\",\\\"offset\\\":0}\"}],\"usage\":{\"input_tokens\":30,"
                            "\"output_tokens\":10}}")};
    }
    if (retrieval_calls > 1) {
        CHECK(strstr(body, "out_of_scope"));
    }
    doc_free(req);
    return fake(endpoint, body, key);
}
static void test_symbiosic(App *a) {
    char *error = NULL;
    Snapshot snap = {0};
    Doc *scope = doc_new();
    put(scope, root(scope), "type", js(scope, "trace"));
    put(scope, root(scope), "id", js(scope, cited_id));
    CHECK(snapshot_take(a, root(scope), &snap, &error));
    Doc *selection = doc_new();
    J *ids = arr(selection);
    add(ids, js(selection, cited_id));
    put(selection, root(selection), "trace_ids", ids);
    put(selection, root(selection), "domain", js(selection, "software-engineering"));
    ModelConfig saved = a->cfg.chat;
    a->cfg.chat.pricing_known = true;
    a->cfg.chat.input_rate = 1;
    a->cfg.chat.output_rate = 2;
    int before = calls;
    Doc *quote = estimate_build(a, &snap, selection, &error);
    CHECK(quote && !error && calls == before);
    CHECK(integer(get(root(quote), "total")) == 1);
    CHECK(!strcmp(sget(root(quote), "mode"), "full"));
    J *entry = yyjson_mut_arr_get(get(root(quote), "entries"), 0);
    CHECK(integer(get(get(entry, "coverage"), "level")) == 5);
    CHECK(strcmp(sget(get(entry, "coverage"), "endpoint"), "decisions"));
    Doc *result = analyze_entry(a, root(quote), entry, &a->analyses, &error);
    CHECK(result && !error && !get(root(result), "error"));
    CHECK(calls == before + 1);
    CHECK(get(root(result), "result"));
    CHECK(fabs(number(get(root(result), "cost_usd")) - .00014) < .0000001);
    CHECK(!strcmp(sent_body, sget(entry, "request_body")));
    docs_push(&a->analyses, analysis_index(result));
    doc_free(result);
    mode = 1;
    result = analyze_entry(a, root(quote), entry, &a->analyses, &error);
    CHECK(get(root(result), "error") && boolean(get(root(result), "outcome_unknown")));
    doc_free(result);
    mode = 2;
    result = analyze_entry(a, root(quote), entry, &a->analyses, &error);
    CHECK(get(root(result), "error"));
    doc_free(result);
    mode = 0;
    Doc *all = parse("{\"all\":true}");
    CHECK(!estimate_build(a, &snap, all, &error));
    CHECK(error);
    free(error);
    error = NULL;
    doc_free(all);
    add(ids, js(selection, cited_id));
    CHECK(!estimate_build(a, &snap, selection, &error));
    free(error);
    error = NULL;
    yyjson_mut_arr_remove_last(ids);
    Doc *domain =
        parse("{\"id\":\"custom-review\",\"name\":\"Custom Review\",\"detection_prompt\":\"Custom "
              "work\",\"analysis_prompt\":\"Check custom constraints.\"}");
    int status;
    Doc *updated = domains_update(a, root(domain), &status);
    CHECK(status == STATUS_OK && get(root(updated), "custom-review"));
    doc_free(updated);
    doc_free(domain);
    char *project = fmt("%s/project", a->cfg.data_dir);
    CHECK(mkdir_private(project, &error));
    char *file = fmt("%s/app.py", project);
    CHECK(atomic_write(file, "print('hello')\n", 15, &error));
    char *link = fmt("%s/escape", project);
    CHECK(symlink("/etc", link) == 0);
    free(link);
    Trace *t = a->traces.v[0];
    char *old_cwd = xstrdup(sget(meta(t), "cwd"));
    put(t->doc, meta(t), "cwd", js(t->doc, project));
    Doc *manifest = directory_manifest(a, project, &error);
    CHECK(manifest && !error);
    CHECK(integer(get(root(manifest), "file_count")) == 1);
    CHECK(integer(get(get(root(manifest), "languages"), "Python")) == 15);
    Doc *read = directory_read(project, "../outside", 0, 100, NULL);
    CHECK(get(root(read), "error"));
    doc_free(read);
    read = directory_read(project, "escape/passwd", 0, 100, NULL);
    CHECK(get(root(read), "error"));
    doc_free(read);
    read = directory_read(project, "app.py", 0, 5, NULL);
    CHECK(!strcmp(sget(root(read), "text"), "print"));
    CHECK(integer(get(root(read), "next_offset")) == 5);
    doc_free(read);
    Doc *dirsel = doc_new();
    put(dirsel, root(dirsel), "cwd", js(dirsel, project));
    Doc *dq = estimate_build(a, &snap, dirsel, &error);
    CHECK(dq && !error && !strcmp(sget(root(dq), "mode"), "full"));
    CHECK(strstr(sget(yyjson_mut_arr_get(get(root(dq), "entries"), 0), "evidence"),
                 "BEGIN FILE app.py"));
    doc_free(dq);
    char *binary = fmt("%s/image.bin", project);
    CHECK(atomic_write(binary, "\0\1\2\3", 4, &error));
    free(binary);
    put(dirsel, root(dirsel), "domain", js(dirsel, "auto"));
    dq = estimate_build(a, &snap, dirsel, &error);
    CHECK(dq && !error && !strcmp(sget(root(dq), "mode"), "agentic"));
    test_transport = retrieval_fake;
    result = analyze_entry(a, root(dq), yyjson_mut_arr_get(get(root(dq), "entries"), 0),
                           &a->analyses, &error);
    CHECK(result && !error && !get(root(result), "error"));
    CHECK(detection_calls == 1 && retrieval_calls == 2);
    CHECK(!strcmp(sget(root(result), "domain"), "frontend"));
    CHECK(!strcmp(sget(get(root(result), "result"), "coverage"), "partial"));
    docs_push(&a->analyses, analysis_index(result));
    doc_free(result);
    test_transport = fake;
    J *expected = NULL;
    size_t i, n;
    J *v;
    yyjson_mut_arr_foreach(get(root(manifest), "files"), i, n,
                           v) if (!strcmp(sget(v, "path"), "app.py")) expected = v;
    CHECK(expected);
    CHECK(atomic_write(file, "changed!", 8, &error));
    read = directory_read(project, "app.py", 0, 100, expected);
    CHECK(get(root(read), "error"));
    doc_free(read);
    put(t->doc, meta(t), "cwd", js(t->doc, old_cwd));
    free(old_cwd);
    free(project);
    free(file);
    doc_free(manifest);
    doc_free(dirsel);
    doc_free(dq);
    doc_free(quote);
    doc_free(selection);
    doc_free(scope);
    snapshot_free(&snap);
    a->cfg.chat = saved;
}
