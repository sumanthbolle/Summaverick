import type {
  ServiceNowIntent,
  ServiceNowIntentResult,
} from "../schemas/domain-intent.js";

const MODULE_KEYWORDS: Record<string, string[]> = {
  itsm: ["incident", "problem", "change", "itsm", "assignment rule", "sla"],
  cmdb: [
    "cmdb",
    "ci ",
    "configuration item",
    "reconciliation",
    "identification",
    "discovery",
    "service mapping",
  ],
  itom: ["itom", "event management", "orchestration", "mid server"],
  irm: ["irm", "risk", "policy exception"],
  grc: ["grc", "governance", "compliance", "audit"],
  spm: ["spm", "portfolio", "demand", "project workspace"],
  csm: ["csm", "customer service", "case", "sn_customerservice"],
  hrsd: ["hrsd", "hr service", "employee center", "hr case"],
  secops: ["secops", "security incident", "vulnerability response"],
  app_engine: ["app engine", "scoped app", "studio", "app engine studio"],
  flow_designer: ["flow designer", "flow(", "subflow", "action designer"],
  integrationhub: ["integrationhub", "spoke", "restmessage", "rest message"],
  ui_builder: ["ui builder", "workspace", "uxf", "experience"],
  fluent_sdk: [
    "fluent",
    "@servicenow/sdk",
    ".now.ts",
    "now.config.json",
    "businessrule",
    "scriptinclude",
    "now.ref",
    "now.id",
    "keys.ts",
  ],
  platform: [
    "acl",
    "glide record",
    "gliderecord",
    "script include",
    "business rule",
    "client script",
    "ui policy",
    "transform map",
    "update set",
  ],
};

const DOMAIN_MARKERS = [
  "servicenow",
  "service now",
  "now platform",
  "fluent",
  "@servicenow/sdk",
  ".now.ts",
  "now.config.json",
  "gliderecord",
  "glide record",
  "cmdb",
  "itsm",
  "itom",
  "hrsd",
  "secops",
  "business rule",
  "businessrule",
  "script include",
  "scriptinclude",
  "transform map",
  "update set",
  "sys_",
  "sysid",
  "sys_id",
];

const RELEASE_FAMILIES = [
  "australia",
  "zurich",
  "yokohama",
  "xanadu",
  "washington",
  "vancouver",
  "utah",
  "tokyo",
];

export function isServiceNowDomainQuery(query: string): boolean {
  const q = query.toLowerCase();
  return DOMAIN_MARKERS.some((m) => q.includes(m));
}

