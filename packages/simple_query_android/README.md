# simple_query_android

Android implementation for the [`simple_query`](https://pub.dev/packages/simple_query) federated plugin.

Provides full CRUD access to device data via Android's `ContentResolver`:

- **contacts** — read/write/observe via `content://com.android.contacts`
- **media** — read/write/observe/stream via MediaStore
- **files** — read/write/observe/stream via MediaStore file provider
- **calendar** — read/write/observe via CalendarProvider
- **messages** — read/write/observe/stream via SMS/MMS providers
- **calls** — read/write/observe via CallLog

Also supports:
- Atomic batch operations via `ContentResolver.applyBatch()`
- Binary content streaming via `ParcelFileDescriptor` pipes
- ContentObserver-based change notification
- Cursor-based and offset-based pagination
- Permission integration via `simple_permissions_native`

This package is automatically included when you depend on `simple_query`.
