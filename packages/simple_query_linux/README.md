# simple_query_linux

Linux platform backend for `simple_query`.

Current implementation:

- `files` and `media` domains: local filesystem query/mutate/observe/binary support
- Restricted domains (`messages`, `calls`) return deterministic `SimpleQueryErrorCode.notSupported`
- `contacts` and `calendar` are read-only (contacts/calendar via native backend)
- `linux.tracker` and `linux.xdg` extension methods provide diagnostic metadata
- Batch semantics are ordered and `sequentialBestEffort`, not atomic

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

After `flutter pub get`, run the Linux behavioral suite from this package
directory:

```sh
bash tool/run_native_tests.sh
```

The runner uses `FLUTTER_ROOT` when set; otherwise it finds the Flutter SDK from
the `flutter` executable on `PATH`. It requires `clang++`, `pkg-config`, and the
GTK 3 development packages. It always builds the no-EDS variant and also builds
the Evolution Data Server variant when `libebook-1.2`, `libecal-2.0`, and
`libedataserver-1.2` are available through `pkg-config`. The no-EDS run captures
the exact generated native bytes for an observer event and batch response; the
runner then replays those bytes through the generated Dart channels and shared
payload decoder. This cross-language replay is part of the default suite and
fails if native and Dart wire contracts diverge.

For a native-only diagnostic run, such as compiling against a read-only source
mount, set `SIMPLE_QUERY_NATIVE_ONLY=1`. That mode deliberately skips the Dart
wire-contract replay and is not a substitute for the default verification.

The native implementation is split into private helper, query, mutation,
observer, and registration translation units. CMake and the native runner
compile those same production sources separately, so the tests also verify
their real linkage rather than including implementation files into the test.

## Flutter Linux consumer build

From the repository root on Linux, build the integration fixture:

```sh
bash tool/build_linux_consumer.sh
```

This requires Flutter's Linux artifacts, `clang`, `cmake`, `ninja`, `pkg-config`,
and GTK 3 development packages. The fixture creates a temporary Flutter Linux
application, selects the current local Linux package, and overrides its shared
and platform-interface dependencies with the local workspace packages. It
builds the real Flutter runner and verifies the generated plugin registrar,
then removes the fixture without editing the checkout. Continuous integration
runs this build alongside the native behavioral suite. This proves compilation
and registration wiring, not application deployment or runtime behavior.
