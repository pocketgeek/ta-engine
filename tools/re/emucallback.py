"""Execute captured Wine dispatch callbacks; never substitute their game effects.

Currently limited to NtUserDispatchMessage with a captured stack and ID 4
callbacks whose native children are leaf syscalls. Child APIs still need their
own explicit replay contracts. Unsupported nesting or inputs stop replay.
"""
import struct
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EIP,UC_X86_REG_EAX
from callbacktrace import native_call_tree


class CapturedDispatchCallbacks:
    def __init__(self,process,capture,dispatch_entry):
        self.p=process; self.capture=capture; self.index=0; self.active=None
        self.callback_count=0; self.child_count=0
        self.dispatch_entry=dispatch_entry
        roots=native_call_tree(capture)
        self.nodes=[n for n in roots if n['kind']=='syscall' and
                    self.entry(n)['stub_return']==dispatch_entry+12]
        for address in {e['stub_return']-12 for e in capture['native_entries']
                        if e['kind']=='syscall'}-{dispatch_entry}:
            process.uc.hook_add(UC_HOOK_CODE,self.child_call,begin=address,end=address)
        process.uc.hook_add(UC_HOOK_CODE,self.dispatch,begin=dispatch_entry,end=dispatch_entry)
        address=capture['callback_return']
        process.uc.hook_add(UC_HOOK_CODE,self.callback_return,begin=address,end=address)

    def entry(self,node):
        return self.capture['native_entries'][node['entry']['index']]

    def callback_event(self,ref):
        return self.capture['user_callbacks'][ref['index']]

    def dispatch(self,uc,address,size,data):
        if self.active is not None: raise RuntimeError('nested dispatch callback replay unsupported')
        if self.index>=len(self.nodes): raise RuntimeError('recorded dispatch calls exhausted')
        node=self.nodes[self.index]; entry=self.entry(node); p=self.p
        sp=uc.reg_read(UC_X86_REG_ESP)
        if (sp!=entry['stack'] or p.u32(sp)!=entry['return_address']
                or entry['tick']!=p.u32(p.game+0x19f44)):
            raise RuntimeError('dispatch caller, stack or tick differs')
        if len(entry['arguments'])!=1: raise RuntimeError('unsupported dispatch signature')
        actual=p.u32(sp+4)
        expected=bytes.fromhex(entry['pointer_samples']['0'])[:28]
        if len(expected)!=28 or bytes(uc.mem_read(actual,28))!=expected:
            raise RuntimeError('dispatch MSG input differs')
        if any(child['kind']!='callback' for child in node['children']):
            raise RuntimeError('dispatch has unsupported native work outside callbacks')
        self.active={'node':node,'context':uc.context_save(),'sp':sp,
                     'return_address':p.u32(sp),'callback':0,'child':0}
        self.start_callback()

    def start_callback(self):
        state=self.active; node=state['node']; uc=self.p.uc
        if state['callback']==len(node['children']):
            result=self.capture['syscall_calls'][node['return']['index']]['result']
            uc.context_restore(state['context'])
            uc.reg_write(UC_X86_REG_EAX,result)
            uc.reg_write(UC_X86_REG_ESP,state['sp']+8)
            uc.reg_write(UC_X86_REG_EIP,state['return_address'])
            self.active=None; self.index+=1
            return
        callback=node['children'][state['callback']]
        event=self.callback_event(callback['entry'])
        if event['number']!=4 or any(c['kind']!='syscall' or c['children'] for c in callback['children']):
            raise RuntimeError('unsupported callback kind or nested native callback')
        payload=bytes.fromhex(event['payload_hex'])
        if (len(payload)!=event['size'] or not 0<event['size']<=65536
                or not state['sp']-65536<=event['stack']
                or not event['stack']+16<=event['address']
                or event['address']+len(payload)>state['sp']):
            raise RuntimeError('callback payload does not fit captured stack')
        uc.context_restore(state['context'])
        uc.mem_write(event['address'],payload)
        uc.mem_write(event['stack'],struct.pack('<4I',0,event['number'],event['address'],event['size']))
        uc.reg_write(UC_X86_REG_ESP,event['stack'])
        uc.reg_write(UC_X86_REG_EIP,self.capture['callback_entry'])
        state['child']=0

    def child_call(self,uc,address,size,data):
        if self.active is None: return
        state=self.active
        callback=state['node']['children'][state['callback']]
        if state['child']>=len(callback['children']):
            raise RuntimeError('unexpected native call during callback')
        child=callback['children'][state['child']]; entry=self.entry(child)
        sp=uc.reg_read(UC_X86_REG_ESP)
        args=list(struct.unpack('<'+str(len(entry['arguments']))+'I',uc.mem_read(sp+4,len(entry['arguments'])*4)))
        if (address+12!=entry['stub_return'] or sp!=entry['stack']
                or self.p.u32(sp)!=entry['return_address'] or args!=entry['arguments']
                or self.p.u32(self.p.game+0x19f44)!=entry['tick']):
            raise RuntimeError(f'callback child {state["child"]} native input/order differs')
        state['child']+=1; self.child_count+=1

    def callback_return(self,uc,address,size,data):
        if self.active is None: raise RuntimeError('callback returned without captured parent')
        state=self.active; callback=state['node']['children'][state['callback']]
        if state['child']!=len(callback['children']):
            raise RuntimeError('callback returned before all captured native children')
        expected=self.callback_event(callback['return'])
        sp=uc.reg_read(UC_X86_REG_ESP)
        pointer,length,status=struct.unpack('<3I',uc.mem_read(sp+4,12))
        if length!=expected['size'] or status!=expected['status']:
            raise RuntimeError('callback return status or size differs')
        if bytes(uc.mem_read(pointer,length))!=bytes.fromhex(expected['payload_hex']):
            raise RuntimeError('callback return payload differs')
        state['callback']+=1; self.callback_count+=1
        self.start_callback()
