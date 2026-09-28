export interface ServiceNowInstanceQueryInput {
  table: string;
  encodedQuery: string;
  fields: string[];
  limit: number;
  purpose: string;
}

export interface ServiceNowInstanceQueryResult {
  table: string;
  rows: Record<string, unknown>[];
  redactedFields: string[];
  rowCount: number;
  taskScoped: boolean;
  persisted: false;
  queryLatencyMs: number;
}

export interface AuthorizedInstanceQueryContext {
  taskId: string;
  allowLiveInstance: boolean;
  projectRoot?: string;
}