export function classifyServiceNowIntent(query: string): ServiceNowIntentResult {
  const q = query.toLowerCase();
  const modules = detectModules(q);
  const requestedReleaseFamily = RELEASE_FAMILIES.find((f) => q.includes(f));

  let intent: ServiceNowIntent = "unknown";
  let rationale = "No strong ServiceNow intent signals.";
  let confidence = 0.35;

  const rules: Array<{
    intent: ServiceNowIntent;
    test: () => boolean;
    rationale: string;
    confidence: number;
  }> = [
    {
      intent: "instance_schema",
      test: () =>
        /\b(field|column|dictionary|choice|schema|table inheritance|row limit|unauthorized table)\b/.test(
          q
        ) &&
        /\b(exist|type|choices?|instance|my (table|incident)|query|enforce)\b/.test(
          q
        ),
      rationale: "Asks about instance schema / field existence.",
      confidence: 0.86,
    },
    {
      intent: "instance_record_lookup",
      test: () =>
        /\b(record|sys_id|does .* exist|find .* role|lookup|locate .* role)\b/.test(
          q
        ) && /\b(instance|table|role)\b/.test(q),
      rationale: "Asks for live record/role lookup.",
      confidence: 0.82,
    },
    {
      intent: "fluent_sdk",
      test: () =>
        /(fluent|@servicenow\/sdk|\.now\.ts|now\.config\.json|now\.ref|now\.id|keys\.ts)/i.test(
          q
        ) ||
        /\b(businessrule|scriptinclude|table\(|acl\()\b/.test(q),
      rationale: "Mentions Fluent SDK / metadata APIs.",
      confidence: 0.9,
    },
    {
      intent: "code_review",
      test: () =>
        /\b(review|invalid propert|why .* failing|bug|lint)\b/.test(q) &&
        /(fluent|\.now\.ts|business ?rule|script)/i.test(q),
      rationale: "Requests review of Fluent/platform code.",
      confidence: 0.84,
    },
    {
      intent: "release_comparison",
      test: () =>
        /\b(what changed|between releases?|release notes|upgrade from)\b/.test(q) ||
        Boolean(requestedReleaseFamily && /\b(vs|versus|compared?|difference)\b/.test(q)),
      rationale: "Asks about release differences.",
      confidence: 0.8,
    },
    {
      intent: "security_review",
      test: () =>
        /\b(acl|secure|security|cross-?scope|privilege|client.?callable|access control)\b/.test(
          q
        ) &&
        !/\b(how (do|should) i (implement|design|build)|architecture)\b/.test(q),
      rationale: "Security / ACL / privilege focus.",
      confidence: 0.78,
    },
    {
      intent: "troubleshooting",
      test: () =>
        /\b(why|not working|failing|broken|error|troubleshoot|debug)\b/.test(q),
      rationale: "Troubleshooting wording detected.",
      confidence: 0.76,
    },
    {
      intent: "implementation_design",
      test: () =>
        /\b(how (do|should) i (implement|design|build)|architecture|best way to)\b/.test(
          q
        ),
      rationale: "Implementation / design request.",
      confidence: 0.74,
    },
    {
      intent: "configuration_guidance",
      test: () =>
        /\b(configure|configuration|assignment rules?|notification|ui policy)\b/.test(
          q
        ),
      rationale: "Configuration guidance request.",
      confidence: 0.72,
    },
    {
      intent: "platform_development",
      test: () =>
        /\b(gliderecord|script include|business rule|rest api|transform maps?|client script|client versus server|server execution)\b/.test(
          q
        ) ||
        /\b(scoped application|update set|scripted rest|journal fields?|import sets?)\b/.test(
          q
        ) ||
        /restmessage/i.test(q),
      rationale: "Classic platform development topic.",
      confidence: 0.8,
    },
    {
      intent: "product_concept",
      test: () =>
        /\b(what is|explain|how does .* work|versus|fundamentals)\b/.test(q) ||
        modules.some((m) =>
          ["itsm", "cmdb", "itom", "csm", "hrsd", "spm"].includes(m)
        ),
      rationale: "Product/domain concept question.",
      confidence: 0.7,
    },
  ];

  for (const rule of rules) {
    if (rule.test()) {
      intent = rule.intent;
      rationale = rule.rationale;
      confidence = rule.confidence;
      break;
    }
  }

  const requiresLiveInstance =
    intent === "instance_schema" || intent === "instance_record_lookup";

  const requiresSdkDocs =
    intent === "fluent_sdk" ||
    intent === "code_review" ||
    /(fluent|@servicenow\/sdk|\.now\.ts)/i.test(q);

  const requiresRepositoryContext =
    intent === "code_review" ||
    intent === "troubleshooting" ||
    intent === "implementation_design" ||
    /\b(this (file|project|repo)|\.now\.ts|keys\.ts)\b/.test(q);

  const requiresProductDocs =
    !requiresSdkDocs ||
    intent === "product_concept" ||
    intent === "configuration_guidance" ||
    intent === "release_comparison" ||
    intent === "troubleshooting" ||
    intent === "platform_development" ||
    intent === "security_review";

  return {
    domain: "servicenow",
    intent,
    modules: modules.length ? modules : ["platform"],
    requiresSdkDocs,
    requiresProductDocs,
    requiresRepositoryContext,
    requiresLiveInstance,
    requestedReleaseFamily,
    confidence,
    rationale,
  };
}

function detectModules(q: string): string[] {
  const found: string[] = [];
  for (const [module, keywords] of Object.entries(MODULE_KEYWORDS)) {
    if (keywords.some((k) => q.includes(k))) {
      found.push(module);
    }
  }
  return found;
}
