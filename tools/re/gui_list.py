"""Compact one-line-per-gadget listing of .GUI files."""
import sys,os; sys.path.insert(0,'/home/pocket_geek/TA/tools/re'); from gui_survey import gadgets
T={'0':'CONT','1':'BTN','2':'LIST','3':'TEXT','4':'SCRL','5':'LBL','6':'SURF','7':'FONT','8':'PIC','9':'T9','10':'T10','12':'T12'}
for p in sys.argv[1:]:
    print('##',os.path.basename(p))
    for k,g in gadgets(p):
        c=g.get('COMMON',{}); t=c.get('id')
        ex={kk:v for kk,v in g.items() if not isinstance(v,dict) and v not in ('','0')}
        print(' %-4s %-16s %3s,%3s %3sx%-3s a=%s ca=%s as=%s fg=%s bg=%s tx=%s fn=%s act=%s %s %s'%(T.get(t,t),c.get('name'),c.get('xpos'),c.get('ypos'),c.get('width'),c.get('height'),c.get('attribs'),c.get('commonattribs'),c.get('assoc'),c.get('colorf'),c.get('colorb'),c.get('texturenumber'),c.get('fontnumber'),c.get('active'),ex, ('help='+c['help'][:50]) if c.get('help') else ''))
