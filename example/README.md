# simple_query example

This example app uses only the `simple_query` facade package and includes
Android integration smoke coverage for the v2 API.

## Requirements

- Flutter SDK configured for Android
- Android emulator or physical Android device
- Device permission and default-app policy may affect data-access results

## Run

```bash
cd example
flutter pub get
flutter run -d <android-device-id>
```

## Integration smoke tests (Android only)

```bash
cd example
flutter test integration_test -d <android-device-id>
```

Expected behavior:
- Tests validate call paths and typed result shapes for v2 operations.
- Operational failures are accepted only when surfaced as `SimpleQueryError`
  with deterministic v2 error codes (`permissionDenied`, `unavailable`,
  `notSupported`, `transientFailure`).
