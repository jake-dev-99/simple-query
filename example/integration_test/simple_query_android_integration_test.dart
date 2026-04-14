import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:simple_query/simple_query.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  bool isExpectedOperationalError(SimpleQueryError error) {
    return error.code == SimpleQueryErrorCode.permissionDenied ||
        error.code == SimpleQueryErrorCode.unavailable ||
        error.code == SimpleQueryErrorCode.notSupported ||
        error.code == SimpleQueryErrorCode.transientFailure;
  }

  testWidgets('capability snapshot is exhaustive', (tester) async {

    final capabilities = await SimpleQuery.instance.getCapabilities();
    final capabilityDomains = capabilities.capabilities
        .map((descriptor) => descriptor.domain)
        .toSet();

    expect(capabilityDomains, QueryDomain.values.toSet());
    expect(capabilities.capabilities, isNotEmpty);
  });

  testWidgets('query path executes with typed result or mapped error', (
    tester,
  ) async {

    try {
      final response = await SimpleQuery.instance.query(
        const QueryRequest(
          domain: QueryDomain.files,
          page: QueryPage(limit: 1, offset: 0),
        ),
      );

      expect(response, isA<QueryResult>());
      expect(response.records, isA<List<Map<String, Object?>>>());
    } on SimpleQueryError catch (error) {
      expect(isExpectedOperationalError(error), isTrue);
    }
  });

  testWidgets('batch path executes with mapped outcome', (tester) async {


    try {
      final result = await SimpleQuery.instance.batch(
        const BatchRequest(
          operations: <MutationRequest>[
            MutationRequest(
              domain: QueryDomain.files,
              type: MutationType.delete,
              filters: <QueryFilterCondition>[
                QueryFilterCondition(
                  field: 'path',
                  operator: QueryFilterOperator.equals,
                  value: '/simple_query/non_existent_target.txt',
                ),
              ],
            ),
          ],
        ),
      );
      expect(result, isA<BatchResult>());
    } on SimpleQueryError catch (error) {
      expect(isExpectedOperationalError(error), isTrue);
    }
  });

  testWidgets('binary open/close call path', (tester) async {


    BinaryContentHandle? handle;
    try {
      handle = await SimpleQuery.instance.openBinary(
        const BinaryRequest(
          domain: QueryDomain.files,
          recordId: '/simple_query/non_existent_binary_target.bin',
        ),
      );
      expect(handle, isA<BinaryContentHandle>());
      await SimpleQuery.instance.closeBinary(handle.handleId);
    } on SimpleQueryError catch (error) {
      expect(isExpectedOperationalError(error), isTrue);
    } finally {
      if (handle != null) {
        try {
          await SimpleQuery.instance.closeBinary(handle.handleId);
        } catch (_) {
          // Ignore duplicate close failures.
        }
      }
    }
  });

  testWidgets('observe subscribe + cancel lifecycle', (tester) async {


    StreamSubscription<ObserveEvent>? subscription;
    try {
      subscription = SimpleQuery.instance
          .observe(const ObserveRequest(domain: QueryDomain.files))
          .listen((event) {
        expect(event, isA<ObserveEvent>());
      });

      await Future<void>.delayed(const Duration(milliseconds: 100));
      await subscription.cancel();
    } on SimpleQueryError catch (error) {
      expect(isExpectedOperationalError(error), isTrue);
    } finally {
      await subscription?.cancel();
    }
  });
}
