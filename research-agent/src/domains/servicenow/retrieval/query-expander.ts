const SDK_SEARCH_TERMS = [
  "BusinessRule",
  "Table",
  "Acl",
  "Flow",
  "ScriptInclude",
  "ClientScript",
  "UiPolicy",
  "RestApi",
  "Record",
  "Now.ref",
  "Now.ID",
  "Now.include",
  "keys-file",
  "build",
  "transform",
  "deploy",
  "auth",
  "naming",
  "structure",
  "scoping",
  "file-layout",
  "query",
  "encoded-query-guide",
];

const PRODUCT_EXPANSIONS: Record<string, string[]> = {
  "business rule": ["before business rule", "after business rule", "async business rule"],
  cmdb: ["CMDB identification", "CMDB reconciliation", "CI relationships"],
  incident: ["incident lifecycle", "incident state", "assignment rules"],
  acl: ["access control list", "security ACL", "table ACL", "field ACL"],
  fluent: ["ServiceNow Fluent SDK", "now.ts metadata", "BusinessRule Fluent"],
};

export function expandServiceNowQuery(query: string): string[] {
  const expansions = new Set<string>([query.trim()]);
  const lower = query.toLowerCase();

  for (const [key, values] of Object.entries(PRODUCT_EXPANSIONS)) {
    if (lower.includes(key)) {
      for (const v of values) expansions.add(v);
    }
  }

  for (const term of SDK_SEARCH_TERMS) {
    if (lower.includes(term.toLowerCase())) {
      expansions.add(term);
    }
  }

  return [...expansions].slice(0, 12);
}

export function suggestSdkSearchTerms(query: string): string[] {
  const lower = query.toLowerCase();
  const hits = SDK_SEARCH_TERMS.filter((t) => lower.includes(t.toLowerCase()));
  if (hits.length) return hits;
  if (/\bbusiness\s*rule\b/.test(lower)) return ["BusinessRule"];
  if (/\bscript\s*include\b/.test(lower)) return ["ScriptInclude"];
  if (/\bacl\b/.test(lower)) return ["Acl"];
  if (/\btable\b/.test(lower)) return ["Table"];
  return ["structure", "naming"];
}
