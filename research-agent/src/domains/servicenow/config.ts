export interface ServiceNowDomainConfig {
  enabled: boolean;
  sdk: {
    enabled: boolean;
    projectRoot?: string;
    minimumExplainVersion: string;
    minimumQueryVersion: string;
  };
  documentation: {
    enabled: boolean;
    repository: string;
    releaseFamily: string;
    indexUrl: string;
    refreshIntervalHours: number;
  };
  instance: {
    enabled: boolean;
    baseUrl?: string;
    allowlistedTables: string[];
    allowlistedFields: Record<string, string[]>;
    maximumRows: number;
    queryTimeoutMs: number;
  };
  citations: {
    required: boolean;
    minimumEvidenceCount: number;
  };
  security: {
    redactSensitiveFields: boolean;
    permitWriteOperations: false;
    persistLiveInstanceResults: false;
  };
}

export const DEFAULT_METADATA_TABLES = [
  "sys_db_object",
  "sys_dictionary",
  "sys_choice",
  "sys_user_role",
  "sys_scope",
  "sys_app",
  "sys_properties",
  "sys_script",
  "sys_script_include",
  "sys_security_acl",
  "sys_ui_action",
  "sys_script_client",
  "sys_ui_policy",
  "sys_ui_policy_action",
  "sys_rest_message",
  "sys_ws_definition",
  "sys_ws_operation",
  "sys_transform_map",
  "sys_transform_entry",
] as const;

export const BUSINESS_TABLES_REQUIRING_EXPLICIT_OPT_IN = [
  "incident",
  "problem",
  "change_request",
  "sc_request",
  "sc_req_item",
  "task",
  "cmdb_ci",
  "sys_user",
] as const;

export const DEFAULT_FIELD_ALLOWLIST: Record<string, string[]> = {
  sys_db_object: ["name", "label", "super_class", "sys_id", "is_extendable"],
  sys_dictionary: [
    "name",
    "element",
    "column_label",
    "internal_type",
    "reference",
    "mandatory",
    "max_length",
    "sys_id",
  ],
  sys_choice: ["name", "element", "value", "label", "sequence", "inactive"],
  sys_user_role: ["name", "description", "sys_id", "elevated_privilege"],
  sys_scope: ["scope", "name", "sys_id", "version"],
  sys_app: ["name", "scope", "version", "sys_id"],
  sys_properties: ["name", "value", "description", "type", "sys_id"],
  sys_script: ["name", "collection", "when", "active", "sys_id"],
  sys_script_include: ["name", "api_name", "client_callable", "active", "sys_id"],
  sys_security_acl: ["name", "operation", "type", "admin_overrides", "active", "sys_id"],
  sys_ui_action: ["name", "table", "action_name", "client", "active", "sys_id"],
  sys_script_client: ["name", "table", "type", "active", "sys_id"],
  sys_ui_policy: ["short_description", "table", "active", "sys_id"],
  sys_ui_policy_action: ["ui_policy", "field", "mandatory", "visible", "disabled"],
  sys_rest_message: ["name", "rest_endpoint", "sys_id"],
  sys_ws_definition: ["name", "service_id", "active", "sys_id"],
  sys_ws_operation: ["name", "web_service_definition", "http_method", "active", "sys_id"],
  sys_transform_map: ["name", "source_table", "target_table", "active", "sys_id"],
  sys_transform_entry: ["map", "source_field", "target_field", "sys_id"],
};

export function defaultServiceNowDomainConfig(
  overrides: Partial<ServiceNowDomainConfig> = {}
): ServiceNowDomainConfig {
  const base: ServiceNowDomainConfig = {
    enabled: true,
    sdk: {
      enabled: true,
      projectRoot: undefined,
      minimumExplainVersion: "4.6.0",
      minimumQueryVersion: "4.8.0",
    },
    documentation: {
      enabled: true,
      repository: "https://github.com/ServiceNow/ServiceNowDocs",
      releaseFamily: "australia",
      indexUrl:
        "https://raw.githubusercontent.com/ServiceNow/ServiceNowDocs/australia/llms.txt",
      refreshIntervalHours: 24,
    },
    instance: {
      enabled: false,
      baseUrl: undefined,
      allowlistedTables: [...DEFAULT_METADATA_TABLES],
      allowlistedFields: { ...DEFAULT_FIELD_ALLOWLIST },
      maximumRows: 50,
      queryTimeoutMs: 15_000,
    },
    citations: {
      required: true,
      minimumEvidenceCount: 1,
    },
    security: {
      redactSensitiveFields: true,
      permitWriteOperations: false,
      persistLiveInstanceResults: false,
    },
  };

  return deepMerge(base, overrides);
}

export function loadServiceNowDomainConfigFromEnv(
  env: NodeJS.ProcessEnv = process.env
): ServiceNowDomainConfig {
  const releaseFamily = env.SERVICENOW_RELEASE_FAMILY || "australia";
  return defaultServiceNowDomainConfig({
    enabled: env.SERVICENOW_DOMAIN_ENABLED !== "false",
    sdk: {
      enabled: env.SERVICENOW_SDK_ENABLED !== "false",
      projectRoot: env.SERVICENOW_SDK_PROJECT_ROOT || undefined,
      minimumExplainVersion: "4.6.0",
      minimumQueryVersion: "4.8.0",
    },
    documentation: {
      enabled: env.SERVICENOW_DOCS_ENABLED !== "false",
      repository:
        env.SERVICENOW_DOCS_REPOSITORY ||
        "https://github.com/ServiceNow/ServiceNowDocs",
      releaseFamily,
      indexUrl:
        env.SERVICENOW_DOCS_INDEX_URL ||
        `https://raw.githubusercontent.com/ServiceNow/ServiceNowDocs/${releaseFamily}/llms.txt`,
      refreshIntervalHours: Number(env.SERVICENOW_DOCS_REFRESH_HOURS || 24),
    },
    instance: {
      enabled: env.SERVICENOW_INSTANCE_QUERY_ENABLED === "true",
      baseUrl: env.SERVICENOW_INSTANCE_URL || undefined,
      allowlistedTables: [...DEFAULT_METADATA_TABLES],
      allowlistedFields: { ...DEFAULT_FIELD_ALLOWLIST },
      maximumRows: Number(env.SERVICENOW_INSTANCE_MAX_ROWS || 50),
      queryTimeoutMs: Number(env.SERVICENOW_INSTANCE_QUERY_TIMEOUT_MS || 15_000),
    },
    citations: {
      required: env.SERVICENOW_REQUIRE_CITATIONS !== "false",
      minimumEvidenceCount: 1,
    },
    security: {
      redactSensitiveFields: true,
      permitWriteOperations: false,
      persistLiveInstanceResults: false,
    },
  });
}

function deepMerge<T extends object>(base: T, overrides: Partial<T>): T {
  const out: Record<string, unknown> = { ...(base as Record<string, unknown>) };
  for (const [key, value] of Object.entries(overrides as Record<string, unknown>)) {
    if (
      value &&
      typeof value === "object" &&
      !Array.isArray(value) &&
      typeof out[key] === "object" &&
      out[key] !== null &&
      !Array.isArray(out[key])
    ) {
      out[key] = deepMerge(out[key] as object, value as object);
    } else if (value !== undefined) {
      out[key] = value;
    }
  }
  return out as T;
}
