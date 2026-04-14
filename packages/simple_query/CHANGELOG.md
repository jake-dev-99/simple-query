## 0.2.0

- Initial public release.
- Federated plugin architecture with per-platform packages.
- Typed query API with 7 domains: contacts, media, files, calendar, messages, calls, platformSpecific.
- Cursor-based and offset-based pagination.
- Capability-driven design via `getCapabilities()`.
- Typed record classes: `ContactRecord`, `CalendarEventRecord`, `MediaRecord`, `FileRecord`, `MessageRecord`, `CallRecord`.
- Structured error model with 5 error codes.
- Cross-platform support: Android (full), iOS/macOS (contacts, calendar, files, media), Windows/Linux (contacts, calendar, files, media).
