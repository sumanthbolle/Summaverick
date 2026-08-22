/**
 * Command runner contract. Ported from research-agent/src/core/command-runner.ts.
 *
 * The original `defaultCommandRunner` spawned `@servicenow/sdk` via
 * `node:child_process`. A Cloudflare Worker cannot spawn subprocesses, so the
 * default runner here is a SHIM that returns a non-zero "unavailable" result.
 * The type, the mock runner, and every call site are unchanged, so the SDK
 * `explain` (Layer 1) and SDK `query` (Layer 3) providers still run their real
 * control flow — they simply observe that the CLI is unavailable and skip,
 * exactly as they would against a machine with no SDK installed.
 */

export interface CommandResult {
  stdout: string;
  stderr: string;
  exitCode: number;
  command: string;
}

export type CommandRunner = (
  command: string,
  options?: { cwd?: string; timeoutMs?: number }
) => Promise<CommandResult>;

/** Worker shim: subprocess execution is not available in this runtime. */
export async function defaultCommandRunner(
  command: string,
  _options: { cwd?: string; timeoutMs?: number } = {}
): Promise<CommandResult> {
  return {
    command,
    stdout: "",
    stderr:
      "Command execution is unavailable in the Cloudflare Worker runtime (no subprocess).",
    exitCode: 127,
  };
}

export function createMockCommandRunner(
  handlers: Record<string, CommandResult | ((cmd: string) => CommandResult)>
): CommandRunner {
  return async (command) => {
    for (const [pattern, handler] of Object.entries(handlers)) {
      if (command.includes(pattern)) {
        return typeof handler === "function" ? handler(command) : handler;
      }
    }
    return {
      command,
      stdout: "",
      stderr: `No mock handler for: ${command}`,
      exitCode: 1,
    };
  };
}
