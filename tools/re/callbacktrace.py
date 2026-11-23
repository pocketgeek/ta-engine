#!/usr/bin/env python3
"""Validate recorded native-call/callback nesting without inferring parents."""
import argparse
import json
from pathlib import Path


def native_call_tree(capture):
    if not capture.get('native_entry_sites'):
        raise ValueError('native entry records required; return order cannot establish callback parents')
    roots=[]; stack=[]; seen=set()
    tables={'entry':'native_entries','callback':'user_callbacks',
            'syscall':'syscall_calls','unix':'unix_calls'}
    for ordinal,ref in enumerate(capture['native_order']):
        kind,index=ref['kind'],ref['index']
        if kind not in tables or not isinstance(index,int) or index<0:
            raise ValueError(f'invalid native event reference at {ordinal}')
        key=(kind,index)
        if key in seen: raise ValueError(f'duplicate native event at {ordinal}')
        seen.add(key)
        events=capture.get(tables[kind],[])
        if index>=len(events): raise ValueError(f'missing native event at {ordinal}')
        event=events[index]
        if kind=='entry' and event.get('phase')=='restart':
            if not stack or stack[-1]['entry']!={'kind':'entry','index':event['restarts_entry']}:
                raise ValueError(f'dispatcher restart has no matching active call at {ordinal}')
            original=capture['native_entries'][event['restarts_entry']]
            fields=('kind','stack','return_address','number')
            fields+=('arguments','stub_return') if event['kind']=='syscall' else ('parameters','handle')
            if any(event[f]!=original[f] for f in fields):
                raise ValueError(f'dispatcher restart arguments differ at {ordinal}')
            stack[-1].setdefault('restarts',[]).append(ref)
            continue
        entering=kind=='entry' or (kind=='callback' and event['phase']=='entry')
        if entering:
            node={'kind':event['kind'] if kind=='entry' else 'callback',
                  'entry':ref,'entry_ordinal':ordinal,'children':[]}
            (stack[-1]['children'] if stack else roots).append(node)
            stack.append(node)
            continue
        if not stack: raise ValueError(f'native return has no captured entry at {ordinal}')
        node=stack[-1]
        if node['kind']!=kind:
            raise ValueError(f'native return nesting differs at {ordinal}: {kind} inside {node["kind"]}')
        entry_ref=node['entry']; entry=capture[tables[entry_ref['kind']]][entry_ref['index']]
        if 'entry_index' in event and event['entry_index']!=entry_ref['index']:
            raise ValueError(f'native return entry identity differs at {ordinal}')
        if kind=='syscall':
            fields=('stub_return','number','return_address','stack','arguments')
        elif kind=='unix':
            fields=('return_address','number','handle','parameters')
        else:
            if event['phase']!='return': raise ValueError('invalid callback phase')
            fields=()
        for field in fields:
            if entry[field]!=event[field]:
                raise ValueError(f'native {field} differs between entry and return at {ordinal}')
        if event['tick']<entry['tick']: raise ValueError('native call tick moved backwards')
        node['return']=ref; node['return_ordinal']=ordinal
        stack.pop()
    if stack: raise ValueError(f'{len(stack)} native calls remain open at capture end')
    for kind,table in tables.items():
        if sum(1 for k,_ in seen if k==kind)!=len(capture.get(table,[])):
            raise ValueError(f'{kind} records missing from native order')
    return roots


def summary(roots):
    counts={}; max_depth=0; callback_parents={}
    def walk(nodes,depth,parent):
        nonlocal max_depth
        for n in nodes:
            counts[n['kind']]=counts.get(n['kind'],0)+1
            max_depth=max(max_depth,depth)
            if n['kind']=='callback': callback_parents[parent]=callback_parents.get(parent,0)+1
            walk(n['children'],depth+1,n['kind'])
    walk(roots,1,'none')
    return {'roots':len(roots),'calls':counts,'max_depth':max_depth,'callback_parents':callback_parents}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    args=parser.parse_args()
    print(json.dumps(summary(native_call_tree(json.loads(args.capture.read_text()))),indent=2))


if __name__=='__main__': main()
