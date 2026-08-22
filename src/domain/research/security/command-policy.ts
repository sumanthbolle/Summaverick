export const ALLOWED_READONLY_COMMAND_PREFIXES = [
  "npx @servicenow/sdk --help",
  "npx @servicenow/sdk explain",
  "npx @servicenow/sdk query",
  "npm list @servicenow/sdk",
  "cat package.json",
] as const;

export const MUTATING_SDK_SUBCOMMANDS = [
  "build",
  "transform",
  "deploy",
  "install",
  "uninstall",
  "upgrade",
  "publish",
  "delete",
] as const;

export interface CommandPolicyDecision {
  allowed: boolean;
  mutating: boolean;
  requiresApproval: boolean;
  reason: string;
  normalizedCommand: string;
}

export function evaluateSdkCommand(
  command: string,
  options: { permitWriteOperations: boolean } = { permitWriteOperations: false }
): CommandPolicyDecision {
  const normalizedCommand = command.trim().replace(/\s+/g, " ");
  const lower = normalizedCommand.toLowerCase();

  const mutatingHit = MUTATING_SDK_SUBCOMMANDS.find((sub) => {
    const re = new RegExp(`(?:^|\\s)${sub}(?:\\s|$)`, "i");
    return (
      /@servicenow\/sdk/.test(lower) &&
      re.test(lower.replace(/npx\s+/, " "))
    );
  });

  if (mutatingHit) {
    return {
      allowed: options.permitWriteOperations,
      mutating: true,
      requiresApproval: true,
      reason: `Potentially mutating SDK subcommand "${mutatingHit}" requires explicit approval.`,
      normalizedCommand,
    };
  }

  const allowed = ALLOWED_READONLY_COMMAND_PREFIXES.some((prefix) =>
    lower.startsWith(prefix.toLowerCase())
  );

  if (!allowed) {
    return {
      allowed: false,
      mutating: false,
      requiresApproval: true,
      reason: "Command is outside the read-only ServiceNow SDK allowlist.",
      normalizedCommand,
    };
  }

  // Never guess flags: require --help for unknown first-use paths is enforced by callers.
  return {
    allowed: true,
    mutating: false,
    requiresApproval: false,
    reason: "Read-only allowlisted command.",
    normalizedCommand,
  };
}

export function buildInstanceQueryCommand(input: {
  table: string;
  encodedQuery: string;
  fields: string[];
  limit: number;
}): string {
  const fieldsArg =
    input.fields.length > 0 ? ` --fields ${input.fields.join(",")}` : "";
  const limitArg = ` --limit ${input.limit}`;
  // Structured construction only — never expose a generic shell tool to the model.
  return `npx @servicenow/sdk query ${shellQuote(input.table)} -q ${shellQuote(
    input.encodedQuery
  )} -o json${fieldsArg}${limitArg}`;
}

function shellQuote(value: string): string {
  return `'${value.replace(/'/g, `'\\''`)}'`;
}
