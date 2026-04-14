## 0.2.1

- Bump `simple_permissions_native` dependency constraint from `^0.0.1` to `^1.2.0` to match the current published version. No API changes.

## 0.2.0

- Initial public release.
- Typed query API with 7 domains: contacts, media, files, calendar, messages, calls, platformSpecific.
- Cursor-based and offset-based pagination.
- Capability-driven design via `getCapabilities()`.
- Typed record classes: `ContactRecord`, `CalendarEventRecord`, `MediaRecord`, `FileRecord`, `MessageRecord`, `CallRecord`.
- Structured error model with 5 error codes.
