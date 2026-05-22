---
name: simple_query checks permissions but never depends on simple_permissions
description: Codebase rule — simple_query is permission-aware (it checks state and fails fast with permissionDenied) but does NOT request permissions or depend on simple_permissions. Permission requests are the developer's job.
type: feedback
---
simple_query is a data-access library. It must:

1. **Check** whether a permission is granted before hitting the native data
   API (`ContentResolver.query`, `CNContactStore.enumerateContacts`,
   `EKEventStore.events(matching:)`, etc.).
2. **Fail fast** when not granted: throw
   `SimpleQueryError(code: permissionDenied, details: {permission: '<os-specific-name>', domain: '<QueryDomain.name>', operation: '<read|write|...>'})`.
3. Hold its own inline catalog of which OS permission corresponds to each
   `(authority, write)` pair — Android permission strings, Apple
   `*authorizationStatus(for:)` calls, etc. Owns the mapping.

simple_query must NOT:

- Depend on `simple_permissions` or `simple_permissions_native`. Past
  guidance was the opposite; this rule is **inverted** intentionally.
- Request permissions. No dialogs, no intents, no async waiting on grant.
  The developer chooses how to request — directly via
  `ActivityCompat.requestPermissions`, via `simple_permissions`, via
  `permission_handler`, doesn't matter — and retries the simple_query call
  after the grant succeeds.
- Surface `simple_permissions` types in its public API.

The win: deterministic `permissionDenied` errors (vs the inconsistent mix
of `SecurityException` / null cursor / empty result that
`ContentResolver` produces today), with the exact OS permission identifier
the caller needs to request next. Zero coupling between simple_query and
any specific permission framework.

When auditing PRs in this repo: flag any new `simple_permissions*` import
inside `packages/simple_query*/`, and any new direct call to
`ActivityCompat.requestPermissions`, `*requestAccess`,
`*requestAuthorization`, or any UI-facing permission prompt.
