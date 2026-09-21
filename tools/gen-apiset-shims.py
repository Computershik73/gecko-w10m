"""api-set forwarder shims, for phones whose OS lacks an api-set name.

A Lumia 650 on Windows 10 Mobile 1607 ends the app in the loader: the
ETW trace shows vcruntime140_app.dll never mapped, and the one import it has
that nothing else in the package resolved on that phone is
api-ms-win-core-fibers-l1-1-0. Api-set names are resolved through the OS's
schema first; a name the schema does not know is searched for as an ordinary
DLL, in the package directory -- which is how Microsoft's own app-local UCRT
ships api-ms-win-crt-*.dll files beside an executable for older Windows.

So: for every api-set name any binary in the package imports, a DLL of that
name whose exports forward to the DLL that really implements them. Where the
schema knows the name, the file is never opened. Where it does not, the
forwarder stands in. Only the functions actually imported are forwarded;
ordinal imports cannot be, and are skipped.

Usage: gen-apiset-shims.py <stage dir> <work dir> <cl.exe> <lld-link.exe>
"""
import glob
import os
import struct
import subprocess
import sys


def imports(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        return {}
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', d, opt)[0]
    dd = opt + (96 if magic == 0x10b else 112)
    irva = struct.unpack_from('<I', d, dd + 8)[0]
    if not irva:
        return {}
    so = opt + struct.unpack_from('<H', d, pe + 20)[0]
    secs = []
    for i in range(nsec):
        s = so + 40 * i
        secs.append((struct.unpack_from('<I', d, s + 12)[0],
                     struct.unpack_from('<I', d, s + 8)[0],
                     struct.unpack_from('<I', d, s + 20)[0]))

    def r2o(r):
        for va, vs, raw in secs:
            if va <= r < va + vs:
                return raw + r - va
        return None

    def cs(r):
        o = r2o(r)
        return d[o:d.index(b'\0', o)].decode(errors='replace')

    p = r2o(irva)
    out = {}
    while True:
        oft, _, _, name, ft = struct.unpack_from('<IIIII', d, p)
        if not name:
            break
        t = r2o(oft or ft)
        names = []
        while True:
            e = struct.unpack_from('<I', d, t)[0]
            if not e:
                break
            if not e & 0x80000000:
                names.append(cs(e + 2))
            t += 4
        out[cs(name).lower()] = names
        p += 20
    return out


def target_for(apiset):
    # Where the functions really live on Windows 10 Mobile. Everything the
    # kernel and its api-sets cover is in kernelbase; the CRT in ucrtbase; COM
    # and WinRT in combase; RPC in rpcrt4. All four were mapped into the
    # process on the 650 before it died.
    if apiset.startswith('api-ms-win-crt-'):
        return 'ucrtbase.dll'
    if apiset.startswith(('api-ms-win-core-com-', 'api-ms-win-core-winrt')):
        return 'combase.dll'
    if apiset.startswith('api-ms-win-core-rpc'):
        return 'rpcrt4.dll'
    if apiset.startswith('api-ms-win-shcore-'):
        return 'shcore.dll'
    if apiset.startswith('ext-ms-win-uiacore-'):
        return 'uiautomationcore.dll'
    return 'kernelbase.dll'


def main():
    stage, work, cl, lld = sys.argv[1:5]
    os.makedirs(work, exist_ok=True)
    wanted = {}
    for f in glob.glob(os.path.join(stage, '*.dll')) + glob.glob(os.path.join(stage, '*.exe')):
        base = os.path.basename(f).lower()
        if base.startswith(('api-ms-win-', 'ext-ms-win-')):
            continue
        for dll, names in imports(f).items():
            if dll.startswith(('api-ms-win-', 'ext-ms-win-')):
                wanted.setdefault(dll[:-4], set()).update(names)

    # one empty object, shared by every shim
    src = os.path.join(work, 'empty.c')
    obj = os.path.join(work, 'empty.obj')
    open(src, 'w').write('int gecko_w10m_apiset_shim;\n')
    subprocess.run([cl, '/nologo', '/c', src, '/Fo:' + obj], check=True,
                   stdout=subprocess.DEVNULL)

    made = 0
    for apiset in sorted(wanted):
        names = sorted(n for n in wanted[apiset] if n)
        if not names:
            continue
        target = target_for(apiset)[:-4]
        deffile = os.path.join(work, apiset + '.def')
        with open(deffile, 'w') as d:
            d.write('LIBRARY ' + apiset + '\nEXPORTS\n')
            for n in names:
                d.write('  %s=%s.%s\n' % (n, target, n))
        out = os.path.join(stage, apiset + '.dll')
        r = subprocess.run([lld, obj, '/DLL', '/NOENTRY', '/APPCONTAINER',
                            '/MACHINE:ARM', '/NODEFAULTLIB', '/DEF:' + deffile,
                            '/IMPLIB:' + os.path.join(work, apiset + '.lib'),
                            '/OUT:' + out], capture_output=True, text=True)
        if r.returncode != 0:
            print('    shim %s FAILED: %s' % (apiset, r.stderr.strip()[:200]))
            continue
        made += 1
    print('    %d api-set forwarder shims staged (%d names imported by the package)'
          % (made, len(wanted)))


if __name__ == '__main__':
    main()
