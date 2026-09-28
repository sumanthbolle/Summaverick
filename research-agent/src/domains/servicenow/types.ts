import type { ServiceNowEvidence } from "./schemas/evidence.js";
import type { ServiceNowIntent } from "./schemas/domain-intent.js";

export interface SdkTopicMatch {
  topic: string;
  score: number;
  snippet?: string;
}

export interface SdkTopicPreview {
  topic: string;
  preview: string;
  documentationVersion: string;
}

export interface SdkProjectOrientation {
  projectRoot: string;
  installedSdkVersion: string | null;
  packageLockHash?: string;
  quickstartTopics: string[];
  fluentLanguageTopics: string[];
  helpText: string;
  keysFileDocs?: string;
  cachedAt: string;
}

export interface SdkEvidenceMetadata {
  installedSdkVersion?: string;
  documentationVersion: string;
  topic: string;
  projectRoot?: string;
  versionMismatch?: boolean;
}

export type PlannedSource =
  | "sdk_explain"
  | "product_docs"
  | "local_repository"
  | "live_instance";

export interface ServiceNowResearchPlan {
  intent: ServiceNowIntent;
  questionsToResolve: string[];
  sources: PlannedSource[];
  requiresLiveInstance: boolean;
  releaseFamily?: string;
  sdkVersion?: string;
  maximumToolCalls: number;
}

export interface ServiceNowDocsIndexEntry {
  title: string;
  url: string;
  publication?: string;
  productArea?: string;
  module?: string;
}

export interface ServiceNowDocsIndex {
  releaseFamily: string;
  fetchedAt: string;
  entries: ServiceNowDocsIndexEntry[];
}

export interface ServiceNowDocumentReference {
  releaseFamily: string;
  pathOrUrl: string;
  title?: string;
}

export interface ServiceNowDocument {
  title: string;
  content: string;
  canonicalUrl?: string;
  releaseFamily: string;
  path: string;
  contentHash: string;
  updatedAt?: string;
}

export interface ServiceNowDocsSearchInput {
  query: string;
  releaseFamily?: string;
  modules?: string[];
  productAreas?: string[];
  limit: number;
}

export interface DocumentationRefreshResult {
  releaseFamily: string;
  indexedCount: number;
  refreshedAt: string;
}

export interface FluentProjectInfo {
  projectRoot: string;
  configPath: string;
  packageJsonPath?: string;
  installedSdkVersion: string | null;
}

export interface DeletionRiskAssessment {
  definitionKind: string;
  filePath: string;
  keysReferenced: string[];
  dependentFiles: string[];
  requiresUserApproval: true;
  impactSummary: string;
  safeToAutoDelete: false;
}

export interface RankedEvidenceBundle {
  evidence: ServiceNowEvidence[];
  unsupportedClaimCount: number;
}
