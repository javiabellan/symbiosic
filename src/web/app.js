"use strict";
const $ = (id) => document.getElementById(id),
  make = (tag, text, cls) => {
    const e = document.createElement(tag);
    if (text !== undefined) {
      e.textContent = text;
    }
    if (cls) {
      e.className = cls;
    }
    return e;
  },
  button = (text, fn, cls) => {
    const b = make("button", text, cls);
    b.type = "button";
    b.onclick = fn;
    return b;
  },
  providers = {
    openai: "OpenAI",
    anthropic: "Anthropic",
    gemini: "Google Gemini",
    openrouter: "OpenRouter",
  },
  colors = {
    user: "#bd8ba7",
    assistant: "#8bb798",
    system: "#a99bd0",
    reasoning: "#ad9bc2",
    tool_result: "#e2c66f",
    tool_call: "#8bb798",
  },
  langColors = {
    C: "#73927e",
    "C++": "#9d93bc",
    Python: "#7f9fb2",
    JavaScript: "#d5bf71",
    TypeScript: "#7d9fc3",
    HTML: "#cf987d",
    CSS: "#aa97bc",
    Rust: "#b58f79",
    JSON: "#a5b8a7",
    Markdown: "#bac8bb",
    Prompts: "#97b6a2",
    Other: "#c8d3ca",
  };
const paths = {
  folder: "M2 5h6l2 2h12v13H2z",
  file: "M5 2h9l5 5v15H5z M14 2v6h5",
  settings:
    "M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8z M12 2v3m0 14v3M2 12h3m14 0h3M5 5l2 2m10 10 2 2M5 19l2-2M17 7l2-2",
  copy: "M9 3h11v14H9z M5 7H3v14h12v-2",
  trace: "M4 5h16M4 12h11M4 19h16",
  summary: "M4 4h16v16H4z M8 8h8M8 12h8M8 16h4",
  issues: "M12 3 2 21h20z M12 9v5m0 3v1",
  chat: "M3 4h18v13H8l-5 4z",
  arrow: "m9 5 7 7-7 7",
  refresh: "M20 7a8 8 0 1 0 1 7 M20 2v6h-6",
  check: "m5 12 4 4 10-10",
};
function icon(name, cls = "") {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("fill", name === "folder" ? "currentColor" : "none");
  svg.setAttribute("stroke", "currentColor");
  svg.setAttribute("stroke-width", "1.4");
  svg.setAttribute("stroke-linejoin", "round");
  svg.setAttribute("stroke-linecap", "round");
  svg.setAttribute("aria-hidden", "true");
  svg.classList.add("icon");
  if (cls) {
    svg.classList.add(cls);
  }
  const p = document.createElementNS(svg.namespaceURI, "path");
  p.setAttribute("d", paths[name] || paths.file);
  svg.append(p);
  return svg;
}
const num = (x) => new Intl.NumberFormat().format(x || 0),
  money = (x) =>
    x == null
      ? "Not configured"
      : new Intl.NumberFormat("en-US", {
          style: "currency",
          currency: "USD",
          minimumFractionDigits: 3,
          maximumFractionDigits: 4,
        }).format(x),
  base = (p) => p?.split("/").filter(Boolean).pop() || "Unknown directory",
  date = (s) =>
    s
      ? new Date(s).toLocaleDateString(undefined, {
          day: "numeric",
          month: "short",
          year: "numeric",
        })
      : "Date unknown",
  duration = (s) =>
    s == null
      ? "Time unknown"
      : s < 60
        ? `${Math.round(s * 10) / 10}s`
        : `${Math.floor(s / 60)}m ${Math.round(s % 60)}s`,
  qs = (o) => new URLSearchParams(o).toString();
