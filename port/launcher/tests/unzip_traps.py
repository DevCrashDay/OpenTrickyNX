"""unzip_traps.py <ot_unzip_test.exe> <empty work folder>

Builds broken or hostile zips (a way out of the folder, an absolute or drive
path, a cut stream, a size past the end of the zip, a cut file, a wrong CRC-32,
a 4 GB announced size, a small announced size that inflates to 50 MB, a stored
entry whose sizes differ) and one valid control zip, unpacks each with the
launcher's ot_unzip (ot_unzip_test, see unzip_test.cpp) into its own
<work>/<case>/box/out, and checks that every trap is refused, that the control
unpacks to the same files, and that nothing is ever written outside out/.
Exit 0 when all cases pass.
"""
import hashlib
import os
import struct
import subprocess
import sys
import zlib

TOP = 'OpenTricky-9.9.9-win64/'
GOOD = {'name': TOP + 'README.txt', 'data': b'hello\r\n' * 100}
BIG = bytes(range(256)) * 4 * 50       # 50 KB, compressible


def deflate(b):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(b) + c.flush()


def build(entries):
    """Each entry: name, data, and optionally method (8), comp, crc, csize, usize."""
    out, cd = bytearray(), bytearray()
    for e in entries:
        name = e['name'].encode('utf-8')
        data = e.get('data', b'')
        method = e.get('method', 8)
        comp = e.get('comp', deflate(data) if method == 8 else data)
        crc = e.get('crc', zlib.crc32(data) & 0xffffffff)
        csize, usize = e.get('csize', len(comp)), e.get('usize', len(data))
        off = len(out)
        out += struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, 0, method, 0, 0, crc, csize, usize, len(name), 0) + name + comp
        cd += struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 20, 20, 0, method, 0, 0, crc, csize, usize,
                          len(name), 0, 0, 0, 0, 0, off) + name
    cdoff = len(out)
    out += cd
    out += struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, len(entries), len(entries), len(cd), cdoff, 0)
    return bytes(out)


def cases():
    bigc = deflate(BIG)
    bs = chr(92)
    bad_crc = (zlib.crc32(BIG) ^ 1) & 0xffffffff
    cut = build([GOOD, {'name': TOP + 'big.bin', 'data': BIG}])
    control = [GOOD, {'name': TOP + 'sub/', 'method': 0}, {'name': TOP + 'sub/big.bin', 'data': BIG},
               {'name': TOP + 'empty.txt', 'data': b''}]
    return [
        ('dotdot', build([GOOD, {'name': TOP + '../../escape-dotdot.txt', 'data': b'x'}])),
        ('dotdot-backslash', build([GOOD, {'name': TOP + '..' + bs + '..' + bs + 'escape-bs.txt', 'data': b'x'}])),
        ('absolute', build([{'name': 'README.txt', 'data': b'a'}, {'name': '/escape-abs.txt', 'data': b'x'}])),
        ('drive', build([{'name': 'README.txt', 'data': b'a'}, {'name': 'C:/escape-drive.txt', 'data': b'x'}])),
        ('cut-stream', build([GOOD, {'name': TOP + 'big.bin', 'data': BIG, 'comp': bigc[:len(bigc) // 2]}])),
        ('size-past-end', build([GOOD, {'name': TOP + 'big.bin', 'data': BIG, 'csize': len(bigc) + 10_000_000}])),
        ('file-cut', cut[:len(cut) // 2]),
        ('crc', build([GOOD, {'name': TOP + 'big.bin', 'data': BIG, 'crc': bad_crc}])),
        ('crc-stored', build([GOOD, {'name': TOP + 'big.bin', 'data': BIG, 'method': 0, 'crc': bad_crc}])),
        ('huge-size', build([GOOD, {'name': TOP + 'big.bin', 'data': BIG, 'usize': 0xFFFFFFF0}])),
        ('bomb', build([GOOD, {'name': TOP + 'big.bin', 'data': b'\0' * 50_000_000, 'usize': 1000}])),
        ('stored-sizes', build([GOOD, {'name': TOP + 'big.bin', 'data': BIG, 'method': 0, 'usize': len(BIG) * 2}])),
        ('control', build(control)),
    ]


def tree(root):
    got = {}
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            got[os.path.relpath(p, root).replace(os.sep, '/')] = hashlib.sha256(open(p, 'rb').read()).hexdigest()
    return got


def main():
    exe, work = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    want_control = {'README.txt': hashlib.sha256(GOOD['data']).hexdigest(),
                    'sub/big.bin': hashlib.sha256(BIG).hexdigest(),
                    'empty.txt': hashlib.sha256(b'').hexdigest()}
    fails = 0
    for name, data in cases():
        case = os.path.join(work, name)
        out = os.path.join(case, 'box', 'out')
        os.makedirs(os.path.join(case, 'box'))
        zp = os.path.join(case, name + '.zip')
        with open(zp, 'wb') as f:
            f.write(data)
        try:
            p = subprocess.run([exe, zp, out], capture_output=True, text=True, timeout=120)
            rc, msg = p.returncode, (p.stdout + p.stderr).strip().replace('\n', ' | ')
        except subprocess.TimeoutExpired:
            rc, msg = 'timeout', ''
        outside = [os.path.relpath(os.path.join(d, f), case) for d, _, files in os.walk(case)
                   if not os.path.abspath(d).startswith(out) for f in files if f != name + '.zip']
        for drive in 'CD':
            outside += [drive + ':/' + n for n in ('escape-abs.txt', 'escape-drive.txt') if os.path.exists(drive + ':/' + n)]
        if name == 'control':
            ok = rc == 0 and tree(out) == want_control and not outside
        else:
            ok = rc == 1 and msg.startswith('REFUSED:') and not outside
        fails += not ok
        print('%-4s %-17s rc=%-7s outside=%s  %s' % ('PASS' if ok else 'FAIL', name, rc, outside or '-', msg[:150]))
    print('%d case(s) failed' % fails if fails else 'ALL PASS')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
