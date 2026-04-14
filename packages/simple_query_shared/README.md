# simple_query_shared

Shared infrastructure for non-Android [`simple_query`](https://pub.dev/packages/simple_query) platform backends.

Provides:

- **`LocalFileSystemFallback`** — filesystem-based query/mutate/observe for files and media domains, with path traversal protection and async enumeration
- **`BaseNonAndroidSimpleQueryPlatform`** — abstract base that wires together a native Pigeon bridge and the filesystem fallback, with try-native-then-fallback routing
- **`NonAndroidNativeBridge`** — handles Pigeon method channel communication, platform exception mapping, and observer lifecycle management
- **`DarwinQueryPlatform`** — shared iOS/macOS base class with common capabilities, extension handlers, and namespace routing

Platform packages (`simple_query_ios`, `simple_query_macos`, `simple_query_windows`, `simple_query_linux`) depend on this package. You don't need to depend on it directly.
