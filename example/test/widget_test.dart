import 'package:flutter_test/flutter_test.dart';

import 'package:simple_query_example/main.dart';

void main() {
  testWidgets('renders example actions', (WidgetTester tester) async {
    await tester.pumpWidget(const ExampleApp());

    expect(find.text('Query Contacts'), findsOneWidget);
    expect(find.text('Query Messages (Android only)'), findsOneWidget);
  });
}
