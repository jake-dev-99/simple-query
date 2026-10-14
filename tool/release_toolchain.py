"""Select released stable SDKs from frozen batches and original Deploy evidence."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from release_services import Blocked, PubClient

SDK_FILE = 'tool/release-sdk.json'
SDK_CATALOG = 'https://storage.googleapis.com/flutter_infra_release/releases/releases_linux.json'


# Purpose: Keep only exact released SDK identities in source and workflow metadata.
# /**
#  * @param value The declared Flutter version, framework revision and channel.
#  * @returns The validated stable identity without accepting version ranges.
#  * @throws Blocked for incomplete, experimental or malformed identities.
#  */
def validate_sdk(value):
    if (not isinstance(value, dict) or set(value) != {'version', 'revision', 'channel'}
            or value['channel'] != 'stable'
            or not re.fullmatch(r'\d+\.\d+\.\d+', str(value['version']))
            or not re.fullmatch(r'[0-9a-f]{40}', str(value['revision']))):
        raise Blocked('Packaging SDK must be an exact stable version and framework revision')
    return dict(value)


# Purpose: Turn invalid serialized SDK metadata into an explicit release blocker.
# /**
#  * @param text The serialized source or workflow SDK identity.
#  * @returns Its validated exact stable SDK identity.
#  * @throws Blocked when JSON or SDK identity is malformed.
#  */
def decode_sdk(text):
    try:
        return validate_sdk(json.loads(text))
    except (ValueError, TypeError) as error:
        raise Blocked('Packaging SDK metadata is not valid JSON') from error


# Purpose: Verify the executable before compiling or publishing frozen source.
# /**
#  * @param options Named binary, directory and process environment options.
#  * @returns The actual stable version and full framework revision.
#  * @throws Blocked when the configured SDK cannot prove its stable identity.
#  */
def inspect_sdk(options):
    try:
        result = subprocess.run([options['binary'], '--version', '--machine'],
                                cwd=options['directory'], env=options['env'],
                                text=True, capture_output=True, timeout=120)
        if result.returncode:
            raise Blocked('Could not inspect the configured packaging SDK')
        value = json.loads(result.stdout)
        return validate_sdk({'version': value['frameworkVersion'],
                             'revision': value['frameworkRevision'], 'channel': value['channel']})
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        raise Blocked('Configured packaging SDK has no valid stable identity') from error


# Purpose: Distinguish the latest stable cut SDK from a historical packaging SDK.
# /**
#  * @param catalog The official Linux release catalog used by Flutter setup.
#  * @param version An exact historical version, or None for current stable.
#  * @returns Its released stable version and framework revision.
#  * @throws Blocked when no unique stable release supports that identity.
#  */
def released_sdk(catalog, version=None):
    if not isinstance(catalog, dict):
        raise Blocked('Flutter release catalog is unavailable')
    current = (catalog.get('current_release') or {}).get('stable')
    matches = {(item.get('version'), item.get('hash')) for item in catalog.get('releases', [])
               if item.get('channel') == 'stable'
               and (item.get('version') == version if version is not None else item.get('hash') == current)}
    if len(matches) != 1:
        raise Blocked('Packaging SDK has no unique official stable release')
    selected_version, revision = matches.pop()
    return validate_sdk({'version': selected_version, 'revision': revision, 'channel': 'stable'})


# Purpose: Recover SDK provenance from source-matched original publication runs.
# /**
#  * @param context The frozen plan, GitHub client and official SDK catalog.
#  * @returns The one released stable SDK used by the original Deploy runs.
#  * @throws Blocked on missing, mixed, unfinished or nonmatching run evidence.
#  */
def sdk_from_original_deploy(context):
    plan, journal = context['plan'], context['journal']
    versions = set()
    for package in plan['packages']:
        run = journal.run(package, plan['prepared_sha'])
        recorded_run = package.get('run_id')
        if run is None and recorded_run is None:
            continue
        if (not run or (recorded_run is not None and run['id'] != recorded_run)
                or run['status'] != 'completed'
                or run['conclusion'] != 'success'):
            raise Blocked('Original Deploy SDK proof does not match a completed tag-push run')
        found = set(re.findall(r'/flutter/stable-(\d+\.\d+\.\d+)-(?:x64|arm64)(?:/|\b)',
                               journal.logs(run['id'])))
        if len(found) != 1:
            raise Blocked('Original Deploy SDK proof is missing or ambiguous')
        versions.update(found)
    if len(versions) != 1:
        raise Blocked('Original batch has no single proven packaging SDK')
    return released_sdk(context['catalog'], versions.pop())


# Purpose: Keep receipt reconstruction bound to the batch rather than moving stable.
# /**
#  * @param context The frozen batch, original-run service and official catalog.
#  * @returns The exact SDK pinned in source or proven by original Deploy records.
#  * @throws Blocked when pin, metadata, release catalog or original proof disagree.
#  */
def resolve_batch_sdk(context):
    plan = context['plan']
    recorded = validate_sdk(plan['packaging_sdk']) if 'packaging_sdk' in plan else None
    repository = context.get('repository')
    if not plan.get('prepared_sha'):
        if repository is None or repository.intent_sdk(plan) != recorded:
            raise Blocked('Journal SDK differs from immutable batch intent')
        pinned = recorded
    else:
        committed = repository.prepared_sdk(plan) if repository is not None else None
        frozen_pin = plan.get('files', {}).get(SDK_FILE)
        if committed is not None:
            if recorded != committed or frozen_pin is None or decode_sdk(frozen_pin) != committed:
                raise Blocked('Frozen SDK journal differs from committed prepared source')
            pinned = committed
        elif frozen_pin is not None:
            raise Blocked('Frozen SDK journal pin is absent from committed prepared source')
        else:
            pinned = sdk_from_original_deploy(context)
            if recorded is not None and recorded != pinned:
                raise Blocked('Recorded SDK differs from original Deploy evidence')
    if released_sdk(context['catalog'], pinned['version']) != pinned:
        raise Blocked('Packaging SDK revision differs from its official release')
    return pinned


# Purpose: Avoid SDK reconstruction when cancelled preparation never reached a tag.
# /**
#  * @param context The prior plan and read-only repository tag boundary.
#  * @returns Whether original publication SDK evidence is required.
#  * @throws Blocked if a remote package tag differs from prepared source.
#  */
def needs_batch_sdk(context):
    plan = context.get('plan')
    if plan is None or plan['status'] == 'complete' or context['operation'] == 'cancel':
        return False
    if plan['status'] != 'cancelled':
        return True
    if plan.get('prepared_sha') is None:
        return False
    repository = context.get('repository')
    required = False
    for package in plan['packages']:
        tagged = repository.remote_tag_sha(package['tag']) if repository is not None else None
        if tagged is not None and tagged != plan['prepared_sha']:
            raise Blocked('Cancelled package tag differs from frozen source')
        evidence = (tagged is not None or package['state'] in ('tagged', 'published')
                    or package.get('run_id') is not None)
        if not evidence or package['state'] == 'aborted':
            continue
        pub = context.get('pub')
        if pub is not None:
            run = context['journal'].run(package, plan['prepared_sha'])
            failed = run is not None and run['status'] == 'completed' and run['conclusion'] != 'success'
            if failed and pub.receipt(package['name'], package['version']) is None:
                continue
        required = True
    return required


# Purpose: Let a fresh current-stable cut also reconcile an older frozen batch.
# /**
#  * @param context The operation, latest authorized batch, catalog and GitHub client.
#  * @returns Separate current and batch SDK identities plus source-bound context.
#  * @throws Blocked if the prior batch cannot prove its packaging SDK.
#  */
def resolve_workflow_sdk(context):
    plan = context.get('plan')
    previous = needs_batch_sdk(context)
    batch = resolve_batch_sdk(context) if previous else None
    current = released_sdk(context['catalog']) if context['operation'] == 'cut' or batch is None else batch
    batch_context = {}
    if previous:
        batch_context = {'batch_tag': plan['batch_tag'], 'source_sha': plan['source_sha'],
                         'prepared_sha': plan.get('prepared_sha'), 'sdk': batch}
    return {'current': current, 'batch': batch or current, 'context': batch_context}


# Purpose: Inspect immutable tags with their original committed or proven packaging SDK.
# /**
#  * @param context The selected tag, checked-out repository, original runs and catalog.
#  * @returns The SDK bound to that exact tag commit, including historical pre-pin source.
#  * @throws Blocked when the checkout or original packaging evidence cannot be proven.
#  */
def inspection_sdk(context):
    repository = context['repository']
    sha = repository.git('rev-parse', 'HEAD')
    tagged = repository.git('rev-parse', 'refs/tags/' + context['tag'] + '^{commit}')
    if sha != tagged:
        raise Blocked('Inspection checkout differs from selected tag')
    identity = repository.prepared_sdk({'prepared_sha': sha})
    if identity is None:
        plan = {'prepared_sha': sha, 'packages': [{'tag': context['tag']}]}
        identity = sdk_from_original_deploy({**context, 'plan': plan})
    if released_sdk(context['catalog'], identity['version']) != identity:
        raise Blocked('Inspection SDK differs from its official stable release')
    return identity


# Purpose: Configure exact SDK setup without mutating journals or release tags.
# /**
#  * @param None Workflow metadata comes from the existing trusted environment.
#  * @returns None; writes validated SDK selections to GitHub output/environment files.
#  * @throws Blocked on invalid source pins or unprovable original SDK identity.
#  */
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--deploy', action='store_true')
    parser.add_argument('--inspect', action='store_true')
    parser.add_argument('--source-root', type=Path)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    root = args.source_root or Path(__file__).resolve().parents[1]
    operation = 'resume' if os.environ.get('EVENT_NAME') == 'schedule' else os.environ.get('OPERATION') or 'cut'
    if operation == 'cancel' and not (args.deploy or args.inspect or args.verify):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
            output.write('current_version=\nbatch_version=\n')
        return
    if args.verify:
        expected = decode_sdk(os.environ['RELEASE_CURRENT_SDK'])
        actual = inspect_sdk({'binary': 'flutter', 'directory': root, 'env': dict(os.environ)})
        if actual != expected:
            raise Blocked('Installed SDK differs from the selected version and framework revision')
        print('Verified stable packaging SDK: ' + json.dumps(actual), flush=True)
        return
    pub = PubClient()
    catalog = pub.get(SDK_CATALOG)
    if args.inspect:
        from release_github import GitHubClient
        from release_repository import Repository
        identity = inspection_sdk({'repository': Repository(root), 'tag': os.environ['INSPECT_TAG'],
                                   'journal': GitHubClient(os.environ['GITHUB_REPOSITORY'], os.environ['GH_TOKEN']),
                                   'catalog': catalog})
        selection = {'current': identity, 'batch': identity, 'context': {}}
    elif args.deploy:
        identity = decode_sdk((root / SDK_FILE).read_text())
        if released_sdk(catalog, identity['version']) != identity:
            raise Blocked('Committed publication SDK is not its official stable release')
        selection = {'current': identity, 'batch': identity, 'context': {}}
    else:
        from release_github import GitHubClient
        from release_repository import Repository
        journal = GitHubClient(os.environ['GITHUB_REPOSITORY'], os.environ['GH_TOKEN'])
        plan = journal.load()
        repository = Repository(root)
        orphan = repository.recover_intent(plan)
        if orphan is not None:
            plan = orphan
        operation = 'resume' if os.environ.get('EVENT_NAME') == 'schedule' else os.environ.get('OPERATION') or 'cut'
        selection = resolve_workflow_sdk({'plan': plan, 'journal': journal,
                                          'repository': repository,
                                          'catalog': catalog, 'pub': pub, 'operation': operation})
    with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
        output.write(f'current_version={selection["current"]["version"]}\n')
        output.write(f'batch_version={selection["batch"]["version"]}\n')
    with open(os.environ['GITHUB_ENV'], 'a') as environment:
        environment.write('RELEASE_CURRENT_SDK=' + json.dumps(selection['current']) + '\n')
        environment.write('RELEASE_BATCH_SDK_CONTEXT=' + json.dumps(selection['context']) + '\n')


if __name__ == '__main__':
    main()
