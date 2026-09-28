export type ServiceNowIntent =
  | "product_concept"
  | "platform_development"
  | "fluent_sdk"
  | "code_review"
  | "implementation_design"
  | "troubleshooting"
  | "instance_schema"
  | "instance_record_lookup"
  | "release_comparison"
  | "configuration_guidance"
  | "security_review"
  | "unknown";

export interface ServiceNowIntentResult {
  domain: "servicenow";
  intent: ServiceNowIntent;
  modules: string[];
  requiresSdkDocs: boolean;
  requiresProductDocs: boolean;
  requiresRepositoryContext: boolean;
  requiresLiveInstance: boolean;
  requestedReleaseFamily?: string;
  confidence: number;
  rationale: string;
}

export const SERVICENOW_MODULES = [
  "itsm",
  "cmdb",
  "itom",
  "irm",
  "grc",
  "spm",
  "csm",
  "hrsd",
  "secops",
  "app_engine",
  "flow_designer",
  "integrationhub",
  "ui_builder",
  "platform",
  "fluent_sdk",
] as const;

export type ServiceNowModule = (typeof SERVICENOW_MODULES)[number];
