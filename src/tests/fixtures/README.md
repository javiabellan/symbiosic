# Fixtures

`claude-sanitized-native.jsonl` and `codex-sanitized-native.jsonl` were derived from native transcripts available on the development machine. They retain native record families, field paths and value types. Text, paths, model names, tool names, timestamps and identifiers were replaced; tool input/output bodies were replaced with inert examples. Base instructions, tool definitions, opaque metadata and encrypted reasoning were removed. No original conversation text or credentials are included.

`claude.jsonl` and `codex.jsonl` are compact, constructed regressions using those native shapes. They add deterministic duplicates, tool links, synthetic system context, images, compaction, missing timestamps and cumulative-usage resets. `subagent.jsonl` exercises an explicit native parent reference. `context.jsonl` locks the supported Codex `event_msg.payload.error_code == context_length_exceeded` mapping; all other context-limit variants remain unknown unless supported by a fixture. Claude's supported explicit field is a message row's `error == context_length_exceeded`.

Tool outcomes use only explicit booleans/structured exit codes. A textual phrase such as “failed” does not manufacture a deterministic failure. Unsupported mirror-only Codex records create coverage diagnostics instead of duplicate or invented messages.
