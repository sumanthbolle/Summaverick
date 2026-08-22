import { describe, expect, it } from "vitest";
import { runEvalSuite } from "../src/domain/research/evals/eval-runner";
import { classifyServiceNowIntent } from "../src/domain/research/retrieval/query-classifier";

describe("research eval suite (bundled fixtures)", () => {
  it("meets quality bars on the ported pipeline", () => {
    const s = runEvalSuite();
    // The 62 bundled regression + adversarial cases run through the REAL ported
    // classifier/router/verifier. Guard against regressions in the port.
    expect(s.passRate).toBeGreaterThanOrEqual(0.85);
    expect(s.intentAccuracy).toBeGreaterThanOrEqual(0.85);
    expect(s.adversarialBlockRate).toBeGreaterThanOrEqual(0.85);
  });
});

describe("classifyServiceNowIntent", () => {
  it("classifies a scripting question with confidence", () => {
    const c = classifyServiceNowIntent(
      "How do I use GlideAjax in a client script?"
    );
    expect(c.intent).toBeTruthy();
    expect(c.confidence).toBeGreaterThan(0);
  });
});
