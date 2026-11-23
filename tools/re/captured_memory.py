"""Bounded reader for memory regions in an initial retail capture."""
import base64
import zlib


def initial_memory_reader(frame):
    regions = frame.get('game_memory', [])
    decoded = {}
    def read(address, size):
        if not 0 <= address <= 0xffffffff or not 0 <= size <= 32*1024*1024 or address+size>0x100000000:
            raise ValueError('invalid captured memory range')
        out = bytearray()
        while len(out) < size:
            cursor = address+len(out)
            region = next((r for r in regions if r['address'] <= cursor < r['address']+r['size']), None)
            if region is None:
                raise ValueError(f'initial captured memory absent at {cursor:#x}')
            base, extent = region['address'], region['size']
            if not 0 < extent <= 32*1024*1024:
                raise ValueError('invalid captured memory extent')
            if base not in decoded:
                decoder = zlib.decompressobj()
                blob = decoder.decompress(base64.b64decode(region['zlib_base64'], validate=True), extent+1)
                if len(blob) != extent or not decoder.eof or decoder.unused_data:
                    raise ValueError('invalid compressed captured memory')
                decoded[base] = blob
            offset = cursor-base
            count = min(size-len(out), extent-offset)
            out.extend(decoded[base][offset:offset+count])
        return bytes(out)
    return read
