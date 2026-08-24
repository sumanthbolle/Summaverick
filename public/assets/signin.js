import { mountChrome, api, currentUser, $, showError, setHidden } from "/assets/app.js";

mountChrome({ active: "account" });

function b64urlToBuf(s) {
  const pad = "=".repeat((4 - (s.length % 4)) % 4);
  const b64 = (s + pad).replace(/-/g, "+").replace(/_/g, "/");
  const raw = atob(b64);
  const out = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
  return out.buffer;
}
function bufToB64url(buf) {
  const bytes = new Uint8Array(buf);
  let s = "";
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function publicKeyFromJson(options) {
  const pk = { ...options };
  pk.challenge = b64urlToBuf(options.challenge);
  if (pk.user) pk.user = { ...pk.user, id: b64urlToBuf(options.user.id) };
  if (pk.allowCredentials) {
    pk.allowCredentials = options.allowCredentials.map((c) => ({
      ...c,
      id: b64urlToBuf(c.id),
    }));
  }
  if (pk.excludeCredentials) {
    pk.excludeCredentials = options.excludeCredentials.map((c) => ({
      ...c,
      id: b64urlToBuf(c.id),
    }));
  }
  return pk;
}

function credToJson(cred) {
  const res = cred.response;
  const out = {
    id: cred.id,
    rawId: bufToB64url(cred.rawId),
    type: cred.type,
    response: {},
  };
  if (res.clientDataJSON) out.response.clientDataJSON = bufToB64url(res.clientDataJSON);
  if (res.attestationObject) out.response.attestationObject = bufToB64url(res.attestationObject);
  if (res.authenticatorData) out.response.authenticatorData = bufToB64url(res.authenticatorData);
  if (res.signature) out.response.signature = bufToB64url(res.signature);
  if (res.userHandle) out.response.userHandle = bufToB64url(res.userHandle);
  return out;
}

function paintUser(user) {
  setHidden($("signed"), false);
  setHidden($("authForm"), true);
  $("who").textContent = user.displayName || user.email || user.id;
}

async function refresh() {
  const user = await currentUser();
  if (user) paintUser(user);
}

$("passkey").addEventListener("click", async () => {
  $("error").hidden = true;
  $("notice").hidden = true;
  const email = $("email").value.trim();
  try {
    if (!window.PublicKeyCredential) throw new Error("This browser doesn’t support passkeys.");
    const login = await api("/api/auth/login/options", {
      method: "POST",
      body: JSON.stringify({ email }),
    });
    try {
      const assertion = await navigator.credentials.get({
        publicKey: publicKeyFromJson(login.options),
      });
      await api("/api/auth/login/verify", {
        method: "POST",
        body: JSON.stringify(credToJson(assertion)),
      });
      await refresh();
      return;
    } catch (loginErr) {
      if (loginErr && loginErr.name === "NotAllowedError") throw loginErr;
    }
    const reg = await api("/api/auth/register/options", {
      method: "POST",
      body: JSON.stringify({ email }),
    });
    const attestation = await navigator.credentials.create({
      publicKey: publicKeyFromJson(reg.options),
    });
    await api("/api/auth/register/verify", {
      method: "POST",
      body: JSON.stringify(credToJson(attestation)),
    });
    await refresh();
  } catch (e) {
    showError($("error"), e);
  }
});

$("authForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  $("error").hidden = true;
  try {
    await api("/api/auth/magic", {
      method: "POST",
      body: JSON.stringify({ email: $("email").value.trim() }),
    });
    const n = $("notice");
    n.hidden = false;
    n.textContent = "If that inbox can receive mail, a sign-in link is on its way. It expires in 15 minutes.";
  } catch (err) {
    showError($("error"), err);
  }
});

$("logout").addEventListener("click", async () => {
  await api("/api/auth/logout", { method: "POST" });
  location.reload();
});

refresh();
