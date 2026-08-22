import type { HttpServiceNowDocsProvider } from "../providers/servicenow-docs-provider";
import type { ServiceNowDocsSearchInput } from "../types";
import type { ServiceNowEvidence } from "../schemas/evidence";

export async function searchProductDocs(options: {
  provider: HttpServiceNowDocsProvider;
  input: ServiceNowDocsSearchInput;
}): Promise<ServiceNowEvidence[]> {
  return options.provider.search(options.input);
}
