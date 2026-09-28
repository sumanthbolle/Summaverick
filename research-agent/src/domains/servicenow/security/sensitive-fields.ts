export const BLOCKED_TABLES = [
  "sys_user_password",
  "oauth_credential",
  "oauth_entity_profile",
  "discovery_credentials",
  "jwt_provider",
  "jwt_claim_validation",
  "sys_certificate",
  "sys_auth_profile",
  "sys_encryption_context",
  "sn_hr_core_profile",
  "sn_hr_core_case",
  "cmdb_ci_password",
] as const;

export const SENSITIVE_FIELD_NAMES = [
  "password",
  "user_password",
  "password_needs_reset",
  "api_key",
  "client_secret",
  "private_key",
  "secret",
  "token",
  "access_token",
  "refresh_token",
  "ssn",
  "national_id",
  "credit_card",
  "journal",
  "comments",
  "work_notes",
  "sys_attachment",
] as const;

export function isBlockedTable(table: string): boolean {
  const t = table.toLowerCase();
  return BLOCKED_TABLES.some((b) => t === b || t.startsWith(`${b}_`));
}

export function isSensitiveField(field: string): boolean {
  const f = field.toLowerCase();
  return SENSITIVE_FIELD_NAMES.some(
    (s) => f === s || f.includes(s) || f.endsWith(`_${s}`)
  );
}

export function redactRecord(
  row: Record<string, unknown>,
  options: { redactSensitiveFields: boolean }
): { row: Record<string, unknown>; redactedFields: string[] } {
  if (!options.redactSensitiveFields) {
    return { row: { ...row }, redactedFields: [] };
  }
  const out: Record<string, unknown> = {};
  const redactedFields: string[] = [];
  for (const [key, value] of Object.entries(row)) {
    if (isSensitiveField(key)) {
      out[key] = "[REDACTED]";
      redactedFields.push(key);
    } else {
      out[key] = value;
    }
  }
  return { row: out, redactedFields };
}
