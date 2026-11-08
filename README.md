# traceview

A local, single-user C17 proof of concept for exploring Claude Code and Codex transcripts. It normalizes native JSONL, calculates deterministic statistics, previews the cost of per-trace Decisions analysis, and investigates a frozen scope through a Responses chat with four read-only tools.

## Build and run

Install a C compiler, Make, pkg-config, **libcurl**, **libuv**, **yyjson**, and **llhttp** development headers and libraries. No Node.js or frontend build step is used.

On macOS with Homebrew:

```sh
brew install pkg-config curl libuv yyjson llhttp
cp env.template .env
# Edit .env and enter your own OPENAI_API_KEY, if you want paid operations.
make
./traceview
```

On Linux, install the same libraries using your distribution's development packages, or install upstream C releases into a system prefix. Package names and llhttp include locations vary by distribution. The build uses pkg-config for libcurl, libuv and yyjson, and links llhttp directly when its pkg-config file is unavailable. Flags are overridable:

```sh
make CC=clang PREFIX=/usr/local
# Example for a nonstandard installation:
make CPPFLAGS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I/opt/trace-libs/include" \
     LDFLAGS="-L/opt/trace-libs/lib" \
     LDLIBS="-lcurl -luv -lyyjson -lllhttp -lpthread -ldl -lm"
```

Run from the repository directory so `rubric.json`, `tools.json`, and `web/index.html` are available. The foreground process prints **http://127.0.0.1:4317**. Open that address yourself; the application never launches a browser, forks, daemonizes, or installs a service. Ctrl-C or SIGTERM stops accepting work, interrupts queued jobs, and lets active workers finish. An active network call may take up to its 120-second timeout to finish.

An empty key allows ingestion, browsing, statistics, saved results, and local estimates. Paid analysis and chat require a key. The program never copies an environment key into a file. Existing environment variables override `.env`; `.env` supports comments, blank lines, and literal `KEY=value` with matching optional quotes. Shell expressions are not evaluated. Only leading `~/` in configured paths expands.

For a separate, keyless demonstration using the checked-in fixtures:

```sh
OPENAI_API_KEY= DATA_DIR=./data/demo \
  CLAUDE_ROOT=./tests/demo/claude CODEX_ROOT=./tests/demo/codex ./traceview
```

The demo roots contain sanitized example transcripts. They are independent from the native roots in `env.template`. Providers are recognized from record structure; repeated paths across configured roots are imported once.

## Using it

1. Select a directory or a provider-tagged session. Identical normalized working directories share a group across providers. Missing cwd appears under **Unknown directory**. Worktrees remain separate.
2. Inspect deterministic statistics and expand chronological events. Long event text has a continuation button; complete normalized text remains in the local snapshot. Tool results are separate from human messages, and missing usage or outcomes remain unknown.
3. **Analyze** previews the selected trace. **Analyze all** previews every imported trace, regardless of the current directory. Neither action contacts OpenAI.
4. Review the native dialog: eligible/cached/excluded counts, approximate input tokens, six-decimal USD, and coverage warnings. **Start analysis** commits the saved bodies. Quotes expire after ten minutes, and repeated Start returns the same job.
5. Changes arriving after the quote do not change its requests. Old-revision results remain visible as **Outdated**; a matching current result takes precedence. There is no force-reanalyze button.
6. Ask a question in a trace, exact directory, or all scope. A chat's scope is immutable. Changing the scope starts a different conversation; saved chats can be reopened. Each turn freezes its trace revisions and analysis references before entering the queue.

Analysis and chat send selected content to OpenAI. **No automatic secret redaction is implemented.** Images become `[image omitted]`; encrypted reasoning is not decoded. The Decisions projection omits reasoning and routine metadata, and shortens tool results over 2 KiB to UTF-8-safe first/last 1 KiB excerpts. An entire request over 128 KiB is excluded, without chunking or summarization.

