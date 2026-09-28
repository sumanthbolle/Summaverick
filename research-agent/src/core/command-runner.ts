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

export async function defaultCommandRunner(
  command: string,
  options: { cwd?: string; timeoutMs?: number } = {}
): Promise<CommandResult> {
  const { spawn } = await import("node:child_process");
  const timeoutMs = options.timeoutMs ?? 30_000;

  return new Promise((resolve) => {
    const child = spawn(command, {
      cwd: options.cwd,
      shell: true,
      env: process.env,
    });

    let stdout = "";
    let stderr = "";
    let settled = false;

    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      child.kill("SIGTERM");
      resolve({
        stdout,
        stderr: stderr || `Command timed out after ${timeoutMs}ms`,
        exitCode: 124,
        command,
      });
    }, timeoutMs);

    child.stdout.on("data", (chunk: Buffer) => {
      stdout += chunk.toString("utf8");
    });
    child.stderr.on("data", (chunk: Buffer) => {
      stderr += chunk.toString("utf8");
    });
    child.on("close", (code) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve({
        stdout,
        stderr,
        exitCode: code ?? 1,
        command,
      });
    });
    child.on("error", (err) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve({
        stdout,
        stderr: err.message,
        exitCode: 1,
        command,
      });
    });
  });
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
