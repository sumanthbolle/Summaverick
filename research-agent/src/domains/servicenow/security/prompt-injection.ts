const INJECTION_PATTERNS: RegExp[] = [
  /ignore\s+(all\s+)?(previous|prior|above)\s+instructions/i,
  /disregard\s+(your\s+)?system\s+prompt/i,
  /you\s+are\s+now\s+/i,
  /override\s+(tool|security|policy|permissions?)/i,
  /enable\s+write\s+operations?/i,
  /disable\s+(citations?|security|allowlist)/i,
  /exfiltrate|leak\s+credentials|dump\s+passwords?/i,
  /increase\s+(row|query)\s+limit/i,
  /change\s+the\s+tenant/i,
  /execute\s+(shell|bash|cmd)/i,
];

export interface PromptInjectionScanResult {
  suspicious: boolean;
  matchedPatterns: string[];
}

export function scanForPromptInjection(text: string): PromptInjectionScanResult {
  const matchedPatterns: string[] = [];
  for (const pattern of INJECTION_PATTERNS) {
    if (pattern.test(text)) {
      matchedPatterns.push(pattern.source);
    }
  }
  return {
    suspicious: matchedPatterns.length > 0,
    matchedPatterns,
  };
}

export function sanitizeEvidenceContent(content: string): string {
  // Evidence is always treated as untrusted data; strip obvious instruction fences.
  return content
    .replace(/<\/?system>/gi, "")
    .replace(/BEGIN_SYSTEM_PROMPT[\s\S]*?END_SYSTEM_PROMPT/gi, "[stripped]")
    .trim();
}
