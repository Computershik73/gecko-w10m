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
   import kernel32.dll by name, the engine's DLLs among them.

   These forwarders are NOT shipped under the real names. The api-set schema
   on that phone names kernel32.dll as the host of the core api-sets and
   falls back to kernelbase only while no kernel32.dll can be found; the
   moment the package carried one, the loader bound every module's api-set
   imports to it and ended the process on the first function the forwarder
   did not have. So each legacy name gets an alias of the same length or
   shorter, the forwarder ships under the alias, and the import-name strings
   in every binary of the package are rewritten in place to the alias.

   And the forwarders do not point at api-set names the phone might lack.
   The 1607 SDK's umbrella libraries are the desktop's; the phone's schema
   is a subset, and a forward to a name outside it fails the whole load with
   "module not found" -- which is what mozglue's load came back with once
   kernl32.dll was mapped. A forward goes to an api-set name only if that
   name has been seen to resolve on the phone (PROVEN below); otherwise it
   goes to the DLL that hosts the family on Windows 10 Mobile, by name.
   Every forward of that second kind is written into probe-forwards.h, so
   the loader probe can ask each host for each function and name any that
   are missing.

Usage: gen-apiset-shims.py <stage dir> <work dir> <cl.exe> <lld-link.exe> <sdk lib dir>
"""
import glob
import os
import re
import struct
import subprocess
import sys

# legacy name -> (alias of no greater length, fallback DLL for unmapped functions)
LEGACY = {
    'kernel32.dll': ('kernl32.dll', 'kernel32legacy.dll'),
    'advapi32.dll': ('advap32.dll', 'kernelbase.dll'),
    'ole32.dll': ('ole3x.dll', 'combase.dll'),
    'version.dll': ('vers1on.dll', 'kernelbase.dll'),
    'dbghelp.dll': ('dbghlp.dll', None),
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

# Api-set names seen to resolve on a Lumia 650 running 10.0.14393: the
# imports of Gecko.exe and Probe.exe, both of which ran there, and the names
# the probe loaded by hand.
PROVEN = set('''
api-ms-win-core-handle-l1-1-0 api-ms-win-core-file-l1-2-1 api-ms-win-core-file-l2-1-1
api-ms-win-eventing-provider-l1-1-0 api-ms-win-core-errorhandling-l1-1-1
api-ms-win-core-libraryloader-l1-2-0 api-ms-win-core-libraryloader-l1-2-2
api-ms-win-core-libraryloader-l2-1-0 api-ms-win-core-synch-l1-2-0
api-ms-win-appmodel-runtime-l1-1-1 api-ms-win-core-processenvironment-l1-2-0
api-ms-win-core-profile-l1-1-0 api-ms-win-core-processthreads-l1-1-2
api-ms-win-core-sysinfo-l1-2-1 api-ms-win-core-sysinfo-l1-2-0
api-ms-win-core-interlocked-l1-2-0 api-ms-win-core-rtlsupport-l1-2-0
api-ms-win-core-string-l1-1-0 api-ms-win-core-localization-l1-2-1
api-ms-win-core-fibers-l1-1-1 api-ms-win-core-fibers-l1-1-0
api-ms-win-core-heap-l1-2-0 api-ms-win-core-console-l1-1-0
api-ms-win-core-memory-l1-1-2 api-ms-win-core-memory-l1-1-4
api-ms-win-core-com-l1-1-1 api-ms-win-core-com-l1-1-0 api-ms-win-core-debug-l1-1-1
api-ms-win-core-util-l1-1-0 api-ms-win-core-winrt-l1-1-0 api-ms-win-core-winrt-string-l1-1-0
api-ms-win-core-winrt-error-l1-1-1 api-ms-win-core-threadpool-l1-2-0
api-ms-win-core-file-l2-1-0 api-ms-win-crt-locale-l1-1-0 api-ms-win-crt-time-l1-1-0
'''.split())


def host_for(apiset):
    """The DLL that hosts an api-set family on Windows 10 Mobile."""
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
    if apiset.startswith(('api-ms-win-service-', 'api-ms-win-security-lsalookup-')):
        return 'sechost.dll'
    if apiset.startswith('api-ms-win-security-cryptoapi-'):
        return 'cryptsp.dll'
    return 'kernelbase.dll'


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


def rewrite_import_names(path, renames):
    """Rewrite import DLL name strings in place: same length, NUL-padded."""
    d = open(path, 'rb').read()
    out = d
    changed = 0
    for old, new in renames.items():
        assert len(new) <= len(old)
        pat = re.compile(re.escape(old.encode()) + b'\0', re.IGNORECASE)
        n = len(pat.findall(out))
        if n:
            out = pat.sub(new.encode() + b'\0' * (len(old) - len(new) + 1), out)
            changed += n
    if changed:
        assert len(out) == len(d)
        open(path, 'wb').write(out)
    return changed


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
    aliases = {k: v[0] for k, v in LEGACY.items()}
    alias_names = set(aliases.values())

    binaries = [f for f in glob.glob(os.path.join(stage, '*.dll')) + glob.glob(os.path.join(stage, '*.exe'))
                if not os.path.basename(f).lower().startswith(('api-ms-win-', 'ext-ms-win-'))
                and os.path.basename(f).lower() not in alias_names]

    wanted_apisets = {}
    wanted_legacy = {}
    for f in binaries:
        for dll, names in imports(f).items():
            if dll.startswith(('api-ms-win-', 'ext-ms-win-')):
                wanted_apisets.setdefault(dll[:-4], set()).update(names)
            elif dll in LEGACY:
                wanted_legacy.setdefault(dll, set()).update(names)
            elif dll in alias_names:
                legacy = [k for k, v in aliases.items() if v == dll][0]
                wanted_legacy.setdefault(legacy, set()).update(names)

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
        target = host_for(apiset)[:-4]
        deffile = os.path.join(work, apiset + '.def')
        with open(deffile, 'w') as d:
            d.write('LIBRARY ' + apiset + '\nEXPORTS\n')
            for n in names:
                d.write('  %s=%s.%s\n' % (n, target, n))
        if link(lld, [obj], deffile, os.path.join(stage, apiset + '.dll'), work, apiset):
            made += 1
    print('    %d api-set forwarder shims staged (%d names imported by the package)'
          % (made, len(wanted_apisets)))

    for legacy in LEGACY:
        p = os.path.join(stage, legacy)
        if os.path.exists(p):
            os.remove(p)

    # The legacy-name layer is for Windows 10 Mobile 1607 and is parked:
    # twenty of the functions the engine imports exist in no DLL on that
    # build, and everything past the loader is untested there. Off by
    # default, because the alias rewrite would route every OS build through
    # these forwarders, and 1709 -- the one build the port is proven on --
    # has the real DLLs. GECKO_W10M_LEGACY_SHIMS=1 turns it back on.
    if os.environ.get('GECKO_W10M_LEGACY_SHIMS') != '1':
        with open(os.path.join(work, 'probe-forwards.h'), 'w') as h:
            h.write('static const struct { const wchar_t* host; const char* name; } kForwards[] = {{0, 0}};\n')
        print('    legacy-name forwarders: off (GECKO_W10M_LEGACY_SHIMS=1 to build them)')
        return

    probe_pairs = []   # (host dll, function) for the probe to verify
    for dll in sorted(wanted_legacy):
        alias, fallback = LEGACY[dll]
        base = alias[:-4]
        names = sorted(n for n in wanted_legacy[dll] if n)
        forwards, stubs, dropped, by_host = [], [], [], 0
        for n in names:
            if n in STUBS:
                stubs.append(n)
            elif n in RENAMES:
                forwards.append('%s=%s' % (n, RENAMES[n]))
                probe_pairs.append(tuple(RENAMES[n].split('.')))
            elif n in umbrella:
                apiset = umbrella[n][:-4]
                if apiset in PROVEN:
                    forwards.append('%s=%s.%s' % (n, apiset, n))
                else:
                    host = host_for(apiset)
                    forwards.append('%s=%s.%s' % (n, host[:-4], n))
                    probe_pairs.append((host[:-4], n))
                    by_host += 1
            elif fallback:
                forwards.append('%s=%s.%s' % (n, fallback[:-4], n))
                probe_pairs.append((fallback[:-4], n))
                by_host += 1
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
        ok = link(lld, objs, deffile, os.path.join(stage, alias), work, base)
        print('    %s as %s: %s -- %d forwarded (%d by host name), %d stubbed%s'
              % (dll, alias, 'staged' if ok else 'FAILED', len(forwards), by_host, len(stubs),
                 (', %d DROPPED: %s' % (len(dropped), ' '.join(dropped))) if dropped else ''))

    with open(os.path.join(work, 'probe-forwards.h'), 'w') as h:
        h.write('/* generated by gen-apiset-shims.py: every legacy forward that names a host DLL */\n')
        h.write('static const struct { const wchar_t* host; const char* name; } kForwards[] = {\n')
        for host, n in sorted(set(probe_pairs)):
            h.write('  {L"%s.dll", "%s"},\n' % (host, n))
        h.write('  {0, 0}};\n')
    print('    %d host-name forwards listed for the probe' % len(set(probe_pairs)))

    patched = 0
    for f in binaries:
        if rewrite_import_names(f, aliases):
            patched += 1
    print('    import names rewritten to the aliases in %d binaries' % patched)


if __name__ == '__main__':
    main()
