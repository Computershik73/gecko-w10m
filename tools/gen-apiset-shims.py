"""Forwarder shims for names the phone's OS does not have.

Two kinds, one mechanism. A DLL the loader cannot find by name is searched
for in the package directory, so a package-local DLL of that name whose
exports forward to where the functions really live stands in for it.

1. Api-set names. For every api-set name any binary in the package imports,
   a DLL of that name forwarding the imported functions to kernelbase,
   ucrtbase, combase or rpcrt4. Where the schema knows the name the file is
   never opened; where it does not, the forwarder stands in.

2. Legacy names. Windows 10 Mobile 1607 has no kernel32.dll, advapi32.dll,
   ole32.dll, version.dll or dbghelp.dll -- the loader probe on a Lumia 650
   asked for each and was refused -- while nineteen files in the package
   import kernel32.dll by name, the engine's DLLs among them. For each such
   name a DLL forwarding every imported function to the api-set the 1607
   SDK's umbrella libraries map it to; what the umbrella does not map goes to
   kernel32legacy.dll, which that phone has, and the handful of functions
   that exist nowhere on the phone (the event log, LogonUser, dbghelp's
   symbol lookup) are stubs that fail cleanly. On a phone that has the real
   DLL, kernel32 is a KnownDLL and the system's copy wins; the others resolve
   through the same api-sets the system's copies would.

Usage: gen-apiset-shims.py <stage dir> <work dir> <cl.exe> <lld-link.exe> <sdk lib dir>
"""
import glob
import os
import struct
import subprocess
import sys

LEGACY = {
    # name the phone lacks: where an unmapped function is forwarded
    'kernel32.dll': 'kernel32legacy.dll',
    'advapi32.dll': 'kernelbase.dll',
    'ole32.dll': 'combase.dll',
    'version.dll': 'kernelbase.dll',
    'dbghelp.dll': None,
}

# Functions no DLL on the phone provides: a stub that fails, and the value
# it returns. All are on paths a phone never takes -- the event log, LogonUser,
# symbolising a stack for a crash report.
STUBS = {
    'RegisterEventSourceW': 0,   # HANDLE: NULL
    'DeregisterEventSource': 0,  # BOOL: FALSE
    'ReportEventW': 0,           # BOOL: FALSE
    'LogonUserW': 0,             # BOOL: FALSE
    'RegRenameKey': 120,         # ERROR_CALL_NOT_IMPLEMENTED
    'SymFromAddr': 0,            # BOOL: FALSE
}

# Functions that exist under another name.
RENAMES = {
    'LsaNtStatusToWinError': 'ntdll.RtlNtStatusToDosError',
}


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


def umbrella_map(libdir):
    """function name -> dll name, from the SDK's umbrella import libraries."""
    m = {}
    for lib in ('WindowsApp.lib', 'OneCoreUap.lib'):
        path = os.path.join(libdir, lib)
        if not os.path.exists(path):
            continue
        d = open(path, 'rb').read()
        off = 8
        while off + 60 <= len(d):
            size = int(d[off + 48:off + 58].decode().strip() or 0)
            body = d[off + 60:off + 60 + size]
            if body[:4] == b'\x00\x00\xff\xff':
                end = body.index(b'\0', 20)
                sym = body[20:end].decode(errors='replace')
                dll = body[end + 1:body.index(b'\0', end + 1)].decode(errors='replace')
                m.setdefault(sym, dll.lower())
            off += 60 + size + (size & 1)
    return m


def target_for_apiset(apiset):
    if apiset.startswith('api-ms-win-crt-'):
        return 'ucrtbase'
    if apiset.startswith(('api-ms-win-core-com-', 'api-ms-win-core-winrt')):
        return 'combase'
    if apiset.startswith('api-ms-win-core-rpc'):
        return 'rpcrt4'
    if apiset.startswith('api-ms-win-shcore-'):
        return 'shcore'
    if apiset.startswith('ext-ms-win-uiacore-'):
        return 'uiautomationcore'
    return 'kernelbase'


