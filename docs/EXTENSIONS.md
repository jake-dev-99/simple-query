# Platform Extensions

simple_query's core API (`query`, `mutate`, `observe`, `openBinary`, etc.)
covers the portable surface. Anything **not** portable across platforms
lives behind `callExtension(namespace:, method:, args:)`.

This page is an index. Per-platform details live in
`docs/extensions/<platform>.md`.

## Index

| Platform | Namespace prefix | Details |
| --- | --- | --- |
| Android | `android.*` | [extensions/android.md](extensions/android.md) |
| iOS | `ios.*` | [extensions/ios.md](extensions/ios.md) |
| macOS | `macos.*` | [extensions/macos.md](extensions/macos.md) |
| Windows | `windows.*` | [extensions/windows.md](extensions/windows.md) |
| Linux | `linux.*` | [extensions/linux.md](extensions/linux.md) |

## Usage

Every extension call uses the same facade method:

```dart
final result = await SimpleQuery.instance.callExtension(
  namespace: 'android.provider',
  method: 'queryWithJoins',
  args: <String, Object?>{ /* method-specific */ },
);
// result is Map<String, Object?>? — shape is per-namespace/method.
```

## Diagnostics

Extension responses may include an `implementation` key in metadata:

- `implementation = native_bridge` — result came from a native-backed
  platform API (EventKit, Contacts framework, WinRT, etc.).
- `implementation = filesystem_fallback` — result came from the shared
  filesystem fallback (desktop / iOS/macOS `files` + `media`).
- `implementation = diagnostic_synthetic` — result is a placeholder or
  diagnostic response, not authoritative provider data.

See [API_SEMANTICS.md](API_SEMANTICS.md#extension-semantics) for the full
contract.

## Stability policy

Extension methods are **explicitly non-portable** and may change between
minor versions. Pin to an exact simple_query version if you rely on a
specific extension method shape.

## When to add a new extension

Reach for `callExtension` when:

- The capability is platform-specific and has no sensible cross-platform
  analogue (e.g. Android `ContentResolver.call`, iOS `PHPhotoLibrary`
  media-type listing).
- The feature is experimental and not yet ready for the core contract.

Do **not** reach for `callExtension` when:

- The feature makes sense on 2+ platforms — extend the core contract
  instead (new `QueryDomain`, optional canonical field, etc.).
- You need a custom content URI — use
  [`SimpleQuery.queryRaw`](../packages/simple_query/lib/src/simple_query_api.dart)
  (a first-class entry point, not an extension call).
