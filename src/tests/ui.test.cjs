const { test, before, after } = require("node:test");
const assert = require("node:assert/strict");
const { chromium } = require("playwright");
const { spawn } = require("node:child_process");
const { once } = require("node:events");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const ROOT = path.resolve(__dirname, "..");
const WIDTHS = [320, 390, 768, 1024, 1440];
const HTTP_OK = 200;
let temporary, server, browser, url;

before(async () => {
  temporary = fs.mkdtempSync(path.join(os.tmpdir(), "symbiosic-ui-"));
  const project = path.join(temporary, "sample project");
  fs.mkdirSync(project);
  fs.writeFileSync(path.join(project, "example.txt"), "Inline file preview.\n");
  for (const provider of ["claude", "codex"]) {
    const directory = path.join(temporary, provider);
    fs.mkdirSync(directory);
    const text = fs.readFileSync(path.join(ROOT, "tests/fixtures", `${provider}.jsonl`), "utf8");
    fs.writeFileSync(
      path.join(directory, "session.jsonl"),
      text.replaceAll("/work//app/./src/..", project).replaceAll("/work/app", project),
    );
  }
  server = spawn(path.join(ROOT, "symbiosic"), ["--headless"], {
    cwd: ROOT,
    env: {
      ...process.env,
      OPENAI_API_KEY: "",
      ANTHROPIC_API_KEY: "",
      OPENROUTER_API_KEY: "",
      GEMINI_API_KEY: "",
      DATA_DIR: path.join(temporary, "data"),
      CLAUDE_ROOT: path.join(temporary, "claude"),
      CODEX_ROOT: path.join(temporary, "codex"),
      PORT: "14317",
      POLL_MS: "100",
    },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stderr = "";
  server.stderr.on("data", (data) => {
    stderr += data;
  });
  url = (await once(server.stdout, "data"))[0].toString().trim();
  assert.match(url, /^http:\/\/127\.0\.0\.1:/, stderr);
  browser = await chromium.launch({
    headless: true,
    ...(process.env.CHROME_PATH ? { executablePath: process.env.CHROME_PATH } : {}),
  });
});
after(async () => {
  if (browser) {
    await browser.close();
  }
  if (server && server.exitCode === null) {
    server.kill("SIGTERM");
    await once(server, "exit");
  }
  if (temporary) {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
});
async function open(width = 1440) {
  const page = await browser.newPage({ viewport: { width, height: 900 } });
  const errors = [];
  page.on("pageerror", (error) => {
    errors.push(error.message);
  });
  await page.goto(url);
  await page.locator(".trace-header h1").waitFor();
  if (process.env.SCREENSHOT_DIR && [390, 1440].includes(width)) {
    fs.mkdirSync(process.env.SCREENSHOT_DIR, { recursive: true });
    await page.screenshot({
      path: path.join(process.env.SCREENSHOT_DIR, `workspace-${width}.png`),
      fullPage: true,
    });
  }
  return { page, errors };
}
test("previews use the workspace; Settings is the only dialog", async () => {
  const { page, errors } = await open();
  assert.equal(await page.locator("dialog").count(), 1);
  await page.locator(".directory-button").first().click();
  await page.getByRole("button", { name: "example.txt", exact: true }).click();
  await page.getByText("Inline file preview.", { exact: true }).waitFor();
  assert.equal(await page.locator("dialog[open]").count(), 0);
  await page.getByRole("button", { name: "← Back", exact: true }).click();
  await page.getByRole("button", { name: "example.txt", exact: true }).waitFor();
  await page.locator(".session").first().click();
  await page.locator(".bubble").first().click();
  await page.locator(".preview-body pre").waitFor();
  assert.equal(await page.locator("dialog[open]").count(), 0);
  await page.getByRole("button", { name: "← Back", exact: true }).click();
  await page.locator(".bubble").first().waitFor();
  assert.deepEqual(errors, []);
  await page.close();
});
test("native title changes refresh the selected heading", async () => {
  const { page, errors } = await open();
  await page
    .locator(".session")
    .filter({ has: page.locator(".badge.codex") })
    .click();
  await page.waitForFunction(
    () =>
      document.querySelector(".trace-header h1")?.textContent === "Fix the failing parser test.",
  );
  fs.writeFileSync(
    path.join(temporary, "session_index.jsonl"),
    JSON.stringify({ id: "codex-session", thread_name: "Renamed native session" }),
  );
  await page.waitForFunction(
    () =>
      document.querySelector(".trace-header h1")?.textContent === "Renamed native session" &&
      document.querySelector(".session.active .session-title")?.textContent ===
        "Renamed native session",
  );
  assert.equal(
    await page.locator(".session.active .session-title").innerText(),
    "Renamed native session",
  );
  fs.unlinkSync(path.join(temporary, "session_index.jsonl"));
  assert.deepEqual(errors, []);
  await page.close();
});

test("provider → model → thinking selection and inline estimate", async () => {
  const { page, errors } = await open();
  let paidCalls = 0;
  page.on("request", (request) => {
    if (request.url().endsWith("/api/analyze")) {
      paidCalls++;
    }
  });
  await page.route("**/api/models?provider=openrouter", (route) =>
    route.fulfill({
      status: HTTP_OK,
      contentType: "application/json",
      body: JSON.stringify({
        models: [{ id: "fixture/model", input_limit: 100000, context_limit: 100000 }],
      }),
    }),
  );
  await page.locator(".model-menu > summary").click();
  const provider = page
    .locator(".provider-menu")
    .filter({ has: page.locator("summary").getByText("OpenRouter", { exact: true }) });
  await provider.locator(":scope > summary").click();
  await provider.getByRole("button", { name: "Load models", exact: true }).click();
  await provider.locator(".thinking-menu > summary").click();
  await provider
    .getByRole("button", { name: "OpenRouter / fixture/model / high", exact: true })
    .click();
  assert.match(
    await page.locator(".model-menu > summary").innerText(),
    /OpenRouter \/ fixture\/model \/ high/,
  );
  const estimate = page.waitForRequest((request) => request.url().endsWith("/api/estimate"));
  await page.getByRole("button", { name: "Estimate analysis", exact: true }).click();
  const request = (await estimate).postDataJSON();
  assert.equal(request.model_config.provider, "openrouter");
  assert.equal(request.model_config.thinking, "high");
  await page.getByRole("heading", { name: "Analysis estimate", exact: true }).waitFor();
  assert.equal(await page.locator("dialog[open]").count(), 0);
  assert.equal(paidCalls, 0);
  assert.equal(
    await page.getByRole("button", { name: "Start analysis", exact: true }).isDisabled(),
    true,
  );
  assert.deepEqual(errors, []);
  await page.close();
});
test("small layouts preserve navigation and reachable settings", async () => {
  for (const width of WIDTHS) {
    const { page, errors } = await open(width);
    assert.equal(
      await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth),
      true,
      `No horizontal overflow at ${width}px`,
    );
    if (width <= 640) {
      await page.getByRole("button", { name: "Sessions", exact: true }).click();
      await page.getByRole("textbox", { name: "Search sessions", exact: true }).fill("parser");
      assert.ok(await page.locator(".session:visible").count());
      await page.locator(".session:visible").first().click();
      assert.equal(
        await page
          .getByRole("button", { name: "Sessions", exact: true })
          .getAttribute("aria-expanded"),
        "false",
      );
    }
    await page.getByRole("button", { name: "Settings", exact: true }).click();
    await page.locator("#settings-dialog[open]").waitFor();
    assert.equal(
      await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth),
      true,
      `Settings fits ${width}px`,
    );
    await page.getByRole("button", { name: "Close settings", exact: true }).click();
    assert.deepEqual(errors, []);
    await page.close();
  }
});
