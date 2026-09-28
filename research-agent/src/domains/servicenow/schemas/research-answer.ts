import type { Confidence } from "../../../core/types.js";
import type { EvidenceClaimLink } from "./evidence.js";

export interface ServiceNowResearchAnswer {
  summary: string;
  directAnswer: string;
  explanation?: string;
  recommendedImplementation?: string;
  code?: {
    language: string;
    content: string;
    isComplete: boolean;
  }[];
  warnings: string[];
  assumptions: string[];
  evidence: EvidenceClaimLink[];
  confidence: Confidence;
  releaseFamily?: string;
  sdkVersion?: string;
  requiresInstanceValidation: boolean;
}

export const INSUFFICIENT_EVIDENCE_MESSAGE =
  "I could not confirm this from the available SDK documentation, product documentation or authorized instance data.";
