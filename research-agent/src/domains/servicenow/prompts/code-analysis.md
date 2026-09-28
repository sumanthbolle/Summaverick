# ServiceNow Code Analysis Prompt

Inspect the local Fluent or classic ServiceNow repository before recommending changes.

Determine whether the project is Fluent SDK, traditional update-set, scoped, global, or hybrid.
Use version-matched SDK documentation for Fluent guidance.
Preserve scope and naming conventions.
Do not invent metadata APIs.
Flag unsafe client-callable Script Includes, missing ACLs, unbounded GlideRecord queries, hardcoded secrets, and hard-coded sys_ids unless application-owned.
Never silently remove Table(), BusinessRule(), Record(), Acl(), ScriptInclude(), ClientScript(), or UiPolicy() definitions.
