"""Deterministic release selection and frozen candidate files (no remote writes)."""
from pathlib import Path
import re
import subprocess

import yaml


def git(root, *args):
    return subprocess.check_output(['git', *args], cwd=root, text=True).strip()


def version(value):
    match = re.fullmatch(r'(\d+)\.(\d+)\.(\d+)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?', str(value))
    if not match:
        raise ValueError(f'Invalid semver: {value}')
    return tuple(map(int, match.groups()))


def bump_version(current, bump):
    major, minor, patch = version(current)
    if bump == 'major':
        return f'{major + 1}.0.0'
    if bump == 'minor':
        return f'{major}.{minor + 1}.0'
    if bump == 'patch':
        return f'{major}.{minor}.{patch + 1}'
    raise ValueError(f'Invalid bump: {bump}')


def floor(constraint):
    # Internal dependencies must use a hosted version, never a path/git link.
    # Accept the ordinary caret, exact, and bounded comparator forms.
    if not isinstance(constraint, str):
        raise ValueError(f'Internal hosted dependency constraint required: {constraint!r}')
    text = constraint.strip()
    if re.fullmatch(r'\^?\d+\.\d+\.\d+', text):
        return version(text.lstrip('^'))
    match = re.fullmatch(r'>=\s*(\d+\.\d+\.\d+)\s+<\s*(\d+\.\d+\.\d+)', text)
    if match and version(match[1]) < version(match[2]):
        return version(match[1])
    raise ValueError(f'Unsupported internal dependency constraint: {constraint!r}')


def changed_manifest(text, next_version, floors):
    text, count = re.subn(r'^version:[^\n]*$', 'version: ' + next_version,
                         text, flags=re.MULTILINE)
    if count != 1:
        raise ValueError('Exactly one pubspec version is required')
    section = None
    lines = []
    changed = set()
    for line in text.splitlines(keepends=True):
        heading = re.match(r'^([A-Za-z_]+):', line)
        if heading:
            section = heading[1]
        dependency = re.match(r'^(  )([A-Za-z_][A-Za-z_0-9]*):\s*([^#\n]*)(.*)', line)
        key = (section, dependency[2]) if dependency else None
        if key in floors:
            ending = '\n' if line.endswith('\n') else ''
            comment = (' ' + dependency[4].strip()) if dependency[4].strip() else ''
            line = f'  {dependency[2]}: ^{floors[key]}{comment}{ending}'
            changed.add(key)
        lines.append(line)
    if changed != set(floors):
        raise ValueError('Dependency entries must be ordinary two-space pubspec mappings')
    return ''.join(lines)


def released_changelog(history, next_version, notes):
    pending = re.search(r'^## \[?Unreleased\]?[ \t]*$', history, re.MULTILINE)
    authored = ''
    if pending:
        following = re.search(r'^## ', history[pending.end():], re.MULTILINE)
        end = pending.end() + following.start() if following else len(history)
        authored = history[pending.end():end].strip()
        history = history[:pending.start()] + history[end:]
    content = '\n\n'.join(part for part in (authored, notes.strip()) if part)
    return f'## {next_version}\n\n{content}\n\n' + history


def owner(relative, packages):
    matches = [(len(path), name) for name, path in packages.items()
               if path == '.' or relative.startswith(path.rstrip('/') + '/')]
    return max(matches)[1] if matches else None


def release_file(relative):
    return (not relative.startswith(('.github/', 'tool/', '.claude/', '.superpowers/'))
            and Path(relative).name not in {'AGENTS.md', 'CLAUDE.md', 'pubspec_overrides.yaml',
                                            'pubspec.lock', 'RELEASE.md'}
            and '/.github/' not in relative)


def selection_reasons(root, packages, target, published, source_sha, specs):
    """Require verified history or hosted manifest evidence for owned changes."""
    baselines, reasons, verified = {}, {}, []
    for name in packages:
        hosted_versions = {v['version'] for v in (published.get(name) or {}).get('versions', [])}
        tags = git(root, 'tag', '--merged', source_sha, '--list', name + '-v*').splitlines()
        tags = [tag for tag in tags if tag.removeprefix(name + '-v') in hosted_versions]
        baselines[name] = max(tags, key=lambda tag: version(tag.removeprefix(name + '-v'))) if tags else None
        verified.extend(tags)
    commits = {git(root, 'rev-parse', tag + '^{commit}') for tag in verified}
    watermark = next((sha for sha in git(root, 'rev-list', source_sha).splitlines() if sha in commits), None)
    for name in packages:
        if target != 'all':
            continue
        baseline = baselines[name]
        metadata = published.get(name)
        if baseline is None and metadata:
            hosted_spec = (metadata.get('latest') or {}).get('pubspec')
            if hosted_spec and hosted_spec != specs[name]:
                reasons[name] = 'source: manifest differs from hosted version without a matching tag'
                continue
            raise ValueError(f'{name} has no verified package baseline; attention required before all selection')
        if baseline is None:
            if watermark is None:
                raise ValueError(f'{name} needs its first manual publication and a verified repository baseline')
            baseline = watermark
        files = git(root, 'diff', '--name-only', baseline, source_sha).splitlines()
        if any(owner(file, packages) == name and release_file(file) for file in files):
            reasons[name] = 'source: changed since ' + baseline
    if target != 'all':
        if target not in packages:
            raise ValueError(f'Unknown package: {target}')
        reasons = {target: 'explicit individual cut'}
    return baselines, reasons


