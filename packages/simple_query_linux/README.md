# simple_query_linux

Linux platform backend for `simple_query`.

Current implementation:

- `files` and `media` domains: local filesystem query/mutate/observe/binary support
- Restricted domains (`messages`, `calls`) return deterministic `SimpleQueryErrorCode.notSupported`
- `contacts` and `calendar` are read-only (contacts/calendar via native backend)
- `linux.tracker` and `linux.xdg` extension methods provide diagnostic metadata
- Batch fallback semantics are ordered and `sequentialBestEffort`, not atomic

## Generated Linux bindings

Linux uses Pigeon's GObject backend. The package pins Pigeon 22.7.4, whose
GObject generator transfers `GTask` and `GAsyncResult` ownership incorrectly
for asynchronous Flutter API calls. `tool/generate_pigeon.dart` copies that
resolved package into a temporary directory, validates the exact upstream
source fragments, applies the ownership correction to the temporary generator,
and then regenerates the checked-in Dart, C header, and C implementation. It
never edits the shared pub cache and fails closed if the pinned source drifts.

After `flutter pub get`, regenerate the bindings from this package directory:

```sh
dart run tool/generate_pigeon.dart
```

The wrapper formats generated Dart with language version 3.6 so the checked-in
output is byte-reproducible across supported Flutter SDKs. The repository-level
`tool/regen_pigeon.sh` delegates Linux generation to the same wrapper.

## Native tests

Run the Linux behavioral suite from this package directory:

```sh
bash tool/run_native_tests.sh
```

The runner uses `FLUTTER_ROOT` when set; otherwise it finds the Flutter SDK from
the `flutter` executable on `PATH`. It requires `clang++`, `pkg-config`, and the
GTK 3 development packages. It always builds the no-EDS variant and also builds
the Evolution Data Server variant when `libebook-1.2`, `libecal-2.0`, and
`libedataserver-1.2` are available through `pkg-config`.