def link(lld, objs, deffile, out, work, name):
    r = subprocess.run([lld] + objs + ['/DLL', '/NOENTRY', '/APPCONTAINER',
                        '/MACHINE:ARM', '/NODEFAULTLIB', '/DEF:' + deffile,
                        '/IMPLIB:' + os.path.join(work, name + '.lib'),
                        '/OUT:' + out], capture_output=True, text=True)
    if r.returncode != 0:
        print('    shim %s FAILED: %s' % (name, r.stderr.strip()[:300]))
        return False
    return True


def main():
    stage, work, cl, lld, libdir = sys.argv[1:6]
    os.makedirs(work, exist_ok=True)
    umbrella = umbrella_map(libdir)

    wanted_apisets = {}
    wanted_legacy = {}
    for f in glob.glob(os.path.join(stage, '*.dll')) + glob.glob(os.path.join(stage, '*.exe')):
        base = os.path.basename(f).lower()
        if base.startswith(('api-ms-win-', 'ext-ms-win-')) or base in LEGACY:
            continue
        for dll, names in imports(f).items():
            if dll.startswith(('api-ms-win-', 'ext-ms-win-')):
                wanted_apisets.setdefault(dll[:-4], set()).update(names)
            elif dll in LEGACY:
                wanted_legacy.setdefault(dll, set()).update(names)

    src = os.path.join(work, 'empty.c')
    obj = os.path.join(work, 'empty.obj')
    open(src, 'w').write('int gecko_w10m_apiset_shim;\n')
    subprocess.run([cl, '/nologo', '/c', src, '/Fo:' + obj], check=True,
                   stdout=subprocess.DEVNULL)

    made = 0
    for apiset in sorted(wanted_apisets):
        names = sorted(n for n in wanted_apisets[apiset] if n)
        if not names:
            continue
        target = target_for_apiset(apiset)
        deffile = os.path.join(work, apiset + '.def')
        with open(deffile, 'w') as d:
            d.write('LIBRARY ' + apiset + '\nEXPORTS\n')
            for n in names:
                d.write('  %s=%s.%s\n' % (n, target, n))
        if link(lld, [obj], deffile, os.path.join(stage, apiset + '.dll'), work, apiset):
            made += 1
    print('    %d api-set forwarder shims staged (%d names imported by the package)'
          % (made, len(wanted_apisets)))

    for dll in sorted(wanted_legacy):
        base = dll[:-4]
        names = sorted(n for n in wanted_legacy[dll] if n)
        fallback = LEGACY[dll]
        forwards, stubs, dropped = [], [], []
        for n in names:
            if n in STUBS:
                stubs.append(n)
            elif n in RENAMES:
                forwards.append('%s=%s' % (n, RENAMES[n]))
            elif n in umbrella:
                forwards.append('%s=%s.%s' % (n, umbrella[n][:-4], n))
            elif fallback:
                forwards.append('%s=%s.%s' % (n, fallback[:-4], n))
            else:
                dropped.append(n)
        objs = [obj]
        if stubs:
            csrc = os.path.join(work, base + '-stubs.c')
            cobj = os.path.join(work, base + '-stubs.obj')
            with open(csrc, 'w') as c:
                for n in stubs:
                    c.write('__declspec(dllexport) int __stdcall %s(void) { return %d; }\n'
                            % (n, STUBS[n]))
            subprocess.run([cl, '/nologo', '/c', '/GS-', '/O1', csrc, '/Fo:' + cobj],
                           check=True, stdout=subprocess.DEVNULL)
            objs.append(cobj)
        deffile = os.path.join(work, base + '.def')
        with open(deffile, 'w') as d:
            d.write('LIBRARY ' + base + '\nEXPORTS\n')
            for line in forwards:
                d.write('  ' + line + '\n')
            for n in stubs:
                d.write('  ' + n + '\n')
        ok = link(lld, objs, deffile, os.path.join(stage, dll), work, base)
        print('    %s: %s -- %d forwarded, %d stubbed%s'
              % (dll, 'staged' if ok else 'FAILED', len(forwards), len(stubs),
                 (', %d DROPPED: %s' % (len(dropped), ' '.join(dropped))) if dropped else ''))


if __name__ == '__main__':
    main()