def load_specs(root, packages, source_sha):
    """Read immutable package manifests and their internal dependency graph."""
    specs, texts, graph = {}, {}, {}
    for name, path in packages.items():
        text = git(root, 'show', f'{source_sha}:{path + "/" if path != "." else ""}pubspec.yaml') + '\n'
        spec = yaml.safe_load(text)
        if spec.get('name') != name or spec.get('publish_to') == 'none':
            raise ValueError(f'Not the configured publishable package: {name}')
        # The coordinator verifies receipts on pub.dev only.
        if spec.get('publish_to') not in (None, 'https://pub.dev'):
            raise ValueError(f'{name} publishes outside pub.dev (publish_to: {spec["publish_to"]})')
        version(spec['version'])
        texts[name], specs[name] = text, spec
        graph[name] = {}
        for section in ('dependencies', 'dev_dependencies'):
            for dep, constraint in (spec.get(section) or {}).items():
                if dep in packages:
                    floor(constraint)
                    graph[name].setdefault(dep, {})[section] = constraint
    return texts, specs, graph


def package_order(packages, graph):
    """A full graph cycle check precedes dependency-first selection."""
    # Check the full graph, even if this invocation happens to select a leaf.
    order, visiting, visited = [], set(), set()

    def visit(name):
        if name in visiting:
            raise ValueError(f'Dependency cycle at {name}')
        if name in visited:
            return
        visiting.add(name)
        for dep in sorted(graph[name]):
            visit(dep)
        visiting.remove(name)
        visited.add(name)
        order.append(name)

    for name in sorted(packages):
        visit(name)
    return order


def update_floors(packages, specs, graph, order, reasons, target, bump):
    """Select affected dependents without ever lowering an authored floor."""
    floor_updates = {name: {} for name in packages}
    while True:
        selected_before = set(reasons)
        candidates = {name: bump_version(specs[name]['version'], bump) for name in reasons}
        for name in order:
            for dep, sections in graph[name].items():
                if dep not in candidates:
                    continue
                for section, constraint in sections.items():
                    required = floor(constraint)
                    candidate = version(candidates[dep])
                    if candidate < required:
                        raise ValueError(f'{dep} candidate {candidates[dep]} is below {name} floor {constraint}; refusing downgrade')
                    if candidate > required and (target == 'all' or name in reasons):
                        floor_updates[name][(section, dep)] = candidates[dep]
                        reasons.setdefault(name, 'dependency: requires updated ' + dep + ' floor')
        if set(reasons) == selected_before:
            break
    return floor_updates, candidates


def note_pathspec(path, packages):
    """Paths whose commits are this package's release notes.

    A root package ('.') owns the repository, so the other packages and the
    release machinery are carved out of its history.
    """
    if path != '.':
        return [path]
    others = sorted(p for p in packages.values() if p != '.')
    return ['.', *(f':(exclude){p}' for p in [*others, '.github', 'tool'])]


def build_plan(root, packages, target, bump, published, source_sha):
    root = Path(root)
    packages = {name: str(Path(path)) for name, path in packages.items()}
    texts, specs, graph = load_specs(root, packages, source_sha)
    order = package_order(packages, graph)
    baselines, reasons = selection_reasons(root, packages, target, published, source_sha, specs)
    floor_updates, candidates = update_floors(packages, specs, graph, order, reasons, target, bump)
    files, selected = {}, []
    for name in order:
        if name not in reasons:
            continue
        next_version = bump_version(specs[name]['version'], bump)
        path = packages[name]
        prefix = '' if path == '.' else path + '/'
        notes = git(root, 'log', '--pretty=format:- %s',
                    f'{baselines[name]}..{source_sha}' if baselines[name] else source_sha,
                    '--', *note_pathspec(path, packages)).splitlines()[:50]
        for (_, dep), value in sorted(floor_updates[name].items()):
            notes.append(f'- Require {dep} ^{value}.')
        if not notes:
            notes = ['- Update release version.']
        changelog = root / prefix / 'CHANGELOG.md'
        # Missing changelogs are intentionally supported.
        history = subprocess.run(['git', 'show', f'{source_sha}:{prefix}CHANGELOG.md'],
                                 cwd=root, text=True, capture_output=True)
        files[prefix + 'pubspec.yaml'] = changed_manifest(texts[name], next_version, floor_updates[name])
        files[prefix + 'CHANGELOG.md'] = released_changelog(history.stdout if history.returncode == 0 else '',
                                                          next_version, '\n'.join(notes))
        selected.append({'name': name, 'path': path, 'current': str(specs[name]['version']),
                         'version': next_version, 'tag': f'{name}-v{next_version}',
                         'reason': reasons[name], 'state': 'pending', 'run_id': None,
                         'attempts': 0, 'prerequisites': {dep: candidates[dep] for dep in graph[name] if dep in candidates},
                         'floor_changes': {f'{section}.{dep}': value for (section, dep), value in floor_updates[name].items()}})
    return {'schema': 1, 'source_sha': source_sha, 'prepared_sha': None,
            'status': 'planned', 'not_before': 0, 'error': None,
            'packages': selected, 'files': files, 'package_paths': packages}
