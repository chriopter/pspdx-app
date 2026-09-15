// Reads a schema and writes it out for people: every object as a table of its
// fields, with what is required, what values are allowed and what the rules
// between fields say. Nothing here is written by hand, so the page cannot
// drift from the file.
//
// Every element with data-schema="<url>" is filled with its schema; the url is
// relative to the page, and the element's id prefixes the anchors inside it.
(() => {
const esc = s => String(s).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
const code = v => `<code>${esc(typeof v === "string" ? v : JSON.stringify(v))}</code>`;
const list = a => a.map(code).join(", ");

// The patterns that only keep out control characters or ask for https say
// so in words; any other pattern is shown as it is written.
const SAID = {
  "^[^\\u0000-\\u001f]*(?![\\s\\S])": "no control characters",
  "^[^\\u0000-\\u0009\\u000b-\\u001f]*(?![\\s\\S])": "no control characters except a newline",
  "^https://[^\\u0000-\\u001f]+(?![\\s\\S])": "an https:// address",
  "^https://": "an https:// address",
  "^[0-9a-f]{64}(?![\\s\\S])": "64 lowercase hex digits",
  "^[0-9a-f]{32}(?![\\s\\S])": "32 lowercase hex digits",
};
const said = pattern => SAID[pattern] || `matches ${code(pattern)}`;
const strip = desc => (desc || "").replace(/^(required|optional);\s*/i, "");

function typeOf(p, pre) {
  if (p.$ref) { const d = p.$ref.split("/").pop(); return `object <a href="#${esc(pre)}-${esc(d)}">${esc(d)}</a>`; }
  if (p.const !== undefined) return "fixed value";
  if (p.enum) return "one of";
  if (p.type === "array") return `list of ${p.items ? typeOf(p.items, pre) : "values"}`;
  if (p.type === "object" && p.properties) return "object, below";
  return esc(p.type || (p.anyOf ? "string" : "any"));
}

function facts(p) {
  const f = [];
  if (p.const !== undefined) f.push(`exactly ${code(p.const)}`);
  if (p.enum) f.push(list(p.enum));
  if (p.default !== undefined) f.push(`default ${code(p.default)}`);
  const len = (lo, hi, unit) => {
    if (lo !== undefined && hi !== undefined) return lo === hi ? `${lo} ${unit}` : `${lo}–${hi} ${unit}`;
    if (hi !== undefined) return `up to ${hi} ${unit}`;
    if (lo !== undefined) return `at least ${lo} ${unit}`;
  };
  const l = len(p.minLength, p.maxLength, "characters"); if (l) f.push(l);
  const n = len(p.minItems, p.maxItems, "entries"); if (n) f.push(n);
  if (p.uniqueItems) f.push("no duplicates");
  if (p.minimum !== undefined) f.push(`at least ${p.minimum}`);
  if (p.format) f.push(`format ${code(p.format)}`);
  if (p.pattern) f.push(said(p.pattern));
  if (p.anyOf) f.push("either " + p.anyOf.map(a => [a.format && `format ${code(a.format)}`, a.pattern && `matching ${code(a.pattern)}`].filter(Boolean).join(" ")).join("<br>or "));
  if (p.if && p.then) f.push(`if it ${cond(p.if)}, then it also ${cond(p.then)}`);
  if (p.items && p.items.type !== "object" && !p.items.$ref) {
    const inner = facts(p.items); if (inner.length) f.push("each: " + inner.join("; "));
  }
  return f;
}

// The few shapes of condition the PSPDX schemas use, said in words; anything
// else is shown as JSON rather than guessed at.
function cond(c) {
  const parts = [];
  if (c.pattern) parts.push(`matches ${code(c.pattern)}`);
  if (c.not && c.not.pattern) parts.push(`does not match ${code(c.not.pattern)}`);
  if (c.required) parts.push(`has ${list(c.required)}`);
  if (c.not && c.not.required) parts.push(`has no ${list(c.not.required)}`);
  if (c.anyOf) parts.push(c.anyOf.map(cond).join(" or "));
  if (c.properties) for (const [k, v] of Object.entries(c.properties)) {
    if (v.enum) parts.push(`${code(k)} is one of ${list(v.enum)}`);
    else if (v.const !== undefined) parts.push(`${code(k)} is ${code(v.const)}`);
    else if (v.not && v.not.pattern) parts.push(`${code(k)} does not match ${code(v.not.pattern)}`);
    else if (v.pattern) parts.push(`${code(k)} matches ${code(v.pattern)}`);
    else if (v.required) parts.push(`${code(k)} has ${list(v.required)}`);
  }
  return parts.length ? parts.join(" and ") : code(c);
}

// A condition about the entry itself reads "it has …"; one about a field of
// it already names the field, so it reads "its `release` has …".
function subject(text) { return (text.startsWith("<code>") ? "its " : "it ") + text; }

function rules(obj) {
  return (obj.allOf || []).map(r => r.if
    ? `<li>If the entry ${cond(r.if)}, then ${subject(cond(r.then))}${r.else ? `; otherwise ${subject(cond(r.else))}` : ""}.</li>`
    : `<li>${code(r)}</li>`).join("");
}

function table(pre, id, obj, heading) {
  const req = new Set(obj.required || []);
  let html = `<h3 id="${esc(pre)}-${esc(id)}">${heading}</h3>`;
  if (obj.description) html += `<p class="dim">${esc(strip(obj.description))}</p>`;
  html += `<div class="scroll"><table><tr><th>Field</th><th>Type</th><th>Allowed</th><th>Meaning</th></tr>`;
  const nested = [];
  for (const [k, p] of Object.entries(obj.properties || {})) {
    const f = facts(p);
    html += `<tr><td class="name">${code(k)}<br>${req.has(k) ? '<span class="req">required</span>' : '<span class="dim">optional</span>'}</td>` +
            `<td>${typeOf(p, pre)}</td><td>${f.length ? `<ul class="facts">${f.map(x => `<li>${x}</li>`).join("")}</ul>` : ""}</td>` +
            `<td>${esc(strip(p.description))}</td></tr>`;
    if (p.type === "object" && p.properties) nested.push([`${id}-${k}`, p, `${code(k)} in ${esc(heading.replace(/<[^>]+>/g, ""))}`]);
  }
  html += `</table></div>`;
  const r = rules(obj); if (r) html += `<p><strong>Rules</strong></p><ul class="rules">${r}</ul>`;
  html += `<p class="dim">${obj.additionalProperties === false ? "Fields not listed here are rejected." : "Fields not listed here are allowed and ignored."}</p>`;
  for (const [nid, p, h] of nested) html += table(pre, nid, p, h);
  return html;
}

function render(s, pre) {
  let html = table(pre, "top", s, "Top level");
  for (const [k, d] of Object.entries(s.$defs || {})) if (d.properties) html += table(pre, k, d, code(k));
  return html;
}

// A link to an anchor inside a closed section opens the section first.
function reveal() {
  const el = location.hash.length > 1 && document.getElementById(decodeURIComponent(location.hash.slice(1)));
  if (!el) return;
  for (let d = el.closest("details"); d; d = d.parentElement.closest("details")) d.open = true;
  el.scrollIntoView();
}
addEventListener("hashchange", reveal);

Promise.all([...document.querySelectorAll("[data-schema]")].map(box => {
  const url = box.dataset.schema;
  return fetch(url).then(r => { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(s => { box.innerHTML = render(s, box.id || "s"); })
    .catch(e => { box.innerHTML = `<p>Could not read ${code(url)} (${esc(e.message)}).</p>`; });
})).then(reveal);
})();
