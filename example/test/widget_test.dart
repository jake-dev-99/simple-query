import 'package:flutter_test/flutter_test.dart';

import 'package:simple_query_example/main.dart';

void main() {
  testWidgets('renders the full set of example actions',
      (WidgetTester tester) async {
    await tester.pumpWidget(const ExampleApp());
    // First frame only — async capability load hasn't resolved yet.
    await tester.pump();

    // One labelled button per showcased API (see main.dart).
    expect(find.textContaining('Typed builder: contacts'), findsOneWidget);
    expect(
      find.textContaining('Paginated stream: messages'),
      findsOneWidget,
    );
    expect(
      find.textContaining('Mutate: insert calendar event'),
      findsOneWidget,
    );
    expect(find.textContaining('Observe stream: files'), findsOneWidget);
    expect(
      find.textContaining('BinaryContent: read file bytes'),
      findsOneWidget,
    );
    expect(find.textContaining('callExtension'), findsOneWidget);
    expect(find.textContaining('queryRaw'), findsOneWidget);
    expect(find.text('Refresh capabilities'), findsOneWidget);
  });
}
