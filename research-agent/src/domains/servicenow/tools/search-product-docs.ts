import type { HttpServiceNowDocsProvider } from "../providers/servicenow-docs-provider.js";
import type { ServiceNowDocsSearchInput } from "../types.js";
import type { ServiceNowEvidence } from "../schemas/evidence.js";

export async function searchProductDocs(options: {
  provider: HttpServiceNowDocsProvider;
  input: ServiceNowDocsSearchInput;
}): Promise<ServiceNowEvidence[]> {
  return options.provider.search(options.input);
}