function ago(sec) {
  if (!sec) {
    return "—";
  }
  let d = Math.max(0, Date.now() / 1000 - sec);
  return d < 60
    ? "just now"
    : d < 3600
      ? `${Math.floor(d / 60)} min ago`
      : d < 86400
        ? `${Math.floor(d / 3600)} hours ago`
        : `${Math.floor(d / 86400)} days ago`;
}
async function api(path, body) {
  const res = await fetch(path, {
    method: body === undefined ? "GET" : "POST",
    headers: body === undefined ? {} : { "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const data = await res.json();
  if (!res.ok || data.error) {
    throw new Error(
      data.error?.message || data.message || data.error || `Request failed (${res.status})`,
    );
  }
  return data;
}
const THINKING_LEVELS = Object.freeze(["default", "none", "low", "medium", "high", "xhigh"]);
const FRAMING_TOKENS = 1024;
const POLL_MS = 3000;
const JobKind = Object.freeze({ ANALYSIS: "analysis", CHAT: "chat" });
const ModelRole = Object.freeze({ REVIEW: "review", DETECTION: "detection" });
let previewId = 0,
  estimating = false;
const modelChoices = new Map();
let errorTimer;
function fail(e) {
  $("error").textContent = e.message || String(e);
  $("error").style.display = "block";
  clearTimeout(errorTimer);
  errorTimer = setTimeout(() => ($("error").style.display = "none"), 10000);
}
let state = null,
  settings = null,
  domains = {},
  scope = null,
  tab = "original",
  analysis = null,
  model = null,
  domain = "software-engineering",
  dirPath = "",
  generation = 0,
  trace = null,
  overview = [],
  tokenMode = false,
  quote = null,
  settingsTab = "providers",
  domainDraft = null,
  chatId = null,
  chatBusy = false,
  activeJob = null,
  picker = null,
  polling = false;
let settingsDraft = null;
const expanded = new Set(),
  hiddenTypes = new Set(),
  eventCache = new Map(),
  modelCatalog = new Map();
let observer = null,
  loads = 0,
  loadQueue = [];
function scopeKey(s = scope) {
  return s?.type === "trace" ? `t:${s.id}` : `d:${s?.cwd || ""}`;
}
function copyButton(text) {
  const b = button(
    "",
    async () => {
      try {
        await navigator.clipboard.writeText(text);
        b.replaceChildren(icon("check"), document.createTextNode("Copied"));
        setTimeout(() => b.replaceChildren(icon("copy")), 1500);
      } catch {
        fail(new Error("Clipboard unavailable. Select and copy the prompt text."));
      }
    },
    "copy",
  );
  b.append(icon("copy"));
  b.title = "Copy prompt";
  b.setAttribute("aria-label", "Copy prompt");
  return b;
}
function notice(text, cls = "") {
  return make("div", text, `notice ${cls}`);
}
function renderNav() {
  const target = $("tree"),
    query = $("search").value.toLowerCase();
  target.replaceChildren();
  const groups = state.groups || [];
  $("inventory").textContent = `${groups.length} dirs · ${state.traces.length} sessions`;
  for (const group of groups) {
    const cwd = group.group === "Unknown directory" ? "" : group.group,
      traces = state.traces
        .filter((t) => (t.cwd || "") === cwd)
        .filter(
          (t) =>
            (t.title || "Untitled session").toLowerCase().includes(query) ||
            cwd.toLowerCase().includes(query),
        );
    if (query && !traces.length) {
      continue;
    }
    const wrap = make("section", undefined, "group"),
      head = make("div", undefined, "row");
    const toggle = button(
      expanded.has(cwd) ? "⌄" : "›",
      () => {
        expanded.has(cwd) ? expanded.delete(cwd) : expanded.add(cwd);
        renderNav();
      },
      "collapse",
    );
    toggle.setAttribute("aria-label", `${expanded.has(cwd) ? "Collapse" : "Expand"} ${base(cwd)}`);
    toggle.setAttribute("aria-expanded", String(expanded.has(cwd)));
    const b = button(
      "",
      () => selectScope({ type: "directory", cwd }),
      `directory-button ${scope?.type === "directory" && scope.cwd === cwd ? "active" : ""}`,
    );
    b.title = cwd || "Unknown directory";
    b.append(
      icon("folder", "folder-icon"),
      make("span", base(cwd), "dirname"),
      make("span", String(traces.length), "muted"),
    );
    head.append(toggle, b);
    wrap.append(head);
    if (expanded.has(cwd) || query) {
      for (const t of traces) {
        const session = button(
          "",
          () => selectScope({ type: "trace", id: t.trace_id }),
          `session ${scope?.type === "trace" && scope.id === t.trace_id ? "active" : ""}`,
        );
        session.title = t.title || "Untitled session";
        session.append(
          make("span", t.provider === "codex" ? "CODEX" : "CLAUDE", `badge ${t.provider}`),
          make("span", t.title || "Untitled session", "session-title"),
        );
        if (t.analysis) {
          session.append(make("span", "·", "muted"));
        }
        wrap.append(session);
      }
    }
    target.append(wrap);
  }
  if (!groups.length) {
    target.append(make("div", "Your sessions will appear here.", "empty"));
  }
  const counts = {
    codex: state.traces.filter((t) => t.provider === "codex").length,
    claude: state.traces.filter((t) => t.provider === "claude").length,
  };
  $("source-bar").replaceChildren();
  $("source-legend").replaceChildren();
  for (const [p, n] of Object.entries(counts)) {
    const segment = make("div");
    segment.style.cssText = `flex:${n};background:var(--${p})`;
    $("source-bar").append(segment);
    const row = make("div", undefined, "row"),
      dot = make("i", undefined, "dot");
    dot.style.background = `var(--${p})`;
    row.append(
      dot,
      make("span", p === "codex" ? "Codex" : "Claude Code"),
      make("span", `${num(n)} sessions`),
    );
    $("source-legend").append(row);
  }
}
async function selectScope(next) {
  previewId++;
  quote = null;
  document.querySelector(".sidebar").classList.remove("nav-open");
  $("toggle-sessions").setAttribute("aria-expanded", "false");
  scope = next;
  tab = "original";
  analysis = null;
  dirPath = "";
  trace = null;
  chatId = null;
  chatBusy = false;
  eventCache.clear();
  overview = [];
  expanded.add(
    next.type === "directory"
      ? next.cwd
      : state.traces.find((t) => t.trace_id === next.id)?.cwd || "",
  );
  renderNav();
  await renderSelection();
}
async function renderSelection() {
  const gen = ++generation;
  if (observer) {
    observer.disconnect();
  }
  loadQueue = [];
  renderRight();
  $("filters").hidden = true;
  $("trace-area").hidden = true;
  $("content").hidden = false;
  $("content").replaceChildren(make("div", "Loading…", "empty"));
  try {
    if (!scope) {
      renderWelcome();
      return;
    }
    const params = scope.type === "trace" ? { id: scope.id } : { cwd: scope.cwd };
    analysis = await api("/api/analysis?" + qs(params));
    if (gen !== generation) {
      return;
    }
    if (!analysis.id) {
      analysis = null;
    }
    if (scope.type === "trace") {
      trace = await api("/api/trace?" + qs({ id: scope.id, overview: 1 }));
      if (gen !== generation) {
        return;
      }
      overview = trace.items || [];
      renderTraceHeader();
    } else {
      await renderDirectory(gen);
    }
    if (gen !== generation) {
      return;
    }
    renderRight();
    await renderTab(gen);
  } catch (e) {
    if (gen === generation) {
      $("content").replaceChildren(notice(e.message, "error"));
      renderRight();
      if (scope?.type === "directory") {
        renderDirectoryHeader({ languages: {}, cwd: scope.cwd });
      } else {
        renderTraceHeader();
      }
    }
  }
}
function renderWelcome() {
  $("workspace-head").replaceChildren(make("div", "Workspace", "eyebrow"));
  const empty = make("div", undefined, "empty");
  empty.append(
    make("div", "⌘", "empty-icon"),
    make("h2", "Session workspace"),
    make("p", "Browse Codex and Claude Code sessions. Select an item to review."),
  );
  $("content").replaceChildren(empty);
  renderRight();
}
function renderDirectoryHeader(data) {
  const h = $("workspace-head");
  h.replaceChildren();
  const crumb = make("div", undefined, "breadcrumb");
  crumb.append(icon("folder"), make("span", "Working directory"));
  h.append(
    crumb,
    make("h1", base(scope.cwd)),
    make("div", scope.cwd || "This session did not record a working directory.", "path mono"),
  );
  const languages = Object.entries(data.languages || {}).sort((a, b) => b[1] - a[1]),
    total = languages.reduce((s, [, n]) => s + n, 0);
  if (total) {
    const bar = make("div", undefined, "language-bar"),
      legend = make("div", undefined, "language-legend");
    for (const [lang, bytes] of languages) {
      const part = make("div");
      part.style.cssText = `flex:${bytes};background:${langColors[lang] || "#c5cfc6"}`;
      part.title = `${lang} ${((bytes / total) * 100).toFixed(1)}%`;
      bar.append(part);
    }
    for (const [lang, bytes] of languages.slice(0, 6)) {
      const item = make("span"),
        dot = make("i", undefined, "dot");
      dot.style.background = langColors[lang] || "#c5cfc6";
      item.append(dot, document.createTextNode(`${lang} ${((bytes / total) * 100).toFixed(1)}%`));
      legend.append(item);
    }
    h.append(bar, legend);
  }
}
async function renderDirectory(gen = generation, offset = 0) {
  const data = await api("/api/directory?" + qs({ cwd: scope.cwd, path: dirPath, offset }));
  if (gen !== generation) {
    return;
  }
  renderDirectoryHeader(data);
  if (tab !== "original") {
    return;
  }
  const content = $("content");
  if (!offset) {
    content.replaceChildren();
  }
  const bread = make("div", undefined, "breadcrumb");
  let accum = "";
  bread.append(button(base(scope.cwd), () => navigateDir(""), "quiet"));
  for (const segment of dirPath.split("/").filter(Boolean)) {
    accum = accum ? `${accum}/${segment}` : segment;
    const p = accum;
    bread.append(
      make("span", "/"),
      button(segment, () => navigateDir(p), "quiet"),
    );
  }
  if (!offset) {
    content.append(bread);
  }
  const wrap = make("div", undefined, "table-wrap"),
    table = make("table", undefined, "file-table"),
    thead = make("thead"),
    tr = make("tr");
  for (const name of ["Name", "Size", "Last modified"]) {
    tr.append(make("th", name));
  }
  thead.append(tr);
  table.append(thead);
  const tbody = make("tbody");
  if (dirPath && !offset) {
    const row = make("tr"),
      cell = make("td"),
      up = button("..", () => navigateDir(dirPath.split("/").slice(0, -1).join("/")), "file-name");
    up.prepend(icon("folder", "folder-icon"));
    cell.append(up);
    row.append(cell, make("td", ""), make("td", ""));
    tbody.append(row);
  }
  for (const file of data.items.sort(
    (a, b) =>
      (a.type === "directory" ? 0 : 1) - (b.type === "directory" ? 0 : 1) ||
      a.name.localeCompare(b.name),
  )) {
    const row = make("tr"),
      name = make("td"),
      b = button(
        "",
        () => (file.type === "directory" ? navigateDir(file.path) : previewFile(file.path)),
        "file-name",
      );
    b.title = file.path;
    b.append(
      icon(
        file.type === "directory" ? "folder" : "file",
        file.type === "directory" ? "folder-icon" : "file-icon",
      ),
      make("span", file.name),
    );
    if (!["directory", "file"].includes(file.type)) {
      b.disabled = true;
      b.title = `${file.type} is not followed`;
    }
    name.append(b);
    row.append(
      name,
      make("td", file.type === "file" ? formatBytes(file.bytes) : "", "file-date"),
      make("td", ago(file.modified), "file-date"),
    );
    tbody.append(row);
  }
  table.append(tbody);
  wrap.append(table);
  if (!data.items.length) {
    wrap.append(make("div", "This directory is empty.", "empty"));
  }
  content.append(wrap);
  const foot = make("div", undefined, "directory-foot");
  foot.append(
    make(
      "span",
      `${num(data.file_count)} files · ${formatBytes(data.total_bytes)} in this workspace`,
    ),
    make("span", "Read-only · on your machine"),
  );
  content.append(foot);
  if (data.unreadable) {
    content.append(
      notice(
        `${data.unreadable} entries could not be read. Analysis will report incomplete coverage.`,
      ),
    );
  }
  if (data.next_offset != null) {
    content.append(
      button("Load more files", async (e) => {
        e.currentTarget.remove();
        await renderDirectory(gen, data.next_offset);
      }),
    );
  }
}
const formatBytes = (n) =>
  n < 1024
    ? `${n || 0} B`
    : n < 1048576
      ? `${(n / 1024).toFixed(1)} KB`
      : n < 1073741824
        ? `${(n / 1048576).toFixed(1)} MB`
        : `${(n / 1073741824).toFixed(1)} GB`;
async function navigateDir(path) {
  previewId++;
  dirPath = path;
  try {
    await renderDirectory();
  } catch (e) {
    fail(e);
  }
}
// Keep file and message previews in the workspace, with a return path.
function showPreview(title, back) {
  previewId++;
  const panel = make("section", undefined, "file-preview");
  const head = make("div", undefined, "preview-head");
  const body = make("div", undefined, "preview-body");
  head.append(button("← Back", back), make("h2", title, "mono"));
  panel.append(head, body);
  $("filters").hidden = true;
  $("trace-area").hidden = true;
  $("content").hidden = false;
  $("content").replaceChildren(panel);
  $("content").scrollTop = 0;
  return { body, id: previewId };
}

async function previewFile(path) {
  const cwd = scope.cwd;
  const view = showPreview(path, () => navigateDir(dirPath));
  view.body.append(make("p", "Reading file…", "muted"));
  const load = async (offset = 0) => {
    const data = await api("/api/file?" + qs({ cwd, path, offset }));
    if (view.id !== previewId) {
      return;
    }
    if (!offset) {
      view.body.replaceChildren();
    }
    if (data.binary) {
      view.body.append(
        notice("Binary file · 128-byte header"),
        make("p", formatBytes(data.bytes)),
        make("pre", data.hex_header),
      );
    } else {
      view.body.append(make("pre", data.text || "(Empty file)"));
    }
    if (data.next_offset == null) {
      return;
    }
    const more = button("Read next part", async () => {
      more.disabled = true;
      try {
        await load(data.next_offset);
        more.remove();
      } catch (error) {
        more.disabled = false;
        view.body.append(notice(error.message, "error"));
      }
    });
    view.body.append(more);
  };
  try {
    await load();
  } catch (error) {
    view.body.replaceChildren(notice(error.message, "error"));
  }
}
function renderTraceHeader() {
  const t = trace?.meta || state.traces.find((t) => t.trace_id === scope.id) || {},
    stats = t.stats || {},
    header = $("workspace-head");
  header.replaceChildren();
  const row = make("div", undefined, "trace-header"),
    left = make("div", undefined, "grow"),
    title = make("div", undefined, "row");
  title.append(
    make("h1", t.title || "Untitled session"),
    make("span", date(stats.first_ts), "session-date"),
  );
  left.append(title);
  const byline = make("div", undefined, "trace-byline"),
    dot = make("span", undefined, "dot");
  dot.style.background = t.provider === "claude" ? "var(--claude)" : "var(--codex)";
  byline.append(
    dot,
    make(
      "span",
      `${t.provider === "claude" ? "Claude Code" : "Codex"} · ${stats.human_turns || 0} human turns`,
    ),
  );
  left.append(byline);
  const right = make("div", undefined, "session-model");
  const label = [
    providers[t.model_provider] || "",
    t.model || "Model not recorded",
    t.thinking ? `· ${t.thinking}` : "",
  ]
    .filter(Boolean)
    .join(" ");
  right.append(make("div", label));
  const price = make("span", sessionCost(t), "badge");
  price.title =
    "Estimate using recorded usage and configured matching model rates. Unknown when pricing or usage is missing.";
  right.append(price);
  const sw = make("div", undefined, "mode-switch");
  sw.append(
    button(
      duration(stats.span_seconds),
      () => {
        tokenMode = false;
        renderTraceHeader();
        renderConversation();
      },
      tokenMode ? "" : "active",
    ),
    button(
      `${num(overview.reduce((s, e) => s + (e.tokens || 0), 0))} tokens`,
      () => {
        tokenMode = true;
        renderTraceHeader();
        renderConversation();
      },
      tokenMode ? "active" : "",
    ),
  );
  right.append(sw);
  row.append(left, right);
  header.append(row);
}
function sessionCost(t) {
  const cfg = settings?.chat,
    u = t.stats?.usage;
  if (!cfg?.pricing_known || !t.model || t.model !== cfg.model || !u) {
    return "Cost unknown";
  }
  const c = u.components || u;
  const input = c.input_tokens,
    output = c.output_tokens;
  if (input == null || output == null) {
    return "Cost unknown";
  }
  return "~" + money((input * cfg.input_rate + output * cfg.output_rate) / 1e6);
}
function eventType(e) {
  return e.kind === "tool_result"
    ? "tool_result"
    : e.kind === "tool_call"
      ? "tool_call"
      : e.kind === "reasoning"
        ? "reasoning"
        : ["system", "developer"].includes(e.role)
          ? "system"
          : e.role === "user"
            ? "user"
            : "assistant";
}
function filterType(e) {
  const type = eventType(e);
  return type.startsWith("tool_") ? "tools" : type;
}
function renderConversation() {
  if (observer) {
    observer.disconnect();
  }
  loadQueue = [];
  const conv = $("conversation"),
    map = $("minimap");
  conv.replaceChildren();
  map.replaceChildren();
  $("trace-area").classList.toggle("token-mode", tokenMode);
  $("filters").hidden = false;
  $("filters").replaceChildren();
  const filters = [
    ["system", "Context", colors.system],
    ["user", "You", colors.user],
    ["assistant", "Assistant", colors.assistant],
    ["tools", "Tools", colors.tool_result],
    ["reasoning", "Thinking", colors.reasoning],
  ];
  for (const [type, label, color] of filters) {
    const b = button(
        "",
        () => {
          hiddenTypes.has(type) ? hiddenTypes.delete(type) : hiddenTypes.add(type);
          renderConversation();
        },
        `filter ${hiddenTypes.has(type) ? "" : "on"}`,
      ),
      dot = make("i", undefined, "dot");
    dot.style.background = color;
    b.append(dot, document.createTextNode(label));
    b.setAttribute("aria-pressed", String(!hiddenTypes.has(type)));
    $("filters").append(b);
  }
  const total = overview.reduce((s, e) => s + (e.tokens || 0), 0),
    windowSize = trace.meta.context_window || total || 1;
  let used = 0,
    lastZone = "";
  conv.append(
    make(
      "div",
      tokenMode
        ? trace.meta.context_window
          ? `Context window · ${num(trace.meta.context_window)} tokens · colors show cumulative token position`
          : "Context limit not recorded · colors show relative position in this conversation"
        : "Durations are timestamp intervals; they may include idle time.",
      "timing-note",
    ),
  );
  observer = new IntersectionObserver(
    (entries) => {
      for (const entry of entries) {
        if (entry.isIntersecting) {
          observer.unobserve(entry.target);
          loadQueue.push({
            bubble: entry.target,
            seq: Number(entry.target.dataset.seq),
            id: scope.id,
            gen: generation,
          });
          drainEvents();
        }
      }
    },
    { root: conv, rootMargin: "200px" },
  );
  for (const e of overview) {
    const type = eventType(e),
      hidden = hiddenTypes.has(filterType(e)),
      tokens = e.tokens || 0;
    used += tokens;
    const ratio = used / windowSize,
      zone = ratio <= 1 / 3 ? "safe" : ratio <= 2 / 3 ? "warning" : "danger";
    const bar = button(
      "",
      () => {
        if (hiddenTypes.has(filterType(e))) {
          hiddenTypes.delete(filterType(e));
          renderConversation();
        }
        document
          .getElementById(`event-${e.seq}`)
          ?.scrollIntoView({ behavior: "smooth", block: "center" });
      },
      hidden ? "hidden" : "",
    );
    bar.style.setProperty("--bar", colors[type]);
    bar.style.flexGrow = String(Math.max(tokens, 1));
    bar.title = `${type.replace("_", " ")} · ${num(tokens)} tokens`;
    bar.setAttribute(
      "aria-label",
      `Jump to ${type.replace("_", " ")} message ${e.seq}, ${num(tokens)} tokens`,
    );
    map.append(bar);
    if (hidden) {
      continue;
    }
    const wrap = make("article", undefined, `event ${type}`);
    wrap.id = `event-${e.seq}`;
    const tag = make("div", zone !== lastZone ? zone.toUpperCase() : "", `zone-tag ${zone}`);
    lastZone = zone;
    const bubble = button("", () => openEvent(e.seq), "bubble");
    bubble.dataset.seq = e.seq;
    bubble.setAttribute("aria-label", `Read ${type.replace("_", " ")} message ${e.seq}`);
    bubble.append(make("div", undefined, "skeleton"), make("div", undefined, "skeleton"));
    wrap.append(tag, bubble);
    conv.append(wrap);
    if (eventCache.has(e.seq)) {
      fillBubble(bubble, eventCache.get(e.seq), e);
    } else {
      observer.observe(bubble);
    }
  }
  if (!overview.length) {
    conv.append(make("div", "No conversation messages were recorded.", "empty"));
  }
}
async function drainEvents() {
  while (loads < 4 && loadQueue.length) {
    const job = loadQueue.shift();
    loads++;
    api("/api/trace?" + qs({ id: job.id, start_seq: job.seq, limit: 1 }))
      .then((page) => {
        if (job.gen !== generation || scope?.id !== job.id) {
          return;
        }
        if (page.meta.revision !== trace.meta.revision) {
          throw new Error("This session changed. Select it again to refresh.");
        }
        const e = page.items[0];
        eventCache.set(job.seq, e);
        fillBubble(
          job.bubble,
          e,
          overview.find((x) => x.seq === job.seq),
        );
      })
      .catch((e) => {
        if (job.gen === generation) {
          job.bubble.replaceChildren(make("span", e.message, "muted"));
        }
      })
      .finally(() => {
        loads--;
        drainEvents();
      });
  }
}
function fillBubble(bubble, e, meta) {
  if (!e) {
    return;
  }
  const text = e.text_view || "(No text)",
    type = eventType(e);
  bubble.replaceChildren();
  if (type === "tool_call" || type === "reasoning" || type === "system") {
    bubble.append(
      make(
        "div",
        type === "tool_call"
          ? e.tool || "Tool call"
          : type === "system"
            ? "Recorded context"
            : "Recorded thinking",
        "event-label",
      ),
    );
  }
  const excerpt = text.length > 600 ? text.slice(0, 600) + "…" : text;
  bubble.append(make("div", excerpt));
  const foot = make("div", undefined, "bubble-meta");
  foot.append(
    make(
      "span",
      e.next_text_offset != null || text.length > 600 ? "Read full message" : "#" + e.seq,
    ),
  );
  let label = tokenMode
    ? `${num(meta?.tokens ?? e.tokens)} tokens`
    : ["user", "system"].includes(type)
      ? e.ts
        ? new Date(e.ts).toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" })
        : "Time unknown"
      : meta?.duration_seconds != null
        ? `~${duration(meta.duration_seconds)}`
        : "Duration unknown";
  foot.append(make("span", label));
  bubble.append(foot);
}
async function openEvent(seq) {
  const id = scope.id;
  const revision = trace.meta.revision;
  const view = showPreview(`Message ${seq} · ${trace.meta.title}`, () => renderTab());
  const load = async (offset = 0) => {
    const page = await api(
      "/api/trace?" + qs({ id, start_seq: seq, limit: 1, text_offset: offset }),
    );
    if (view.id !== previewId) {
      return;
    }
    if (page.meta.revision !== revision) {
      throw new Error("Session changed. Reopen it.");
    }
    const event = page.items[0];
    view.body.append(make("pre", event?.text_view || "(No text)"));
    if (event?.next_text_offset == null) {
      return;
    }
    const more = button("Read next part", async () => {
      more.disabled = true;
      try {
        await load(event.next_text_offset);
        more.remove();
      } catch (error) {
        more.disabled = false;
        view.body.append(notice(error.message, "error"));
      }
    });
    view.body.append(more);
  };
  try {
    await load();
  } catch (error) {
    view.body.append(notice(error.message, "error"));
  }
}
async function renderTab(gen = generation) {
  $("content").hidden = false;
  $("trace-area").hidden = true;
  $("filters").hidden = true;
  if (tab === "original") {
    if (scope.type === "trace") {
      $("content").hidden = true;
      $("trace-area").hidden = false;
      renderConversation();
    }
    return;
  }
  const content = $("content");
  content.replaceChildren();
  if (!analysis) {
    tab = "original";
    return;
  }
  const result = analysis.result || {};
  if (tab === "summary") {
    for (const [key, label, index] of [
      [
        "project_description",
        scope.type === "directory" ? "Project description" : "Session overview",
        "01",
      ],
      ["issues_summary", "Findings", "02"],
      ["suggested_global_fix", "Suggested fix", "03"],
    ]) {
      const section = make("section", undefined, "analysis-section"),
        title = make("div", undefined, "section-title");
      title.append(make("span", index, "num"), make("span", label));
      const card = make("div", undefined, "analysis-card");
      if (key === "suggested_global_fix") {
        card.append(copyButton(result[key] || ""));
      }
      card.append(document.createTextNode(result[key] || "No summary provided."));
      section.append(title, card);
      content.append(section);
    }
    if (result.limitations?.length) {
      content.append(notice(result.limitations.join(" ")));
    }
  }
  if (tab === "issues") {
    content.append(
      make("div", `${result.issues?.length || 0} opportunities to improve`, "section-title"),
    );
    if (!result.issues?.length) {
      const empty = make("div", undefined, "empty");
      empty.append(
        make("h2", "No actionable issues found"),
        make(
          "p",
          result.coverage === "partial"
            ? "This review covered part of the available evidence. Check the summary for coverage details."
            : "The analysis did not identify a concrete issue in the reviewed evidence.",
        ),
      );
      content.append(empty);
    }
    for (const issue of result.issues || []) {
      const card = make("section", undefined, "issue-card"),
        title = make("div", undefined, "issue-title");
      title.append(
        make("span", issue.severity, `severity ${issue.severity}`),
        make("h3", issue.title),
      );
      const fix = make("div", undefined, "fix");
      fix.append(
        copyButton(issue.suggested_fix),
        make("div", "Suggested fix", "label"),
        document.createTextNode(issue.suggested_fix),
      );
      card.append(
        title,
        make("p", issue.explanation),
        make("div", issue.evidence, "evidence mono"),
        fix,
      );
      content.append(card);
    }
  }
  if (tab === "chat") {
    await renderChat(gen);
  }
}
function renderRight() {
  const r = $("rightbar");
  r.replaceChildren();
  if (!scope) {
    r.append(
      make("span", "Analysis", "eyebrow"),
      make("h2", "Session review", "right-heading"),
      make("p", "Select a directory or session.", "right-intro"),
    );
    return;
  }
  if (analysis) {
    const nav = make("nav", undefined, "analysis-nav");
    const original = button("", () => switchTab("original"), tab === "original" ? "active" : "");
    original.append(
      icon(scope.type === "trace" ? "trace" : "folder"),
      make("span", scope.type === "trace" ? "Trace" : "Directory"),
    );
    nav.append(
      original,
      make("div", undefined, "nav-divider"),
      make("div", `${analysis.domain_name || "Domain"} analysis`, "label"),
    );
    for (const [id, label, ico] of [
      ["summary", "Summary", "summary"],
      ["issues", "Issues", "issues"],
      ["chat", "Chat", "chat"],
    ]) {
      const b = button("", () => switchTab(id), tab === id ? "active" : "");
      b.append(icon(ico), make("span", label));
      if (id === "issues") {
        b.append(make("span", String(analysis.result?.issues?.length || 0), "badge"));
      }
      nav.append(b);
    }
    r.append(nav);
    const stats = make("div", undefined, "analysis-stats");
    const complete = make("div", undefined, "status-complete");
    complete.append(
      icon("check"),
      make(
        "span",
        analysis.result?.coverage === "partial"
          ? "Review complete · partial coverage"
          : "Analysis complete",
      ),
    );
    stats.append(complete);
    for (const [label, value] of [
      ["Model", `${providers[analysis.ai_provider] || analysis.ai_provider} · ${analysis.model}`],
      ["Thinking", analysis.model_config?.thinking || "none"],
      ["Domain", analysis.domain_name || analysis.domain],
      [
        "Estimated cost",
        analysis.estimated_usd == null
          ? "Rates not configured"
          : `${money(analysis.estimated_usd)} – ${money(analysis.maximum_usd)}`,
      ],
      [
        "Recorded usage",
        `${num(analysis.usage?.input_tokens)} in · ${num(analysis.usage?.output_tokens)} out`,
      ],
      ["Calculated cost", analysis.cost_usd == null ? "Unknown" : money(analysis.cost_usd)],
    ]) {
      stats.append(make("div", label, "stats-label"), make("div", value, "stats-value"));
    }
    if (analysis.result_not_saved) {
      stats.append(notice("Result not saved. Keep this session open."));
    }
    stats.append(make("div", "Provider usage × configured rates. Not an invoice.", "help"));
    const again = button(
      "New analysis",
      () => {
        analysis = null;
        tab = "original";
        switchTab("original");
        renderRight();
      },
      "quiet",
    );
    again.style.cssText = "font-size:10px;margin-top:15px;padding-left:0";
    stats.append(again);
    r.append(stats);
    return;
  }
  r.append(
    make("div", "Analysis", "eyebrow"),
    make("h2", "Analysis setup", "right-heading"),
    make(
      "p",
      scope.type === "trace" ? "Review this session." : "Review this directory.",
      "right-intro",
    ),
  );
  const domainBlock = make("div", undefined, "control-block"),
    label = make("label", "01  Domain", "label"),
    select = make("select");
  select.id = "analysis-domain";
  label.htmlFor = select.id;
  select.append(new Option("Detect automatically", "auto"));
  for (const d of Object.values(domains)) {
    select.append(new Option(d.name, d.id));
  }
  select.value = domain;
  select.onchange = () => {
    domain = select.value;
    renderRight();
  };
  domainBlock.append(
    label,
    select,
    make(
      "div",
      domain === "auto"
        ? "One extra Decisions call · OpenAI key required"
        : "Uses the selected domain.",
      "help",
    ),
  );
  r.append(domainBlock);
  const modelBlock = make("div", undefined, "control-block");
  modelBlock.append(make("div", "02  Analysis model", "label"));
  modelBlock.append(modelMenu());
  r.append(modelBlock);
  const note = make("div", undefined, "notice");
  note.style.cssText =
    "margin-top:5px;font-size:10px;background:#e8f0e8;border-color:#dce7dc;color:#7d9481";
  note.textContent = "Selected evidence is sent to this provider.";
  r.append(note);
  const bottom = make("div", undefined, "right-bottom"),
    go = button(activeJob ? "Analyzing…" : "Estimate analysis", prepareAnalysis, "primary analyze");
  go.disabled = !!activeJob || (scope.type === "directory" && !scope.cwd);
  go.prepend(icon(activeJob ? "refresh" : "summary"));
  go.style.display = "flex";
  go.style.justifyContent = "center";
  go.style.gap = "8px";
  bottom.append(
    go,
    make(
      "div",
      model?.pricing_known
        ? "A local estimate first. You decide when to start."
        : "Set model rates in Settings to show a cost estimate.",
      "analysis-note",
    ),
  );
  if (activeJob) {
    bottom.append(make("div", "Read-only analysis is running. You can keep browsing.", "help"));
  }
  r.append(bottom);
}
async function switchTab(next) {
  previewId++;
  tab = next;
  renderRight();
  if (next === "original" && scope.type === "directory") {
    try {
      await renderDirectory();
    } catch (e) {
      fail(e);
    }
  }
  await renderTab();
}
// Estimates stay inline; starting requires a deliberate click on the saved quote.
async function prepareAnalysis() {
  if (estimating || activeJob) {
    return;
  }
  estimating = true;
  quote = null;
  const selected = scopeKey();
  const origin = { ...scope };
  const block = make("section", undefined, "estimate");
  block.setAttribute("aria-live", "polite");
  block.append(make("p", "Calculating estimate…"));
  $("rightbar").append(block);
  const body = {
    domain,
    model_config: clone(model),
    ...(scope.type === "trace" ? { trace_ids: [scope.id] } : { cwd: scope.cwd }),
  };
  try {
    const result = await api("/api/estimate", body);
    if (selected !== scopeKey() || !block.isConnected) {
      return;
    }
    quote = result;
    block.replaceChildren(
      make("h3", "Analysis estimate"),
      make(
        "p",
        `${num(result.tokens_estimated)} input tokens · ${result.maximum_usd == null ? "Cost unknown" : money(result.maximum_usd) + " maximum"}`,
      ),
    );
    block.append(make("p", result.estimate_note, "help"));
    const ready =
      settings.providers.find((p) => p.id === result.ai_provider)?.key_available &&
      (domain !== "auto" || settings.providers.find((p) => p.id === "openai")?.key_available);
    const start = button(
      "Start analysis",
      async () => {
        if (!quote || selected !== scopeKey()) {
          return;
        }
        start.disabled = true;
        try {
          const job = await api("/api/analyze", { quote_id: result.id });
          quote = null;
          activeJob = job.job_id;
          renderRight();
          await watchJob(job.job_id, origin);
        } catch (error) {
          block.append(notice(error.message, "error"));
          start.disabled = false;
        }
      },
      "primary",
    );
    start.disabled = !ready;
    block.append(start);
    if (!ready) {
      block.append(notice("Add the required API key in Settings."));
    }
  } catch (error) {
    if (block.isConnected) {
      block.replaceChildren(notice(error.message, "error"));
    }
  } finally {
    estimating = false;
  }
}
async function watchJob(id, origin, kind = JobKind.ANALYSIS) {
  try {
    for (;;) {
      const job = await api("/api/job?id=" + id);
      if (!["queued", "running"].includes(job.state)) {
        if (job.state !== "completed") {
          const text =
            job.error || job.results?.find((r) => r.error)?.error || "Analysis was interrupted.";
          fail(new Error(text));
        }
        if (kind === JobKind.CHAT) {
          chatBusy = false;
          if (scopeKey(origin) === scopeKey() && tab === "chat") {
            await renderChat();
          }
        } else {
          activeJob = null;
          if (scopeKey(origin) === scopeKey()) {
            await renderSelection();
            if (job.state === "completed") {
              tab = "summary";
              renderRight();
              await renderTab();
            }
          }
          await refreshState();
        }
        return;
      }
      await new Promise((resolve) => setTimeout(resolve, 1000));
    }
  } catch (e) {
    fail(e);
  } finally {
    if (kind === JobKind.CHAT) {
      chatBusy = false;
    } else {
      activeJob = null;
    }
    renderRight();
  }
}
async function renderChat(gen = generation) {
  const content = $("content");
  content.replaceChildren();
  const shell = make("div", undefined, "chat-content"),
    toolbar = make("div", undefined, "chat-toolbar"),
    saved = make("select");
  saved.setAttribute("aria-label", "Saved chats");
  saved.append(new Option("Current conversation", ""));
  const fresh = button("New chat", () => {
    chatId = null;
    renderChat();
  });
  toolbar.append(saved, fresh);
  const messages = make("div", undefined, "chat-messages");
  messages.id = "chat-messages";
  shell.append(toolbar, messages);
  const form = make("form", undefined, "chat-form"),
    input = make("textarea");
  input.placeholder = "Ask about this analysis…";
  input.setAttribute("aria-label", "Your question");
  input.rows = 2;
  input.maxLength = 8000;
  const send = make("button", chatBusy ? "Thinking…" : "Send", "primary");
  send.type = "submit";
  send.disabled = chatBusy;
  form.append(input, send);
  shell.append(
    form,
    make(
      "div",
      "Questions use this selected item and its saved analysis. Follow-up model usage is billed separately.",
      "help",
    ),
  );
  content.append(shell);
  let current = null;
  try {
    const all = await api("/api/chats");
    if (gen !== generation) {
      return;
    }
    const matching = all.items.filter((c) => scopeKey(c.scope) === scopeKey());
    for (const c of matching.slice().reverse()) {
      saved.append(new Option(date(c.updated_at || c.created_at), c.id));
    }
    if (chatId) {
      saved.value = chatId;
      current = await api("/api/chat?id=" + chatId);
      if (gen !== generation) {
        return;
      }
      for (const m of current.messages) {
        const row = make("div", m.text, `chat-message ${m.role}`);
        if (!m.completed) {
          row.append(make("div", "Pending…", "help"));
        }
        messages.append(row);
      }
    }
    if (!current?.messages?.length) {
      const empty = make("div", undefined, "empty");
      empty.append(
        make("h2", "Ask about this analysis"),
        make("p", "Ask about an issue, fix, or source."),
      );
      messages.append(empty);
    }
    messages.scrollTop = messages.scrollHeight;
  } catch (e) {
    messages.append(notice(e.message, "error"));
  }
  saved.onchange = () => {
    chatId = saved.value || null;
    renderChat();
  };
  form.onsubmit = async (e) => {
    e.preventDefault();
    const text = input.value.trim();
    if (!text || chatBusy) {
      return;
    }
    chatBusy = true;
    send.disabled = true;
    send.textContent = "Thinking…";
    const origin = { ...scope };
    try {
      if (!chatId) {
        const c = await api("/api/chat", { scope });
        chatId = c.chat_id;
      }
      const job = await api("/api/chat/message", { chat_id: chatId, text });
      input.value = "";
      await renderChat();
      watchJob(job.job_id, origin, JobKind.CHAT);
    } catch (err) {
      chatBusy = false;
      send.disabled = false;
      send.textContent = "Send";
      fail(err);
    }
  };
  input.onkeydown = (e) => {
    if (e.key === "Enter" && (e.metaKey || e.ctrlKey)) {
      e.preventDefault();
      form.requestSubmit();
    }
  };
}
function clone(v) {
  return JSON.parse(JSON.stringify(v));
}
function providerSelect(value) {
  const select = make("select");
  for (const [id, label] of Object.entries(providers)) {
    select.append(new Option(label, id));
  }
  select.value = value;
  return select;
}
function numericField(parent, cfg, key, label) {
  const wrap = make("label", label),
    input = make("input");
  input.type = "number";
  input.min = "0";
  input.step = key.endsWith("rate") ? "0.001" : "1";
  input.value = cfg[key] || 0;
  input.oninput = () => (cfg[key] = Number(input.value));
  wrap.append(input);
  parent.append(wrap);
  return input;
}
function limitsForm(cfg) {
  const box = make("details");
  box.style.marginTop = "12px";
  const summary = make("summary", "Context limits & pricing");
  summary.style.cssText = "font-size:10px;color:#8a9b8e;cursor:pointer;margin-bottom:12px";
  box.append(summary);
  const limits = make("div", undefined, "limit-fields");
  numericField(limits, cfg, "input_limit", "Input limit");
  numericField(limits, cfg, "context_limit", "Context window");
  numericField(limits, cfg, "output_limit", "Output reserve");
  numericField(limits, cfg, "input_rate", "Input $ / 1M");
  numericField(limits, cfg, "output_rate", "Output $ / 1M");
  box.append(limits);
  const row = make("label", undefined, "row"),
    check = make("input");
  check.type = "checkbox";
  check.checked = cfg.pricing_known;
  check.onchange = () => (cfg.pricing_known = check.checked);
  row.style.cssText = "font-size:10px;color:#8a9b8e;margin-top:12px";
  row.append(check, document.createTextNode("I have configured rates for this model"));
  box.append(
    row,
    make(
      "div",
      "Use the selected provider’s documented limits and rates. Cached and provider-specific charges may remain unresolved.",
      "help",
    ),
  );
  return box;
}
function simpleModelForm(cfg, label, role = ModelRole.REVIEW) {
  const section = make("section", undefined, "settings-section");
  section.append(make("h3", label));
  const fields = make("div", undefined, "model-fields"),
    pl = make("label", "Provider"),
    ml = make("label", "Model"),
    tl = make("label", "Thinking"),
    p = providerSelect(cfg.provider),
    m = make("input"),
    t = make("select");
  m.value = cfg.model;
  m.setAttribute("aria-label", label + " model");
  for (const v of THINKING_LEVELS) {
    t.append(
      new Option(
        v === "default"
          ? "Model default"
          : v === "none"
            ? "None"
            : v === "xhigh"
              ? "Extra high"
              : v[0].toUpperCase() + v.slice(1),
        v,
      ),
    );
  }
  t.value = cfg.thinking || "none";
  p.onchange = () => {
    cfg.provider = p.value;
    cfg.model = "";
    m.value = "";
    cfg.pricing_known = false;
  };
  m.oninput = () => {
    cfg.model = m.value;
    cfg.pricing_known = false;
  };
  t.onchange = () => (cfg.thinking = t.value);
  if (role === ModelRole.DETECTION) {
    p.disabled = true;
    m.readOnly = true;
    t.disabled = true;
    section.append(
      make(
        "div",
        "Optional classification uses OpenAI Decisions with gpt-6-luna. It is never used to analyze traces.",
        "help",
      ),
    );
  }
  pl.append(p);
  ml.append(m);
  tl.append(t);
  fields.append(pl, ml, tl);
  section.append(fields, limitsForm(cfg));
  return section;
}
async function openSettings() {
  try {
    settings = await api("/api/settings");
    settingsDraft = clone(settings);
    settingsTab = "providers";
    $("settings-status").textContent = "";
    renderSettings();
    $("settings-dialog").showModal();
  } catch (e) {
    fail(e);
  }
}
function renderSettings() {
  $("providers-tab").classList.toggle("active", settingsTab === "providers");
  $("domains-tab").classList.toggle("active", settingsTab === "domains");
  const main = $("settings-main");
  main.replaceChildren();
  if (settingsTab === "providers") {
    main.append(
      simpleModelForm(settingsDraft.chat, "Default model for analysis & chat"),
      simpleModelForm(settingsDraft.decision, "Optional domain detection", ModelRole.DETECTION),
    );
    const detection = make("div", undefined, "limit-fields");
    numericField(detection, settingsDraft, "decision_input_usd_per_m", "Detection input $ / 1M");
    main.append(detection);
    const line = make("div", undefined, "nav-divider");
    main.append(line, make("div", "API keys", "label"));
    for (const [id, label] of Object.entries(providers)) {
      const row = make("div", undefined, "key-row"),
        input = make("input");
      input.type = "password";
      input.autocomplete = "off";
      input.setAttribute("aria-label", label + " API key");
      input.placeholder = settings.providers.find((p) => p.id === id)?.key_available
        ? "••••••••  configured"
        : "Enter API key";
      const save = button("Set", async () => {
        save.disabled = true;
        try {
          settings = await api("/api/provider", { provider: id, key: input.value });
          input.value = "";
          input.placeholder = settings.providers.find((p) => p.id === id)?.key_available
            ? "••••••••  configured"
            : "Enter API key";
          $("settings-status").textContent = label + " key updated for this running session.";
        } catch (e) {
          $("settings-status").textContent = e.message;
        } finally {
          save.disabled = false;
        }
      });
      row.append(make("label", label), input, save);
      main.append(row);
    }
    main.append(
      make(
        "div",
        "Keys stay in server memory until restart. Use environment variables for persistent configuration.",
        "help",
      ),
    );
  } else {
    const header = make("div", undefined, "domain-heading"),
      select = make("select");
    select.setAttribute("aria-label", "Domain to edit");
    for (const d of Object.values(domains)) {
      select.append(new Option(d.name, d.id));
    }
    if (!domainDraft) {
      domainDraft = clone(domains["software-engineering"] || Object.values(domains)[0]);
    }
    if (domainDraft?.id) {
      select.value = domainDraft.id;
    }
    select.onchange = () => {
      domainDraft = clone(domains[select.value]);
      renderSettings();
    };
    header.append(
      select,
      button("+ New domain", () => {
        domainDraft = { id: "", name: "", detection_prompt: "", analysis_prompt: "" };
        renderSettings();
      }),
    );
    main.append(header);
    const form = make("div", undefined, "domain-form");
    for (const [key, label] of [
      ["name", "Domain name"],
      ["id", "Identifier"],
    ]) {
      const l = make("label", label, "label"),
        input = make("input");
      input.value = domainDraft[key];
      input.setAttribute("aria-label", label);
      input.placeholder = key === "id" ? "e.g. product-design" : "e.g. Product Design";
      if (key === "id" && domains[domainDraft.id]) {
        input.readOnly = true;
      }
      input.oninput = () => (domainDraft[key] = input.value);
      form.append(l, input);
    }
    for (const [key, label, rows] of [
      ["detection_prompt", "Domain detection prompt", 3],
      ["analysis_prompt", "Domain analysis prompt", 10],
    ]) {
      const l = make("label", label, "label"),
        input = make("textarea");
      input.rows = rows;
      input.value = domainDraft[key];
      input.setAttribute("aria-label", label);
      input.oninput = () => (domainDraft[key] = input.value);
      form.append(l, input);
    }
    form.append(
      make(
        "div",
        "Saved as an editable .prompt file. The domain prompt is inserted into the trace or directory analysis template.",
        "help",
      ),
    );
    main.append(form);
  }
}
$("providers-tab").onclick = () => {
  settingsTab = "providers";
  renderSettings();
};
$("domains-tab").onclick = () => {
  settingsTab = "domains";
  renderSettings();
};
$("save-settings").onclick = async () => {
  const b = $("save-settings");
  b.disabled = true;
  try {
    if (settingsTab === "domains") {
      domains = await api("/api/domains", domainDraft);
      $("settings-status").textContent = "Domain saved.";
      renderSettings();
    } else {
      settings = await api("/api/settings", settingsDraft);
      model = clone(settings.chat);
      $("settings-status").textContent = "Defaults saved.";
    }
    renderRight();
  } catch (e) {
    $("settings-status").textContent = e.message;
  } finally {
    b.disabled = false;
  }
};
// A disclosure hierarchy works with touch and keyboard without opening a window.
function modelMenu() {
  const menu = make("details", undefined, "model-menu");
  const summary = make(
    "summary",
    `${providers[model.provider]} / ${model.model} / ${model.thinking || "default"}`,
  );
  menu.append(summary);
  for (const [id, label] of Object.entries(providers)) {
    const provider = make("details", undefined, "provider-menu");
    const title = make("summary", label);
    const list = make("div", undefined, "model-list");
    provider.append(title, list);
    const fill = () => {
      list.replaceChildren();
      const known = new Map((modelCatalog.get(id) || []).map((item) => [item.id, item]));
      for (const config of [settings.chat, model]) {
        if (config.provider === id && config.model && !known.has(config.model)) {
          known.set(config.model, config);
        }
      }
      for (const item of [...known.values()].sort((a, b) => a.id.localeCompare(b.id))) {
        const level = make("details", undefined, "thinking-menu");
        level.append(make("summary", item.id));
        for (const effort of THINKING_LEVELS) {
          const pick = button(
            effort,
            () => {
              const previous = modelChoices.get(`${id}:${item.id}`);
              const same = model.provider === id && model.model === item.id;
              modelChoices.set(`${model.provider}:${model.model}`, clone(model));
              model = same
                ? { ...model, thinking: effort }
                : {
                    ...(previous || item),
                    provider: id,
                    model: item.id,
                    thinking: effort,
                    input_limit:
                      previous?.input_limit || item.input_limit || item.context_limit || 0,
                    context_limit: previous?.context_limit || item.context_limit || 0,
                    output_limit: previous?.output_limit || settings.chat.output_limit,
                    pricing_known: previous?.pricing_known || false,
                  };
              quote = null;
              renderRight();
            },
            model.provider === id && model.model === item.id && model.thinking === effort
              ? "selected"
              : "",
          );
          pick.setAttribute("aria-label", `${label} / ${item.id} / ${effort}`);
          level.append(pick);
        }
        list.append(level);
      }
      const load = button("Load models", async () => {
        load.disabled = true;
        try {
          const result = await api("/api/models?provider=" + id);
          modelCatalog.set(id, result.models);
          fill();
        } catch (error) {
          list.append(notice(error.message, "error"));
          load.disabled = false;
        }
      });
      list.append(load);
    };
    fill();
    menu.append(provider);
  }
  const custom = make("details", undefined, "custom-model");
  custom.append(make("summary", "Custom model & limits"));
  const provider = providerSelect(model.provider);
  provider.setAttribute("aria-label", "Custom model provider");
  const name = make("input");
  name.value = model.model;
  name.placeholder = "Model ID";
  name.setAttribute("aria-label", "Custom model ID");
  const draft = clone(model);
  provider.onchange = () => {
    draft.provider = provider.value;
    draft.model = "";
    name.value = "";
    draft.context_limit = 0;
    draft.input_limit = 0;
    draft.pricing_known = false;
  };
  name.oninput = () => {
    draft.model = name.value.trim();
    draft.pricing_known = false;
  };
  custom.append(
    provider,
    name,
    limitsForm(draft),
    button("Apply model", () => {
      if (
        !draft.model ||
        draft.input_limit <= FRAMING_TOKENS ||
        draft.context_limit <= draft.output_limit + FRAMING_TOKENS
      ) {
        fail(new Error("Set the model ID and context limits."));
        return;
      }
      model = clone(draft);
      quote = null;
      renderRight();
    }),
  );
  menu.append(custom);
  return menu;
}
$("settings").append(icon("settings"), document.createTextNode("Settings"));
$("settings").onclick = openSettings;
$("search").oninput = renderNav;
$("toggle-sessions").onclick = () => {
  const open = document.querySelector(".sidebar").classList.toggle("nav-open");
  $("toggle-sessions").setAttribute("aria-expanded", String(open));
};
for (const b of document.querySelectorAll("[data-close]")) {
  b.onclick = () => $(b.dataset.close).close();
}
for (const d of document.querySelectorAll("dialog")) {
  d.addEventListener("click", (e) => {
    if (e.target === d) {
      const r = d.getBoundingClientRect();
      if (e.clientX < r.left || e.clientX > r.right || e.clientY < r.top || e.clientY > r.bottom) {
        d.close();
      }
    }
  });
}
async function refreshState() {
  if (polling) {
    return;
  }
  polling = true;
  try {
    const next = await api("/api/state"),
      first = !state;
    const changed = state?.catalog_revision !== next.catalog_revision;
    state = next;
    $("local-status").textContent = next.ingestion_running
      ? "Importing sessions…"
      : next.ai_running
        ? "Working locally · AI active"
        : "Local workspace";
    if (changed) {
      renderNav();
      // Index renames refresh the heading without resetting the reader's position.
      const selected = next.traces.find((item) => item.trace_id === scope?.id);
      if (scope?.type === "trace" && trace && selected && trace.meta.title !== selected.title) {
        trace.meta.title = selected.title;
        renderTraceHeader();
      }
    }
    if (first) {
      const group = next.groups?.[0];
      if (group) {
        expanded.add(group.group === "Unknown directory" ? "" : group.group);
        const selected = next.traces[0];
        await selectScope({ type: "trace", id: selected.trace_id });
      } else {
        renderWelcome();
      }
      const running = next.recent_jobs?.find((j) => ["running", "queued"].includes(j.state));
      if (running) {
        activeJob = running.id;
        watchJob(running.id, null);
      }
    }
  } catch (e) {
    fail(e);
  } finally {
    polling = false;
  }
}
(async () => {
  try {
    [settings, domains] = await Promise.all([api("/api/settings"), api("/api/domains")]);
    model = clone(settings.chat);
    await refreshState();
  } catch (e) {
    fail(e);
    renderWelcome();
  }
})();
setInterval(() => {
  if (!document.hidden) {
    refreshState();
  }
}, POLL_MS);
