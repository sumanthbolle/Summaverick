import type { DefaultServiceNowInstanceQueryProvider } from "../providers/sdk-query-provider";
import type {
  AuthorizedInstanceQueryContext,
  ServiceNowInstanceQueryInput,
} from "../schemas/instance-query";
import type { ServiceNowEvidence } from "../schemas/evidence";

export async function queryInstance(options: {
  provider: DefaultServiceNowInstanceQueryProvider;
  input: ServiceNowInstanceQueryInput;
  context: AuthorizedInstanceQueryContext;
}): Promise<ServiceNowEvidence> {
  const result = await options.provider.query(options.input, options.context);
  return options.provider.toEvidence(result, options.input.purpose);
}
