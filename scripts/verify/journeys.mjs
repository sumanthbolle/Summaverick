/**
 * The two journeys a visitor can actually complete: sending a message, and
 * asking the research tool a question. Both are checked against what the page
 * tells the visitor, not just the HTTP status underneath.
 *
 *   node scripts/verify/journeys.mjs [origin]
 *
 * The research checks read live ServiceNow documentation, so they need network
 * access and take a while.
 */
import { evaluate, goto, launch, setReducedMotion, sleep } from "./cdp.mjs";

const ORIGIN = process.argv[2] ?? "http://localhost:8788";

const results = [];
const check = (name, pass, detail = "") => {
  results.push({ name, pass, detail });
  console.log(`${pass ? "PASS" : "FAIL"}  ${name}${detail ? `\n        ${detail}` : ""}`);
};

const fill = (client, values) =>
  evaluate(
    client,
    `const f = document.querySelector("[data-lead-form]");
     const v = ${JSON.stringify(values)};
     for (const [name, value] of Object.entries(v)) {
       const el = f.elements[name];
       el.value = value;
       el.dispatchEvent(new Event("input", { bubbles: true }));
     }
     return 1;`
  );

const submit = (client) =>
  evaluate(
    client,
    `document.querySelector("[data-lead-submit]").click(); return 1;`
  );

const formState = (client) =>
  evaluate(
    client,
    `const f = document.querySelector("[data-lead-form]");
     const vis = (el) => {
       if (!el) return false;
       const r = el.getBoundingClientRect();
       return r.height > 0 && getComputedStyle(el).display !== "none";
     };
     return {
       status: document.querySelector("[data-lead-status]").textContent.trim(),
       submitLabel: document.querySelector("[data-lead-submit]").textContent.trim(),
       retryVisible: vis(document.querySelector("[data-lead-retry]")),
       emailError: document.querySelector('[data-error-for="email"]').textContent.trim(),
       messageError: document.querySelector('[data-error-for="message"]').textContent.trim(),
       emailInvalid: f.elements.email.getAttribute("aria-invalid"),
       values: { name: f.elements.name.value, email: f.elements.email.value, message: f.elements.message.value },
       statusLive: document.querySelector("[data-lead-status]").getAttribute("aria-live"),
     };`
  );

const { proc, client } = await launch({ headless: true, width: 1440, height: 1000 });

