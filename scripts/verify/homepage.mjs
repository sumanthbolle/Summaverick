/** Browser checks for the intro → RAG homepage flow and its remaining demos.
 *  node scripts/verify/homepage.mjs [origin]
 */
import { evaluate, goto, launch, pressKey, setReducedMotion, sleep } from "./cdp.mjs";

const ORIGIN = process.argv[2] ?? "http://127.0.0.1:8788";
const { proc, client } = await launch({ headless: true, width: 1440, height: 1000 });
const results = [];
const errors = [];
const check = (name, pass, detail = "") => {
  results.push({ name, pass });
  console.log(`${pass ? "PASS" : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
client.on("Runtime.exceptionThrown", event => errors.push(event.exceptionDetails.text));
client.on("Network.responseReceived", event => {
  if (event.response.url.startsWith(ORIGIN) && event.response.status >= 400) errors.push(`${event.response.status} ${event.response.url}`);
});
try {
  await client.send("Runtime.enable");
  await client.send("Network.enable");
  await setReducedMotion(client, false);
  await goto(client, `${ORIGIN}/`);
  await sleep(450);
  const intro = await evaluate(client, `return {
    sections: [...document.querySelectorAll('main > section')].slice(0,3).map(el=>el.id),
    removed: !document.querySelector('[data-hero-panel],[data-stage]'),
    lead: document.querySelector('.hero .studio-lead').textContent,
    heading: getComputedStyle(document.querySelector('#hero-title')).opacity,
    actions: [...document.querySelectorAll('.hero .btn')].map(el=>el.getAttribute('href')),
  }`);
  check("RAG follows the intro directly, followed by services", JSON.stringify(intro.sections) === JSON.stringify(["top", "rag", "build"]));
  check("both opening illustrations are removed", intro.removed);
  check("the offer and actions are readable before animation", intro.lead.includes("A software studio for AI agents.") && Number(intro.heading) > 0.99 && intro.actions.includes("#rag"));
  await evaluate(client, `document.querySelector('.hero a[href="#rag"]').click()`);
  await sleep(500);
  check("the intro action lands on RAG below the sticky navigation", await evaluate(client, `const r=document.querySelector('#rag-title').getBoundingClientRect(); return location.hash==='#rag' && r.top>=document.querySelector('#nav').getBoundingClientRect().bottom-1 && r.top<innerHeight`));
  check("the RAG offer and sample-data label remain visible", await evaluate(client, `return document.querySelector('#rag-title').textContent.includes('RAG assistants') && document.querySelector('.rag__board').textContent.includes('sample data')`));

  // A removed scenario must lead to a real page instead of a dead stage link.
  check("the personal-agent action opens the assistant", await evaluate(client, `return document.querySelector('.offer__link[href="/ask-summaverick"]')!==null`));
  check("older stage/workflow links still land in the examples section", await evaluate(client, `return ['stage','workflow'].every(id=>document.getElementById(id).closest('section').id==='examples')`));
  await evaluate(client, `document.querySelector('#examples').scrollIntoView()`);
  await sleep(350);
  check("remaining demos initialize with an explicit disclaimer", await evaluate(client, `return document.querySelector('[data-demo]').classList.contains('is-enhanced') && document.querySelector('.demo__disclaimer').textContent.includes('Illustrative examples with sample data')`));
  await evaluate(client, `document.querySelector('#demo-tab-3').click()`);
  check("the RAG demo action selects company knowledge", await evaluate(client, `return document.querySelector('#demo-tab-3').getAttribute('aria-selected')==='true' && !document.querySelector('#demo-panel-3').hidden`));

  await setReducedMotion(client, true);
  await goto(client, `${ORIGIN}/`);
  await sleep(250);
  check("reduced motion keeps the intro and reveals readable", await evaluate(client, `return document.documentElement.getAttribute('data-motion')===null && [...document.querySelectorAll('[data-reveal]')].every(el=>Number(getComputedStyle(el).opacity)>0.99) && document.querySelector('[data-face-brand]').textContent.includes('Summaverick')`));
  for (const index of [1, 2, 3, 4]) {
    await evaluate(client, `document.querySelector('#demo-tab-${index}').click()`);
    check(`example ${index} works with reduced motion`, await evaluate(client, `const panel=document.querySelector('#demo-panel-${index}');return !panel.hidden && panel.dataset.at==='4' && document.querySelector('#demo-tab-${index}').getAttribute('aria-selected')==='true'`));
  }
  await evaluate(client, `document.querySelector('#demo-tab-1').focus()`);
  await pressKey(client, { key: "ArrowRight", code: "ArrowRight", keyCode: 39 });
  check("examples support keyboard selection", await evaluate(client, `return document.querySelector('#demo-tab-2').getAttribute('aria-selected')==='true'`));
  const faq = await evaluate(client, `const detail=document.querySelector('#faq details');detail.scrollIntoView({block:'center',behavior:'instant'});const before=scrollY;detail.querySelector('summary').click();return {open:detail.open,stable:before===scrollY}`);
  check("FAQ explanations still open in place", faq.open && faq.stable);
  check("contact controls remain available and the honeypot is skipped", await evaluate(client, `const hidden=document.querySelector('[name="company_url"]');return document.querySelector('[data-lead-submit]').getBoundingClientRect().height>0 && document.querySelector('[data-lead-retry]').hidden && hidden.tabIndex===-1`));

  // Browser-injected evaluation still works while page scripts are disabled.
  await client.send("Emulation.setScriptExecutionDisabled", { value: true });
  await goto(client, `${ORIGIN}/`);
  const noScript = await evaluate(client, `return {
    brand: document.querySelector('[data-face-brand]').textContent.includes('Summaverick'),
    rag: document.querySelector('#rag').getBoundingClientRect().height>0,
    examples: [...document.querySelectorAll('[data-demo-panel]')].filter(el=>el.getBoundingClientRect().height>0).length,
    visible: [...document.querySelectorAll('[data-reveal]')].every(el=>Number(getComputedStyle(el).opacity)>0.99),
    actions: [...document.querySelectorAll('.hero .btn')].every(el=>el.getBoundingClientRect().height>0),
  }`);
  check("without scripting, the intro, RAG, actions and all four examples read", noScript.brand && noScript.rag && noScript.examples===4 && noScript.visible && noScript.actions, JSON.stringify(noScript));
  await client.send("Emulation.setScriptExecutionDisabled", { value: false });
  check("no page exceptions or missing local resources", errors.length===0, errors.join(", "));
} finally {
  client.close();
  proc.kill();
}
console.log(`${results.filter(result=>result.pass).length}/${results.length} checks passed`);
process.exitCode = results.some(result=>!result.pass) ? 1 : 0;
