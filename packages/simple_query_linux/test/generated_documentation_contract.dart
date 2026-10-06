import 'package:flutter_test/flutter_test.dart';

typedef GeneratedDocumentationInventory = ({
  int functions,
  int structures,
});

/// Purpose: Read the JSDoc block immediately preceding one generated
/// definition so unrelated earlier comments cannot satisfy the contract.
///
/// @param lines contains the generated source split into physical lines.
/// @param definitionLine is the zero-based definition line.
/// @returns The adjacent JSDoc block, or null when none exists.
/// @throws Nothing.
String? _documentationBefore(List<String> lines, int definitionLine) {
  var end = definitionLine - 1;
  while (end >= 0 && lines[end].trim().isEmpty) {
    end -= 1;
  }
  if (end < 0 || !lines[end].trim().endsWith('*/')) {
    return null;
  }
  var start = end;
  while (start >= 0 && !lines[start].trimLeft().startsWith('/**')) {
    start -= 1;
  }
  if (start < 0) {
    return null;
  }
  return lines.sublist(start, end + 1).join('\n');
}

/// Purpose: Extract declared parameter names from a generated function
/// signature for one-to-one documentation checks.
///
/// @param signature is a complete generated function-definition line.
/// @returns Parameter names in declaration order.
/// @throws [StateError] when a parameter declaration has no usable name.
List<String> _parameterNames(String signature) {
  final opening = signature.indexOf('(');
  final closing = signature.lastIndexOf(')');
  final parameters = signature.substring(opening + 1, closing).trim();
  if (parameters.isEmpty || parameters == 'void') {
    return const <String>[];
  }
  return parameters.split(',').map((parameter) {
    final match = RegExp(r'([A-Za-z_]\w*)\s*$').firstMatch(parameter);
    if (match == null) {
      throw StateError('Cannot identify parameter in: $signature');
    }
    return match.group(1)!;
  }).toList(growable: false);
}

/// Purpose: Reassemble a top-level generated function header so wrapped API
/// setup, send, and finish definitions cannot escape the inventory.
///
/// @param lines contains the generated source split into physical lines.
/// @param definitionLine is the zero-based line containing the opening brace.
/// @returns The complete signature and its first line, or null for nonfunctions.
/// @throws Nothing.
({String signature, int startLine})? _functionHeader(
  List<String> lines,
  int definitionLine,
) {
  final parts = <String>[];
  var parenthesisBalance = 0;
  for (var index = definitionLine; index >= 0; index -= 1) {
    final part = lines[index].trim();
    parts.insert(0, part);
    parenthesisBalance += ')'.allMatches(part).length;
    parenthesisBalance -= '('.allMatches(part).length;
    final signature = parts.join(' ');
    if (signature.contains('(') && parenthesisBalance == 0) {
      final match = RegExp(
        r'([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{$',
      ).firstMatch(signature);
      if (match == null) {
        return null;
      }
      return (signature: signature, startLine: index);
    }
    if (index != definitionLine &&
        (part.endsWith(';') || part.endsWith('}') || part.endsWith('*/'))) {
      return null;
    }
  }
  return null;
}

/// Purpose: Enforce the complete documentation shape for one generated
/// definition and each supplied API parameter or structure state anchor.
///
/// @param contract names the definition, supplies its adjacent documentation,
/// and lists API parameters or state anchors requiring descriptions.
/// @returns Nothing.
/// @throws A test failure when required documentation is absent or incomplete.
void _expectCompleteDocumentation(
  ({
    String? documentation,
    String definition,
    List<String> parameters,
  }) contract,
) {
  final (:documentation, :definition, :parameters) = contract;
  expect(documentation, isNotNull, reason: '$definition has no JSDoc block.');
  final block = documentation!;
  expect(
    block,
    matches(RegExp(r'Purpose:\s+\S')),
    reason: '$definition has no meaningful Purpose.',
  );
  if (parameters.isEmpty) {
    expect(
      block,
      contains('@param None.'),
      reason: '$definition must document that it accepts no parameters.',
    );
  } else {
    for (final parameter in parameters) {
      expect(
        block,
        contains('@param $parameter '),
        reason: '$definition does not document $parameter.',
      );
    }
  }
  expect(
    block,
    matches(RegExp(r'@returns\s+\S')),
    reason: '$definition has no return contract.',
  );
  expect(
    block,
    matches(RegExp(r'@throws\s+\S')),
    reason: '$definition has no error contract.',
  );
}

/// Purpose: Inventory every top-level generated function, class, and struct
/// definition and enforce its adjacent CQS-CLR-04 documentation contract.
///
/// @param source is the generated GObject implementation under verification.
/// @returns Counts proving the inventory found both functions and structures.
/// @throws A test failure for unclassified or incompletely documented output.
GeneratedDocumentationInventory expectGeneratedDocumentation(String source) {
  final lines = source.split('\n');
  var depth = 0;
  var functions = 0;
  var structures = 0;
  for (var index = 0; index < lines.length; index += 1) {
    final line = lines[index];
    final trimmed = line.trim();
    if (depth == 0 && trimmed.endsWith('{')) {
      final structureMatch = RegExp(
        r'^(?:struct|class)\s+([A-Za-z_]\w*)\s*\{$',
      ).firstMatch(trimmed);
      final functionHeader = _functionHeader(lines, index);
      if (structureMatch != null) {
        final name = structureMatch.group(1)!;
        _expectCompleteDocumentation((
          documentation: _documentationBefore(lines, index),
          definition: name,
          parameters: const <String>['parent_instance'],
        ));
        structures += 1;
      } else if (functionHeader != null) {
        final name = RegExp(
          r'([A-Za-z_]\w*)\s*\(',
        ).allMatches(functionHeader.signature).last.group(1)!;
        _expectCompleteDocumentation((
          documentation: _documentationBefore(
            lines,
            functionHeader.startLine,
          ),
          definition: name,
          parameters: _parameterNames(functionHeader.signature),
        ));
        functions += 1;
      } else {
        fail('Unclassified generated top-level definition: $trimmed');
      }
    }
    depth += '{'.allMatches(line).length;
    depth -= '}'.allMatches(line).length;
    expect(depth, greaterThanOrEqualTo(0),
        reason: 'Unbalanced generated code.');
  }
  expect(depth, 0, reason: 'Generated code has unbalanced definitions.');
  expect(functions, greaterThan(0), reason: 'No generated functions found.');
  expect(structures, greaterThan(0), reason: 'No generated structures found.');
  return (functions: functions, structures: structures);
}