Chat has separate Responses usage; the Decisions price is never applied to it. The model receives at most the last six completed exchanges, capped at 16 KiB by dropping complete oldest exchanges. Prior tool bodies are not resent. Chat citations use `[trace_id@revision:seq]`; the UI checks the current revision before displaying evidence and explicitly reports when an older revision is unavailable. Retrieved older evidence remains in the chat audit.

## Costs and judgments

The initial `gpt-6-luna` Decisions input rate is `$0.10 / 1M`, checked **2026-11-08**, for standard short-context processing at `api.openai.com`. Model names and the rate are configurable. Estimates use `ceil(serialized_request_bytes / 4)` with an illustrative `ceil(bytes / 6)`–`ceil(bytes / 2)` range; this is not a guaranteed bound. A 100,000-token estimate at the default rate is approximately `$0.010000`.

Explicit prompt-cache mode without breakpoints disables provider caching. Completed local results are reused only for the same trace revision, normalizer version, rubric content, model and exact serialized request. Full saved request bodies are read for comparison only after metadata matches. Refusals are valid saved results. Invalid JSON, missing required answer names, HTTP failures and transport errors are job failures. Received raw responses and usage are retained, including errors.

Reported cost is `input_tokens × frozen_rate / 1,000,000`, with no Decisions output charge. Unexpected cache reads/writes or missing usage make pricing unresolved. This is an application calculation, not an invoice. Requests are never automatically retried; a timeout may mean the provider processed the request. Explicitly obtain a new estimate before retrying a failed request. A crash cannot guarantee exactly-once billing.

Predicates display true at `p >= 0.8`, false at `p <= 0.2`, and otherwise unknown. Choices and scores require confidence at least `0.6`. These are uncalibrated PoC thresholds. Incomplete or shortened evidence prevents negative factual conclusions. Scores retain fractional, probability-weighted values; steering is a bin, not an exact correction count. Plan following is masked without a confident applicable plan and observed implementation. Raw probability distributions and confidence values are preserved. Deterministic failures and compaction counts remain separate from model judgments.

## Files, workers and limits

`DATA_DIR` is private (0700); files are owner-only (0600). An advisory `fcntl` lock prevents a second process from using the same directory. JSON and JSONL snapshots use a same-directory temporary file, close, and rename. This is atomic replacement, not a power-loss durability guarantee.

| Path | Contents |
| --- | --- |
| `traces/<id>.jsonl` | Current normalized snapshot, metadata first |
| `analysis/<trace-id>/<analysis-id>.json` | Immutable completed analysis, exact request, raw response, coverage, usage and calculated cost |
| `quotes/<id>.json` | Frozen requests, revisions, rate and estimates |
| `jobs/<id>.json` | Batch/chat state, individual errors and usage |
| `chats/<id>.json` | Immutable scope, display history, usage and latest job |
| `chats/<id>.jsonl` | Append-only snapshot, request, response, tool/evidence and final-answer audit |
| `instance.lock` | Single-writer lock |

On restart, files rebuild the catalog and saved-analysis metadata. Unfinished jobs become interrupted and are never replayed. Incomplete audit tails are truncated before the next append. A paid result that cannot be saved remains available in process memory and is marked **Result not saved**; it is not requested again as recovery.

There is one libuv event loop and two serialized `uv_queue_work` lanes: ingestion and AI. Workers consume immutable snapshots and publish through completion callbacks. AI batches yield between trace requests so queued chat/local estimates can run. Network latency does not block ingestion or HTTP processing. Current normalized snapshots are retained as immutable, reference-counted objects; quoted bodies and chat snapshots cannot follow a mutable catalog pointer.

Discovery scans both roots every `POLL_MS`, skips symlinks and unrelated sidecars, and compares device/inode, size and nanosecond mtime. Unchanged supported and rejected files are not reparsed. Changed files rebuild from their captured complete-line prefix. Source deletion keeps the last snapshot browsable. The originals are never modified or backed up; unsupported native fields may be lost if the original file is removed.

