/**
 * Shared research-agent primitives reused by domain packs.
 * Summaverick Worker chat contract remains unchanged; this package
 * extends research behaviour through modular domain packs.
 */

export type Confidence = "high" | "medium" | "low";

export interface AuthorizedResearchContext {
  tenantId?: string;
  userId?: string;
  taskId: string;
  allowLiveInstance: boolean;
  workingDirectory?: string;
  activeFilePath?: string;
}

export interface TraceEvent {
  name: string;
  timestamp: string;
  attributes?: Record<string, string | number | boolean | null | undefined>;
}

export interface TraceSink {
  emit(event: TraceEvent): void;
}

export class InMemoryTraceSink implements TraceSink {
  readonly events: TraceEvent[] = [];

  emit(event: TraceEvent): void {
    this.events.push(event);
  }
}

export function nowIso(): string {
  return new Date().toISOString();
}

export function createId(prefix: string): string {
  return `${prefix}_${Math.random().toString(36).slice(2, 10)}_${Date.now().toString(36)}`;
}

export function compareSemver(a: string, b: string): number {
  const pa = parseSemver(a);
  const pb = parseSemver(b);
  for (let i = 0; i < 3; i++) {
    if (pa[i] !== pb[i]) return pa[i] - pb[i];
  }
  return 0;
}

export function parseSemver(version: string): [number, number, number] {
  const cleaned = version.replace(/^v/, "").split("-")[0] ?? "0.0.0";
  const parts = cleaned.split(".").map((p) => Number.parseInt(p, 10) || 0);
  return [parts[0] ?? 0, parts[1] ?? 0, parts[2] ?? 0];
}

export function isVersionAtLeast(installed: string, minimum: string): boolean {
  return compareSemver(installed, minimum) >= 0;
}
