#!/usr/bin/env python3
"""Hash user-supplied Darien research material; emit metadata, never asset contents."""
import argparse
import fnmatch
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

SCHEMA = 1
PARSERS = {
    '.hpi': ['hpitool'], '.ufo': ['hpitool'], '.ccx': ['hpitool'],
    '.gaf': ['gaftool'], '.tnt': ['tnttool'], '.3do': ['modeltool'],
    '.cob': ['cobtool'], '.tdf': ['tdftool'], '.fbi': ['tdftool'],
    '.ota': ['tdftool'], '.gui': ['tdftool'], '.bik': ['biktool'],
}


def identity(archive, path):
    # Keep container and member separate: a literal '!' in a filename is legal.
    return ((archive or '').casefold(), path.casefold())


def file_type(path, prefix):
    if prefix.startswith(b'HAPI'): return 'hpi'
    if prefix.startswith(b'MZ'): return 'pe-or-dos'
    if prefix.startswith(b'PK\x03\x04'): return 'zip'
    if prefix.startswith(b'\x89PNG\r\n\x1a\n'): return 'png'
    if prefix.startswith(b'BIK'): return 'bink'
    return Path(path).suffix.lower().lstrip('.') or 'unknown'


def digest_stream(stream):
    sha = hashlib.sha256()
    size = 0
    prefix = b''
    while True:
        chunk = stream.read(1024 * 1024)
        if not chunk: break
        if not prefix: prefix = chunk[:16]
        sha.update(chunk)
        size += len(chunk)
    return size, sha.hexdigest(), prefix


def record(path, archive, result):
    size, digest, prefix = result
    kind = file_type(path, prefix)
    parsers = PARSERS.get(Path(path).suffix.lower(), [])
    if kind == 'hpi': parsers = ['hpitool']
    return dict(path=path, archive=archive, size=size, sha256=digest,
                file_type=kind, parser_candidates=parsers)


def archive_entries(tool, path):
    listing = subprocess.run([str(tool), 'list', str(path)], check=True,
                             capture_output=True).stdout.decode('utf-8', errors='strict')
    entries = []
    summary = None
    for line in listing.splitlines():
        match = re.fullmatch(r'\s*(\d+) (.+)', line)
        total = re.fullmatch(r'(\d+) files, (\d+) bytes', line)
        if total:
            if summary is not None: raise ValueError('duplicate HPI summary')
            summary = tuple(map(int, total.groups()))
        elif match:
            entries.append((match[2], int(match[1])))
        elif not re.fullmatch(r'\s*\[.*\]', line):
            raise ValueError(f'unrecognized hpitool list line: {line!r}')
    if summary != (len(entries), sum(size for _, size in entries)):
        raise ValueError('HPI listing count/size mismatch')
    return sorted(entries)


def inventory(root, label, includes=(), members=(), tool=None):
    root = Path(root)
    if root.is_symlink() or not root.exists():
        raise ValueError('source must be an existing, non-symlink file or directory')
    if members and (tool is None or not Path(tool).is_file()):
        raise ValueError('--archive-members requires an existing --hpitool')
    entries, skipped = [], []
    candidates = [root] if root.is_file() else sorted(root.rglob('*'))
    seen = set()
    def add(item):
        key = identity(item['archive'], item['path'])
        if key in seen: raise ValueError(f'case-insensitive path collision: {key}')
        seen.add(key)
        entries.append(item)
    for path in candidates:
        rel = path.name if root.is_file() else path.relative_to(root).as_posix()
        if includes and not any(fnmatch.fnmatchcase(rel.casefold(), p.casefold()) for p in includes): continue
        if path.is_symlink():
            skipped.append(dict(path=rel, reason='symlink not followed'))
            continue
        if path.is_dir(): continue
        if not path.is_file(): raise ValueError(f'unsupported source file: {rel}')
        before = path.stat()
        with path.open('rb') as stream: item = record(rel, None, digest_stream(stream))
        add(item)
        if any(fnmatch.fnmatchcase(rel.casefold(), p.casefold()) for p in members):
            if item['file_type'] != 'hpi': raise ValueError(f'not an HPI archive: {rel}')
            for member, expected in archive_entries(tool, path):
                # cat writes only to a pipe; member paths are never extracted to disk.
                with subprocess.Popen([str(tool), 'cat', str(path), member], stdout=subprocess.PIPE,
                                      stderr=subprocess.DEVNULL) as child:
                    result = digest_stream(child.stdout)
                    if child.wait() != 0: raise ValueError(f'hpitool cat failed: {rel}!{member}')
                if result[0] != expected: raise ValueError(f'HPI member size mismatch: {rel}!{member}')
                add(record(member, rel, result))
        after = path.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError(f'source changed while hashing: {rel}')
    return dict(schema=SCHEMA, label=label,
                scope=dict(include=sorted(includes), archive_members=sorted(members)),
                entries=sorted(entries, key=lambda e: identity(e['archive'], e['path'])), skipped=skipped)


