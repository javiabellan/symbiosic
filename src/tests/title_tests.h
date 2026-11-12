/* Native names outrank generated summaries, including later summary records. */
static void test_native_titles(App *a) {
    char *path = fmt("%s/title.jsonl", a->cfg.data_dir);
    const char *records = "{\"type\":\"user\",\"sessionId\":\"named\",\"message\":{\"role\":"
                          "\"user\",\"content\":\"Fallback prompt\"}}\n"
                          "{\"type\":\"custom-title\",\"customTitle\":\"Chosen name\"}\n"
                          "{\"type\":\"summary\",\"summary\":\"Generated summary\"}\n";
    char *error = NULL;
    CHECK(atomic_write(path, records, strlen(records), &error));
    Trace *trace = norm(path, "claude", NULL);
    CHECK(!strcmp(sget(meta(trace), "title"), "Chosen name"));
    trace_release(trace);

    records = "{\"type\":\"session_meta\",\"payload\":{\"id\":\"named\",\"thread_name\":\"Native "
              "Codex name\"}}\n";
    CHECK(atomic_write(path, records, strlen(records), &error));
    trace = norm(path, "codex", NULL);
    CHECK(!strcmp(sget(meta(trace), "title"), "Native Codex name"));
    trace_release(trace);
    CHECK(unlink(path) == 0);
    free(error);
    free(path);
}
