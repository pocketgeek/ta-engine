"""Survey retail campaign data: campaign .tdf keys and the win/lose keys used by every
mission OTA they reference (data only; no binary content). Usage: camp_survey.py [tadata dir]"""
import re, glob, collections, os, sys
root = sys.argv[1] if len(sys.argv) > 1 else '/home/pocket_geek/TA/assets/tadata'
KEYS = ['KillEnemyCommander', 'DestroyAllUnits', 'KillAllMobileUnits', 'BuildUnitType', 'CaptureUnitType',
        'KillAllOfType', 'KillUnitType', 'MoveUnitToRadius', 'UnitTypePassesX', 'UnitTypePassesZ',
        'VictoryTimerRunsOut', 'CommanderKilled', 'AllUnitsKilled', 'AllUnitsKilledOfType', 'UnitTypeKilled',
        'DeathTimerRunsOut', 'AnyUnitPassesX', 'AnyUnitPassesZ', 'numplayers', 'useonlyunits', 'maxunits', 'nomovie']
msn = []
for c in sorted(glob.glob(os.path.join(root, 'camps', '*.tdf'))):
    t = open(c, 'rb').read().decode('latin1')
    files = [m.strip().lower() for m in re.findall(r'missionfile\s*=\s*([^;]+);', t, re.I)]
    keys = sorted(set(k.lower() for k in re.findall(r'^\s*([A-Za-z]+)\s*=', t, re.M)))
    print(os.path.basename(c), len(files), keys)
    msn += files
players = collections.Counter(); conds = collections.defaultdict(collections.Counter)
for m in sorted(set(msn)):
    p = os.path.join(root, 'maps', m)
    if not os.path.exists(p): print('MISSING', m); continue
    t = open(p, 'rb').read().decode('latin1')
    gh = t.split('[Schema')[0]
    for k in KEYS:
        for v in re.findall(r'^\s*' + k + r'\s*=\s*([^;]*);', gh, re.I | re.M): conds[k][v.strip()] += 1
    for v in re.findall(r'^\s*Player\s*=\s*(\d+)', t, re.I | re.M): players[v] += 1
print('Player values', dict(players))
for k, v in conds.items(): print(k, sum(v.values()), dict(v.most_common(8)))