| Bound | Value |
| --- | --- |
| Source files per scan | 1,000 |
| Native file / line | 64 MiB / 4 MiB |
| Normalized snapshot | 16 MiB (including envelope overhead) |
| Chat turn snapshot | 256 MiB |
| Decisions / Responses request | 128 KiB |
| Tool result / cumulative tool evidence | 16 KiB / 64 KiB |
| Tool page | 1–50 items |
| Search query | 256 UTF-8 bytes, literal and case-sensitive |
| Chat loop | 6 tool rounds and at most 1 final tools-disabled request |
| HTTP headers / API body | 8 KiB / 16 KiB |
| Provider response | 8 MiB |

Only four model tools exist: `list_sessions`, `get_stats`, `search_events`, and `read_events`. C validates all required properties, argument types, ranges, enums and scope membership. Tools cannot read arbitrary paths, execute commands, write, or make network calls. Totals cover the full scope; pagination changes only displayed rows. Overlap statistics describe **overlapping session spans**, not proven simultaneous execution.

HTTP binds only to `127.0.0.1`. Host must be exactly `127.0.0.1:<PORT>`; browser Origin, when present, must be exactly `http://127.0.0.1:<PORT>`. CLI clients without Origin are supported. There is no CORS, remote account system, streaming, compression or keep-alive. libcurl verifies TLS, does not follow redirects, and uses a 10-second connect / 120-second total timeout. Provider error text is deliberately summarized so it cannot echo submitted secrets.

This deliberately bounded implementation uses full-file rebuilding, linear queries, in-memory current snapshots, and limited native-format compatibility. It does not reconstruct branches, merge subagents, perform incremental/model-background analyses, redact secrets, compact chat automatically, add embeddings/MCP, or provide production hosting. Memory exhaustion is reported to stderr and terminates the process without publishing partial files; other parse, provider and persistence errors are surfaced per operation.

## Validation and source layout

```sh
make test       # C fixtures/fake transport + Python 3 loopback integration tests
make sanitize   # AddressSanitizer and UndefinedBehaviorSanitizer on C tests
make clean
```

Tests never require a key or call a paid API. `TRACEVIEW_TEST` is the only build exposing the injectable transport function pointer; there is no runtime fake endpoint or credential-forwarding override. Python 3 is used only by the additional HTTP integration test. The normal application has only the four C library dependencies.

Tests cover both documented adapters and sanitized native samples, deduplication, partial lines, replacement/deletion, usage resets, scope escapes, pagination, quotes, cache reuse, fractional scores/refusals, unsaved paid results, restart interruption, saved chat audits, fragmented HTTP, local request guards and simultaneous ingestion during a slow fake network request. macOS builds and tests were run locally. Linux portability is supported by the POSIX/libuv implementation and overridable flags; a Linux runtime was not available in this workspace.

| Module | Responsibility |
| --- | --- |
| `main.c` | Foreground lifecycle, catalog publication and two work queues |
| `http.c` | libuv TCP, llhttp, fixed routes and request guards |
| `store.c` | Private files, atomic replacement, discovery and snapshots |
| `normalize.c` | Provider adapters and deterministic per-trace statistics |
| `query.c` | Summaries, aggregates, overlap calculations and bounded tools |
| `ai.c` | Projection, quotes, Decisions, cache and libcurl |
| `chat.c` | Responses tool loop, bounded history and audit |
| `util.c`, `app.h` | Checked allocation, JSON helpers, configuration and shared types |
| `web/index.html` | Single-file vanilla interface; dynamic text uses `textContent` |

Contracts and reference material:

- [Decisions API](https://developers.openai.com/api/reference/resources/decisions/methods/create) and [Decisions guide, scoring and pricing](https://developers.openai.com/api/docs/guides/decisions).
- [GPT-6 Luna](https://developers.openai.com/api/docs/models/gpt-6-luna) and [Responses function calling](https://developers.openai.com/api/docs/guides/function-calling).
- [Claude Code adapter notes](https://github.com/letta-ai/trajectory/blob/main/src/adapters/claude-code/README.md) and [Codex adapter notes](https://github.com/letta-ai/trajectory/blob/main/src/adapters/codex/README.md). These informed the supported field paths; traceview does not depend on Letta or validate against its schema.