def index_manifest(manifest):
    if manifest.get('schema') != SCHEMA: raise ValueError('unsupported inventory schema')
    result = {}
    for entry in manifest['entries']:
        if (not isinstance(entry['path'], str) or
            not (entry['archive'] is None or isinstance(entry['archive'], str)) or
            not isinstance(entry['size'], int) or entry['size'] < 0 or
            not re.fullmatch(r'[0-9a-f]{64}', entry['sha256'])):
            raise ValueError('invalid inventory entry')
        key = identity(entry['archive'], entry['path'])
        if key in result: raise ValueError('duplicate inventory identity')
        result[key] = entry
    return result


def compare(left, right):
    a, b = index_manifest(left), index_manifest(right)
    changed, spelling, unchanged = [], [], 0
    for key in sorted(a.keys() & b.keys()):
        x, y = a[key], b[key]
        if (x['size'], x['sha256']) != (y['size'], y['sha256']):
            changed.append(dict(before=x, after=y))
        else: unchanged += 1
        if (x['archive'], x['path']) != (y['archive'], y['path']):
            spelling.append(dict(before=x, after=y))
    return dict(schema=SCHEMA, left=left['label'], right=right['label'],
                same_scope=left['scope'] == right['scope'],
                skipped_sources=bool(left['skipped'] or right['skipped']),
                added=[b[k] for k in sorted(b.keys()-a.keys())],
                removed=[a[k] for k in sorted(a.keys()-b.keys())],
                changed=changed, spelling_changes=spelling, unchanged=unchanged)


def markdown(report):
    def escape(value): return str(value).replace('|', '\\|').replace('\n', ' ').replace('\r', ' ')
    lines = ['# Local artifact comparison', '',
             f"Left: {escape(report['left'])}; right: {escape(report['right'])}.", '',
             'This compares observed bytes, not historical distribution identity or effective archive precedence.', '',
             f"Same scope: {report['same_scope']}. Skipped sources: {report['skipped_sources']}.", '',
             f"Unchanged entries: {report['unchanged']}.", '',
             '| Status | Container | Path | Parser candidates (not proof of successful parsing) |',
             '|---|---|---|---|']
    for status, entries in [('added', report['added']), ('removed', report['removed']),
                            ('changed', [e['after'] for e in report['changed']]),
                            ('spelling', [e['after'] for e in report['spelling_changes']])]:
        for entry in entries:
            values = [status, entry['archive'] or '(loose)', entry['path'],
                      ', '.join(entry['parser_candidates']) or 'unclassified']
            lines.append('| ' + ' | '.join(map(escape, values)) + ' |')
    return '\n'.join(lines) + '\n'


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=True) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    inv = commands.add_parser('inventory')
    inv.add_argument('root', type=Path)
    inv.add_argument('--label', required=True)
    inv.add_argument('--include', action='append', default=[])
    inv.add_argument('--archive-members', action='append', default=[])
    inv.add_argument('--hpitool', type=Path)
    inv.add_argument('--output', required=True, type=Path)
    diff = commands.add_parser('compare')
    diff.add_argument('left', type=Path); diff.add_argument('right', type=Path)
    diff.add_argument('--output', required=True, type=Path)
    diff.add_argument('--markdown', type=Path)
    args = parser.parse_args()
    try:
        if args.command == 'inventory':
            source, output = args.root.resolve(), args.output.resolve()
            if output == source or (source.is_dir() and output.is_relative_to(source)):
                raise ValueError('output must be outside the source tree')
            value = inventory(args.root, args.label, args.include, args.archive_members, args.hpitool)
        else:
            outputs = [args.output] + ([args.markdown] if args.markdown else [])
            resolved = [p.resolve() for p in outputs]
            if len(set(resolved)) != len(resolved) or any(p in [args.left.resolve(), args.right.resolve()] for p in resolved):
                raise ValueError('comparison outputs must not overwrite inputs or each other')
            value = compare(json.loads(args.left.read_text()), json.loads(args.right.read_text()))
            if args.markdown: args.markdown.write_text(markdown(value), encoding='utf-8')
        write_json(args.output, value)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'crusades inventory: {error}\n')


if __name__ == '__main__': main()
