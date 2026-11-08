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

## Closing binary streams

`closeBinary()` waits up to 5 seconds for a running background copy to stop.
A copy failure that races the close is still raised as its mapped
`SimpleQueryError`. A copy that does not stop in time raises the failure it
recorded so far, with the timeout attached. With none recorded, it raises an
`IOException` (`transientFailure`), because the content may be incomplete. A
failure that copy records later is logged. A JVM `Error` in the copy is not
raised here; it propagates on the copy's own thread.

`dispose()` closes its open binary streams concurrently, so those waits
overlap. It raises the first failure in step order. The plugin detach
(`StreamManager.dispose()`) does not wait; a running copy logs its own failure
when it finishes.
