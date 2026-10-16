/** Browser regression for the migrated assistant. Only AI responses are fixtures;
 *  HTML, assets and routing are served by the real local Worker.
 *  node scripts/verify/ask-summaverick.mjs [origin]
 */
import { evaluate, goto, launch, setReducedMotion, sleep } from "./cdp.mjs";

const origin = process.argv[2] ?? "http://127.0.0.1:8788";
const { proc, client } = await launch({ headless: true, width: 1440, height: 1000, port: 9225 });
const errors = [];
const requests = [];
client.on("Runtime.exceptionThrown", e => errors.push(e.exceptionDetails.exception?.description ?? e.exceptionDetails.text));
client.on("Network.responseReceived", e => {
  if (e.response.url.startsWith(origin) && e.response.status >= 400) errors.push(`${e.response.status} ${e.response.url}`);
});
client.on("Network.requestWillBeSent", e => requests.push(e.request.url));
const check = (name, pass) => {
  if (!pass) throw new Error(name);
  console.log(`PASS ${name}`);
};
async function until(expression) {
  for (let i = 0; i < 100; i++) {
    if (await evaluate(client, `return ${expression}`)) return;
    await sleep(100);
  }
  throw new Error(`Timed out: ${expression}`);
}
async function ask(text) {
  await evaluate(client, `const input=document.querySelector('#queryInput'); input.value=${JSON.stringify(text)}; input.dispatchEvent(new Event('input')); document.querySelector('#sendBtn').click();`);
  await until("!document.querySelector('#loadingMsg')");
}
try {
  await client.send("Runtime.enable");
  await client.send("Network.enable");
  await setReducedMotion(client, true);
  await client.send("Page.addScriptToEvaluateOnNewDocument", { source: `
    if(!sessionStorage.getItem('ask-regression')) { localStorage.removeItem('summaverick-chats'); sessionStorage.setItem('ask-regression','1'); }
    window.__chatRequests=[];
    const realFetch=window.fetch.bind(window);
    window.fetch=async function(input,init){
      const url=new URL(String(input),location.origin);
      if(url.pathname==='/api/ask-summaverick'){
        const body=JSON.parse(init.body); window.__chatRequests.push(body);
        if(body.query.includes('test failure')) return Response.json({success:false,message:'Please retry this question.'},{status:502});
        return Response.json({success:true,result:{answer:'The fixture answer [1].',sources:[{index:1,title:'Evidence',url:'https://example.org/evidence',snippet:'Supporting context',source:'example.org'}],relatedQuestions:['What changed?']}});
      }
      if(url.pathname==='/api/trending') return Response.json({success:true,countryName:'Worldwide',widgets:{worldNews:{title:'Fixture headline',summary:'A fixture summary',url:'https://example.org/news',source:'Example'}}});
      return realFetch(input,init);
    };
  ` });
  await goto(client, origin + "/ask-summaverick?q=Explain%20optics");
  await until("window.__chatRequests.length===1 && !document.querySelector('#loadingMsg')");
  check("canonical route and Meet Summaverick title", await evaluate(client, "return location.pathname==='/ask-summaverick' && document.title.startsWith('Meet Summaverick')"));
  check("deep-link question produces an answer and sources", await evaluate(client, "return document.querySelector('#chatArea').textContent.includes('fixture answer') && !!document.querySelector('.cite')"));
  await evaluate(client, "document.querySelector('[data-mode=deeper]').click()");
  await ask("Explain the scattering");
  check("follow-up carries prior question and answer with selected depth", await evaluate(client, "const q=window.__chatRequests.at(-1);return q.depth==='deeper' && q.context.length===2 && q.context[0].content==='Explain optics' && q.context[1].content.includes('fixture answer')"));
  await evaluate(client, "document.querySelector('.cite').click()");
  check("citation preview opens the source", await evaluate(client, "return ['#citePreview','#citeSheet'].some(sel=>!document.querySelector(sel).hidden && document.querySelector(sel+' [data-cite-open]').href==='https://example.org/evidence')"));
  await evaluate(client, "window.SVWorkspace.closeCite()");
  await evaluate(client, "document.querySelector('#newChatBtn').click()");
  check("new conversation resets the thread", await evaluate(client, "return document.querySelector('#chatArea').textContent.trim()===''"));
  await ask("test failure");
  check("failed requests show a useful retry message", await evaluate(client, "return document.querySelector('#chatArea').textContent.includes('Please retry this question.') && !document.body.classList.contains('loading-active')"));
  await evaluate(client, "document.querySelector('#newChatBtn').click()");
  await until("document.querySelector('#trendingGrid').textContent.includes('Fixture headline')");
  await evaluate(client, "document.querySelector('.lead-story').click()");
  await until("!document.querySelector('#loadingMsg')");
  check("trending cards ask about the story", await evaluate(client, "return window.__chatRequests.at(-1).query.includes('Fixture headline')"));
  await evaluate(client, "document.querySelector('[data-mode=research]').click()");
  await ask("Give more detail");
  check("Full picture depth reaches the API", await evaluate(client, "return window.__chatRequests.at(-1).depth==='research'"));
  await goto(client, origin + "/ask-summaverick");
  await until("document.querySelector('#chatList').textContent.includes('Explain optics')");
  check("chat history survives reload", await evaluate(client, "return JSON.parse(localStorage.getItem('summaverick-chats')).some(c=>c.messages.length>=4)"));
  await client.send("Emulation.setDeviceMetricsOverride", { width: 390, height: 900, deviceScaleFactor: 1, mobile: true });
  await sleep(150);
  await evaluate(client, "document.querySelector('#historyBtn').click()");
  await until("document.querySelector('#chatDrawer').classList.contains('open')");
  check("history drawer opens", await evaluate(client, "return document.querySelector('#chatDrawer').classList.contains('open')"));
  await evaluate(client, "[...document.querySelectorAll('[data-open]')].find(el=>el.textContent.includes('Explain optics')).click()");
  check("saved conversation restores its answers", await evaluate(client, "return document.querySelector('#chatArea').textContent.includes('scattering') && document.querySelectorAll('#chatArea .msg.ai').length===2"));
  await evaluate(client, "document.querySelector('.theme-btn[data-theme=light]').click()");
  check("theme choice is shared with the destination site", await evaluate(client, "return document.documentElement.dataset.theme==='light' && localStorage.getItem('sv-theme')==='light'"));
  await evaluate(client, "document.querySelector('#historyBtn').click();[...document.querySelectorAll('[data-open]')].find(el=>el.textContent.includes('Explain optics')).querySelector('[data-del]').click()");
  check("saved conversations can be deleted", await evaluate(client, "return !JSON.parse(localStorage.getItem('summaverick-chats')).some(c=>c.title.includes('Explain optics'))"));
  await evaluate(client, "document.querySelector('#drawerClose').click()");
  for (const width of [390, 768, 1440]) {
    await client.send("Emulation.setDeviceMetricsOverride", { width, height: 900, deviceScaleFactor: 1, mobile: width < 768 });
    await sleep(150);
    check(`no horizontal overflow at ${width}px`, await evaluate(client, "return document.documentElement.scrollWidth <= innerWidth+1"));
  }
  check("no page exceptions or missing same-origin resources", errors.length === 0);
  check("no dependency on the old website or Worker", requests.every(url => !/sumanthbolle\.com|wandering-haze/.test(url)));
  for (const width of [390, 1024, 1440]) {
    await client.send("Emulation.setDeviceMetricsOverride", { width, height: 900, deviceScaleFactor: 1, mobile: width < 768 });
    await goto(client, origin + "/");
    check(`home navigation fits at ${width}px`, await evaluate(client, "const nav=document.querySelector('.nav-links'), actions=document.querySelector('.nav-actions');return document.documentElement.scrollWidth<=innerWidth+1 && (getComputedStyle(nav).display==='none' || nav.getBoundingClientRect().right<=actions.getBoundingClientRect().left+1)"));
  }
  console.log("AI responses were fixtures; no live model calls were made.");
} finally {
  client.close();
  proc.kill();
}