try {
  await setReducedMotion(client, false);
  await client.send("Network.enable");
  // The rate limiter buckets by CF-Connecting-IP. A run gets its own bucket so
  // repeated verification does not spend the allowance a real visitor would use,
  // and so a previous run cannot make this one look broken.
  await client.send("Network.setExtraHTTPHeaders", {
    headers: {
      "cf-connecting-ip": `198.51.100.${(Date.now() % 250) + 1}`,
    },
  });

  // ---- Contact: client-side validation keeps the visitor in place ---------
  await goto(client, `${ORIGIN}/`);
  await sleep(600);
  await fill(client, { name: "Priya Raman", email: "", message: "" });
  await submit(client);
  await sleep(300);
  const invalid = await formState(client);
  check(
    "contact: an empty email and message are named on the fields, and the typed name survives",
    invalid.emailError.length > 0 &&
      invalid.messageError.length > 0 &&
      invalid.values.name === "Priya Raman" &&
      invalid.retryVisible === false,
    JSON.stringify(invalid)
  );

  // ---- Contact: a real send, reported from the server's answer ------------
  const unique = `verify+${Date.now()}@example.com`;
  await fill(client, {
    name: "Priya Raman",
    email: unique,
    organisation: "Northwind Ops",
    message: "Request intake is spread across email and spreadsheets.",
  });
  await submit(client);
  await sleep(1200);
  const sent = await formState(client);
  check(
    "contact: a confirmed send says so, clears the form and offers no retry",
    sent.status === "Thanks—your message has been received." &&
      sent.retryVisible === false &&
      sent.values.email === "" &&
      sent.submitLabel === "Send your message",
    JSON.stringify(sent)
  );
  check(
    "contact: the status line is announced politely",
    sent.statusLive === "polite",
    `aria-live=${sent.statusLive}`
  );

  // ---- Contact: a failure keeps the input and offers a retry --------------
  await goto(client, `${ORIGIN}/`);
  await sleep(600);
  await client.send("Fetch.enable", {
    patterns: [{ urlPattern: "*/api/contact", requestStage: "Request" }],
  });
  const failOnce = new Promise((resolve) => {
    client.on("Fetch.requestPaused", async ({ requestId }) => {
      await client.send("Fetch.failRequest", { requestId, errorReason: "ConnectionFailed" });
      resolve();
    });
  });
  const keptValues = {
    name: "Priya Raman",
    email: "priya@example.com",
    message: "Request intake is spread across email and spreadsheets.",
  };
  await fill(client, keptValues);
  await submit(client);
  await failOnce;
  await sleep(600);
  const failed = await formState(client);
  check(
    "contact: a request that never completes is reported as unconfirmed, with the details kept and a retry offered",
    failed.status ===
      "We couldn't confirm whether your message was received. Your details are still here." &&
      failed.retryVisible === true &&
      failed.values.email === keptValues.email &&
      failed.values.message === keptValues.message,
    JSON.stringify(failed)
  );

  // Letting the retry through must not create a second message.
  await client.send("Fetch.disable");
  const before = await countLeads();
  await evaluate(client, `document.querySelector("[data-lead-retry]").click(); return 1;`);
  await sleep(1400);
  const retried = await formState(client);
  const after = await countLeads();
  check(
    "contact: the retry goes through and stores exactly one message",
    retried.status === "Thanks—your message has been received." && after === before + 1,
    `status="${retried.status}" leads ${before} -> ${after}`
  );

  // ---- Research tool: the mode is stated before anything is asked ---------
  await goto(client, `${ORIGIN}/ask`);
  await sleep(1200);
  const askIntro = await evaluate(
    client,
    `const mode = document.querySelector("[data-ask-mode]");
     return {
       mode: mode ? mode.textContent.replace(/\\s+/g, " ").trim() : null,
       samples: [...document.querySelectorAll("[data-ask-chips] .chip")].map(b => b.textContent.trim()),
       scopeMentionsServiceNow: /servicenow/i.test(document.body.textContent),
       hasVerifiedBadge: /\\bverified\\b/i.test(document.body.textContent),
     };`
  );
  check(
    "research: the page states which mode it is running in before a question is asked",
    !!askIntro.mode && askIntro.mode.length > 10,
    `mode banner: "${askIntro.mode}"`
  );
  check(
    "research: no blanket 'verified' claim, and the ServiceNow-only scope is stated",
    askIntro.hasVerifiedBadge === false && askIntro.scopeMentionsServiceNow === true,
    `verifiedBadge=${askIntro.hasVerifiedBadge} samples=${askIntro.samples.length}`
  );

  // ---- Research tool: every advertised sample returns something useful -----
  for (const [i, sample] of askIntro.samples.entries()) {
    await goto(client, `${ORIGIN}/ask`);
    await sleep(900);
    await evaluate(
      client,
      `document.querySelectorAll("[data-ask-chips] .chip")[${i}].click(); return 1;`
    );
    const outcome = await waitForAnswer(client, 75000);
    const useful =
      outcome.sources > 0 ||
      /only covers servicenow|not enough|no .*(?:sources|documentation)|unavailable|try again/i.test(
        outcome.text
      );
    const clean = !/^---|front ?matter|^title:|versions:/im.test(outcome.text);
    check(
      `research sample ${i + 1}: "${sample.slice(0, 48)}" returns a useful result or an honest limit`,
      useful && clean && outcome.settled,
      `sources=${outcome.sources} mode=${outcome.mode} text="${outcome.text.slice(0, 120).replace(/\s+/g, " ")}"`
    );
  }

  // ---- Research tool: an out-of-scope question says so --------------------
  await goto(client, `${ORIGIN}/ask`);
  await sleep(900);
  await evaluate(
    client,
    `const input = document.querySelector("[data-ask-input]");
     input.value = "What is the best recipe for sourdough bread?";
     input.dispatchEvent(new Event("input", { bubbles: true }));
     document.querySelector("[data-ask-run]").click();
     return 1;`
  );
  const offTopic = await waitForAnswer(client, 60000);
  check(
    "research: a question outside ServiceNow is turned down in plain words",
    /servicenow/i.test(offTopic.text) && offTopic.settled,
    `mode=${offTopic.mode} text="${offTopic.text.slice(0, 160).replace(/\s+/g, " ")}"`
  );

  // ---- Research tool: the question survives and the trace is optional -----
  const afterAnswer = await evaluate(
    client,
    `const input = document.querySelector("[data-ask-input]");
     const trace = document.querySelector("[data-ask-answer] details");
     return { questionKept: (input.value || "").length > 0,
              traceIsDisclosure: !!trace, traceClosedByDefault: trace ? !trace.open : null };`
  );
  check(
    "research: the question stays in the box and the developer trace is collapsed",
    afterAnswer.questionKept === true &&
      afterAnswer.traceIsDisclosure === true &&
      afterAnswer.traceClosedByDefault === true,
    JSON.stringify(afterAnswer)
  );
} finally {
  client.close();
  proc.kill("SIGTERM");
}

async function waitForAnswer(client, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let last = { text: "", sources: 0, mode: null, settled: false };
  while (Date.now() < deadline) {
    await sleep(1200);
    last = await evaluate(
      client,
      `const answer = document.querySelector("[data-ask-answer]");
       const status = document.querySelector("[data-ask-status]");
       return {
         text: ((answer ? answer.textContent : "") + " " + (status ? status.textContent : "")).replace(/\\s+/g, " ").trim(),
         sources: document.querySelectorAll("[data-ask-answer] .src").length,
         mode: document.querySelector("[data-ask-answer] .ask-answer") ? "model_answer" : "source_results",
         busy: document.querySelector("[data-ask-run]").disabled === true,
       };`
    );
    if (!last.busy && last.text.length > 20) {
      // Give a streaming response a moment to stop growing.
      await sleep(1500);
      const again = await evaluate(
        client,
        `const answer = document.querySelector("[data-ask-answer]");
         const status = document.querySelector("[data-ask-status]");
         return ((answer ? answer.textContent : "") + " " + (status ? status.textContent : "")).replace(/\\s+/g, " ").trim();`
      );
      if (again === last.text) return { ...last, settled: true };
      last.text = again;
    }
  }
  return { ...last, settled: false };
}

async function countLeads() {
  const { execSync } = await import("node:child_process");
  const out = execSync(
    `npx wrangler d1 execute summaverick --local --command "SELECT COUNT(*) AS n FROM leads" --json`,
    { cwd: "/workspace", encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] }
  );
  const parsed = JSON.parse(out.slice(out.indexOf("[")));
  return Number(parsed[0].results[0].n);
}

const failed = results.filter((r) => !r.pass);
console.log(`\n${results.length - failed.length}/${results.length} checks passed`);
process.exit(failed.length ? 1 : 0);
