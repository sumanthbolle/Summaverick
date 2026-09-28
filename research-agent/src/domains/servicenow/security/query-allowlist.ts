import {
  BUSINESS_TABLES_REQUIRING_EXPLICIT_OPT_IN,
  type ServiceNowDomainConfig,
} from "../config.js";
import { isBlockedTable, isSensitiveField } from "./sensitive-fields.js";
import type { ServiceNowInstanceQueryInput } from "../schemas/instance-query.js";

export interface QueryAllowlistDecision {
  allowed: boolean;
  reason: string;
  sanitizedFields: string[];
  sanitizedLimit: number;
}

export function validateInstanceQuery(
  input: ServiceNowInstanceQueryInput,
  config: ServiceNowDomainConfig
): QueryAllowlistDecision {
  const table = input.table.trim().toLowerCase();

  if (!config.instance.enabled) {
    return {
      allowed: false,
      reason: "Live instance queries are disabled by configuration.",
      sanitizedFields: [],
      sanitizedLimit: 0,
    };
  }

  if (isBlockedTable(table)) {
    return {
      allowed: false,
      reason: `Table "${table}" is blocked for credential/sensitive data protection.`,
      sanitizedFields: [],
      sanitizedLimit: 0,
    };
  }

  if (
    (BUSINESS_TABLES_REQUIRING_EXPLICIT_OPT_IN as readonly string[]).includes(
      table
    ) &&
    !config.instance.allowlistedTables.includes(table)
  ) {
    return {
      allowed: false,
      reason: `Business table "${table}" requires explicit allowlist configuration.`,
      sanitizedFields: [],
      sanitizedLimit: 0,
    };
  }

  if (!config.instance.allowlistedTables.includes(table)) {
    return {
      allowed: false,
      reason: `Table "${table}" is not on the read-only allowlist.`,
      sanitizedFields: [],
      sanitizedLimit: 0,
    };
  }

  if (!input.encodedQuery || !input.encodedQuery.trim()) {
    return {
      allowed: false,
      reason: "Encoded query (-q) is required for every instance query.",
      sanitizedFields: [],
      sanitizedLimit: 0,
    };
  }

  const allowedFields =
    config.instance.allowlistedFields[table] ??
    config.instance.allowlistedFields["*"] ??
    [];

  const requested = input.fields.length
    ? input.fields.map((f) => f.trim()).filter(Boolean)
    : allowedFields;

  const sanitizedFields = requested.filter(
    (f) => allowedFields.includes(f) && !isSensitiveField(f)
  );

  if (sanitizedFields.length === 0) {
    return {
      allowed: false,
      reason: `No allowlisted fields remain for table "${table}".`,
      sanitizedFields: [],
      sanitizedLimit: 0,
    };
  }

  const sanitizedLimit = Math.max(
    1,
    Math.min(input.limit || config.instance.maximumRows, config.instance.maximumRows)
  );

  return {
    allowed: true,
    reason: "ok",
    sanitizedFields,
    sanitizedLimit,
  };
}
