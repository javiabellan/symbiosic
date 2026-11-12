/* Windows-authored prompts must parse exactly like LF prompts. */
static void test_prompt_newlines(App *a) {
    char *path = fmt("%s/prompts/domains/crlf.prompt", a->cfg.data_dir);
    const char *text = "---\r\n{\"name\":\"crlf\",\"description\":\"CRLF\","
                       "\"metadata\":{\"detection\":\"Line endings\"}}\r\n---\r\nReview.\r\n";
    char *error = NULL;
    CHECK(atomic_write(path, text, strlen(text), &error));
    App local = {.cfg = a->cfg};
    CHECK(prompts_load(&local, &error));
    J *domain = get(root(local.domains), "crlf");
    CHECK(!strcmp(sget(domain, "name"), "CRLF"));
    CHECK(!strcmp(sget(domain, "analysis_prompt"), "Review.\n"));
    doc_free(local.domains);
    doc_free(local.prompts);
    CHECK(unlink(path) == 0);
    free(path);
    free(error);
}
