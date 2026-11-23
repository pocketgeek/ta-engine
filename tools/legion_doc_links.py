#!/usr/bin/env python3
"""legion_doc_links -- keep docs/legion-pathfinding.md tied to what the instruments really measure.

Checks, all against the working tree:

  1. Every top-level bullet of the "Known weaknesses" section cites at least one `scenario:key` (a fixture in
     tools/scenarios/ and a key of its baseline entry) or `ctest:NAME` (a registered test; NAME may match a
     foreach-generated add_test pattern). A weakness nothing measures is not a known weakness, it is a guess.
  2. Every `scenario:key` / `ctest:NAME` token anywhere in the document resolves: the .scn file exists, the key is
     in that scenario's entry of tools/scenarios/baseline.json (either mode), the ctest exists. Renaming a
     scenario, a key or a test without updating the doc fails here.
  3. The doc's field-work quota ("quota of 384k relaxations per tick") equals kFieldQuota in src/sim/legion.cpp.
  4. No "as of <hash>" line names a commit that git does not know (skipped outside a git checkout).

  legion_doc_links.py [--root DIR]      the checks above (exit 1 on any failure, 2 on a tooling problem)
  legion_doc_links.py --selftest        copies the tree's inputs, renames one cited scenario, one cited key and one
                                        cited ctest in the copy, and fails unless each rename is caught
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

DOC = 'docs/legion-pathfinding.md'
BASELINE = 'tools/scenarios/baseline.json'
SCENARIOS = 'tools/scenarios'
CMAKE = 'CMakeLists.txt'
LEGION_CPP = 'src/sim/legion.cpp'

REF = re.compile(r'`([A-Za-z0-9][A-Za-z0-9_-]*):([A-Za-z0-9_.@-]+)`')
# Backticked tokens that look like scenario:key but are not (file:line, urls, option:value spellings).
NOT_SCENARIOS = {'src', 'docs', 'tools', 'http', 'https', 'git', 'kNetVersion', 'ctest'}


def read(root, rel):
    with open(os.path.join(root, rel), encoding='utf-8') as f:
        return f.read()


def section(text, heading):
    m = re.search(r'(?m)^## ' + re.escape(heading) + r'\s*$', text)
    if not m:
        return None
    rest = text[m.end():]
    n = re.search(r'(?m)^## ', rest)
    return rest[:n.start()] if n else rest


def top_bullets(body):
    """Top-level '* ' bullets of a section, each with its continuation lines and sub-bullets."""
    out, cur = [], None
    for line in body.splitlines():
        if line.startswith('* '):
            if cur is not None:
                out.append(cur)
            cur = line
        elif cur is not None:
            cur += '\n' + line
    if cur is not None:
        out.append(cur)
    return out


def ctest_patterns(root):
    """Regexes for every add_test(NAME ...) of CMakeLists.txt. A ${var} that a foreach() lists becomes the
    alternation of its values (every foreach of that name, unioned); an unknown ${var} becomes '.+'."""
    text = read(root, CMAKE)
    values = {}
    for m in re.finditer(r'foreach\(\s*(\w+)\s+([^)]*)\)', text):
        values.setdefault(m.group(1), set()).update(v for v in m.group(2).split() if not v.startswith('${'))
    pats = []
    for m in re.finditer(r'add_test\(\s*NAME\s+([^\s)]+)', text):
        name = m.group(1)
        if name.startswith('${'):   # a generic ${name} says nothing about which test it is
            continue
        def one(var):
            vals = values.get(var.group(1))
            return '(?:' + '|'.join(re.escape(v) for v in sorted(vals)) + ')' if vals else '.+'
        pats.append(re.compile('^' + re.sub(r'\\\$\\\{(\w+)\\\}', one, re.escape(name)) + '$'))
    return pats


def check(root):
    problems = []
    doc = read(root, DOC)
    baseline = json.load(open(os.path.join(root, BASELINE), encoding='utf-8'))['entries']
    scn_names = {f[:-4] for f in os.listdir(os.path.join(root, SCENARIOS)) if f.endswith('.scn')}
    pats = ctest_patterns(root)

    def resolve(token_scn, key):
        if token_scn == 'ctest':
            return any(p.match(key) for p in pats) or f"no ctest '{key}'"
        if token_scn not in scn_names:
            return f"no scenario '{token_scn}' in {SCENARIOS}/"
        entry = baseline.get(token_scn)
        if entry is None:
            return f"scenario '{token_scn}' has no entry in {BASELINE}"
        if not any(key in keys for keys in entry.values()):
            return f"scenario '{token_scn}' has no key '{key}' in {BASELINE}"
        return True

    cited = 0
    for m in REF.finditer(doc):
        scn, key = m.group(1), m.group(2)
        if scn in NOT_SCENARIOS and scn != 'ctest':
            continue
        if scn != 'ctest' and scn not in scn_names and not re.fullmatch(r'[a-z0-9-]+', scn):
            continue
        # A lowercase-dash token is only a reference when it names a scenario or says ctest; anything else that
        # merely looks like one (`foo:bar`) is a typo in a citation, so it is reported too.
        cited += 1
        r = resolve(scn, key)
        if r is not True:
            problems.append(f'`{scn}:{key}`: {r}')

    body = section(doc, 'Known weaknesses')
    if body is None:
        problems.append('no "## Known weaknesses" section')
    else:
        for b in top_bullets(body):
            refs = [(m.group(1), m.group(2)) for m in REF.finditer(b)
                    if m.group(1) == 'ctest' or re.fullmatch(r'[a-z0-9-]+', m.group(1))]
            if not refs:
                problems.append('Known weakness without a `scenario:key` or `ctest:NAME`: ' +
                                b.splitlines()[0][:90])

    m = re.search(r'quota\s+of\s+([0-9]+)(k|M)?\s+relaxations\s+per\s+tick', doc)
    cpp = read(root, LEGION_CPP)
    q = re.search(r"kFieldQuota\s*=\s*([0-9']+)", cpp)
    if not m:
        problems.append('the doc has no "quota of N relaxations per tick" line')
    elif not q:
        problems.append('kFieldQuota not found in ' + LEGION_CPP)
    else:
        doc_q = int(m.group(1)) * {None: 1, 'k': 1000, 'M': 1000000}[m.group(2)]
        if doc_q != int(q.group(1).replace("'", '')):
            problems.append(f'doc quota {doc_q} != kFieldQuota {q.group(1)}')

    shallow = subprocess.run(['git', '-C', root, 'rev-parse', '--is-shallow-repository'],
                             capture_output=True, text=True).stdout.strip() == 'true'
    # A shallow clone (CI checks out one commit) cannot resolve older "as of" commits; skip the check there.
    if not shallow and (os.path.isdir(os.path.join(root, '.git')) or os.path.isfile(os.path.join(root, '.git'))):
        for h in re.findall(r'(?i)\bas of ([0-9a-f]{7,40})\b', doc):
            # cat-file -t, not 'h^{commit}': MSYS git behind a mingw python drops the braces
            r = subprocess.run(['git', '-C', root, 'cat-file', '-t', h], capture_output=True, text=True)
            if r.returncode != 0 or r.stdout.strip() != 'commit':
                problems.append(f'"as of {h}": not a commit of this repository')
    return problems, cited


def selftest(root):
    """Rename a cited scenario, a cited key and a cited ctest in a copy; each must be caught."""
    doc = read(root, DOC)
    refs = [(m.group(1), m.group(2)) for m in REF.finditer(doc)]
    scn_ref = next((r for r in refs if r[0] != 'ctest' and re.fullmatch(r'[a-z0-9-]+', r[0])), None)
    cmake = read(root, CMAKE)
    # a ctest cited by a literal add_test name (the foreach-generated ones have no single line to rename)
    ctest_ref = next((r for r in refs if r[0] == 'ctest' and re.search(r'NAME ' + re.escape(r[1]) + r'(\s|\))', cmake)), None)
    if not scn_ref or not ctest_ref:
        print('selftest: the doc cites no scenario or no ctest', file=sys.stderr)
        return 2
    fails = 0
    cases = [('scenario renamed', lambda d: os.rename(os.path.join(d, SCENARIOS, scn_ref[0] + '.scn'),
                                                       os.path.join(d, SCENARIOS, scn_ref[0] + '-renamed.scn'))),
             ('key renamed', lambda d: write(d, BASELINE, read(d, BASELINE).replace('"' + scn_ref[1] + '"',
                                                                                 '"' + scn_ref[1] + '_renamed"'))),
             ('ctest renamed', lambda d: write(d, CMAKE, read(d, CMAKE).replace(ctest_ref[1], ctest_ref[1] + '_renamed'))),
             ('weakness uncited', lambda d: write(d, DOC, strip_first_weakness(read(d, DOC)))),
             ('quota changed', lambda d: write(d, LEGION_CPP, re.sub(r"(kFieldQuota\s*=\s*)[0-9']+", r'\g<1>12345', read(d, LEGION_CPP))))]
    for label, mutate in cases:
        with tempfile.TemporaryDirectory() as d:
            for rel in (DOC, BASELINE, CMAKE, LEGION_CPP):
                os.makedirs(os.path.join(d, os.path.dirname(rel)), exist_ok=True)
                shutil.copy(os.path.join(root, rel), os.path.join(d, rel))
            shutil.copytree(os.path.join(root, SCENARIOS), os.path.join(d, SCENARIOS), dirs_exist_ok=True)
            mutate(d)
            problems, _ = check(d)
            if problems:
                print(f'selftest ok: {label} -> {len(problems)} problem(s), e.g. {problems[0]}')
            else:
                print(f'selftest FAIL: {label} was not caught', file=sys.stderr)
                fails += 1
    return 1 if fails else 0


def strip_first_weakness(doc):
    """The doc with every citation removed from the first Known-weakness bullet."""
    m = re.search(r'(?m)^## Known weaknesses\s*$', doc)
    start = m.end()
    end = doc.index('\n* ', doc.index('\n* ', start) + 1)   # the second top-level bullet
    first = REF.sub('the check', doc[start:end])
    return doc[:start] + first + doc[end:]


def write(root, rel, text):
    with open(os.path.join(root, rel), 'w', encoding='utf-8') as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    try:
        if a.selftest:
            return selftest(root)
        problems, cited = check(root)
    except (OSError, ValueError, KeyError) as e:
        print('legion_doc_links: ' + str(e), file=sys.stderr)
        return 2
    for p in problems:
        print('FAIL ' + p)
    print(f'legion_doc_links: {cited} citation(s) resolved, {len(problems)} problem(s)')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
