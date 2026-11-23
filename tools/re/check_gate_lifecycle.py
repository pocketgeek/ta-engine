#!/usr/bin/env python3
"""Compare shipped gate animation, VM and yard occupancy against retail per tick.

Activation commands are controlled inputs. This is the live gate component of
the composed traversal fixture; this test does not advance a moving requester.
"""
import argparse
from pathlib import Path
import subprocess

from native_gate import NativeGate


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path('assets/game'))
    parser.add_argument('--binary',default='build-dbg/retail_script_test')
    parser.add_argument('--gate',default='arangate')
    parser.add_argument('--balance',type=int,choices=(0,1),default=0)
    args=parser.parse_args()
    native=NativeGate(args.root,args.gate)
    commands=[{20:0,40:1,41:1,1000:0,1001:0,1400:1,1600:0,1650:1,2200:0}.get(n,-1)
              for n in range(2600)]
    expected=[]
    for command in commands:
        native.step(command)
        expected.append(native.state())
    output=subprocess.run([args.binary,'--gate-timeline',str(args.root),args.gate,str(args.balance)],
                          input='\n'.join(map(str,commands))+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in output.stdout.splitlines()]
    assert len(actual)==len(expected),(len(actual),len(expected))
    for tick,(want,got) in enumerate(zip(expected,actual)):
        if want!=got:
            field=next((i for i,(a,b) in enumerate(zip(want,got)) if a!=b),min(len(want),len(got)))
            raise AssertionError((args.gate,args.balance,tick,field,'native',want[field:field+5],'World',got[field:field+5]))
    assert any(row[1] for row in expected),'fixture never opened'
    assert not expected[-1][1],'fixture never closed'
    print(f'PASS: {len(expected)} {args.gate} gate VM/animation/yard boundaries, balance={args.balance}')


if __name__=='__main__':
    main()
