#!/usr/bin/env python3
"""kmod injector: checks a game APK and adds the kmod loader to it.

    python kmod_inject.py check  game.apk
    python kmod_inject.py inject game.apk --out game_kmod.apk --loader libkmod.so --core libkmodcore.so [--md7 libkmodmd7.so]
                                 [--ks my.keystore] [--ks-pass android] [--dry-run] [--json]

What it does (nothing here is specific to one game version):
  * reads the binary AndroidManifest.xml: package, the Application class, the launcher Activity, SDK levels
  * decides whether the APK can be injected: arm64 Unreal Engine game, a launcher class to patch, not injected already,
    no known anti-tamper (Google PairIP, licensing library) that would refuse a re-signed copy
  * patches <clinit> of the launcher class (or the Application class) so it calls System.loadLibrary("kmod") first
  * adds the loader libraries, drops the old signature, aligns and signs the result

Needs Python 3.8+, a JDK (java, keytool) with the dexlib2 jars (see --dextool-cp) and the Android build-tools (zipalign, apksigner).
"""
import argparse, hashlib, json, os, re, shutil, struct, subprocess, sys, tempfile, zipfile

# ---------------------------------------------------------------------------------------------------------------- AXML
RES_XML_START_ELEMENT = 0x0102
RES_XML_STRING_POOL = 0x0001
TYPE_STRING, TYPE_INT_DEC, TYPE_INT_HEX, TYPE_INT_BOOLEAN, TYPE_REFERENCE = 0x03, 0x10, 0x11, 0x12, 0x01


def _read_string_pool(d, off):
    typ, hdr, size, count, style_count, flags, strings_start, styles_start = struct.unpack_from('<HHIIIIII', d, off)
    utf8 = bool(flags & 0x100)
    offsets = struct.unpack_from('<%dI' % count, d, off + hdr)
    base = off + strings_start
    out = []
    for o in offsets:
        p = base + o
        if utf8:
            n = d[p]; p += 1
            if n & 0x80: n = ((n & 0x7F) << 8) | d[p]; p += 1
            b = d[p]; p += 1
            if b & 0x80: b = ((b & 0x7F) << 8) | d[p]; p += 1
            out.append(d[p:p + b].decode('utf-8', 'replace'))
        else:
            n = struct.unpack_from('<H', d, p)[0]; p += 2
            if n & 0x8000: n = ((n & 0x7FFF) << 16) | struct.unpack_from('<H', d, p)[0]; p += 2
            out.append(d[p:p + n * 2].decode('utf-16-le', 'replace'))
    return out, size


def parse_manifest(data):
    """Returns a dict: package, application{name,extractNativeLibs}, activities[{name,launcher,alias_target}], minSdk, targetSdk."""
    if struct.unpack_from('<H', data, 0)[0] != 0x0003:
        raise ValueError('AndroidManifest.xml is not binary XML')
    pos = struct.unpack_from('<H', data, 2)[0]
    strings = []
    stack = []
    info = {'package': '', 'application': {}, 'activities': [], 'minSdk': None, 'targetSdk': None, 'permissions': []}
    cur_activity = None
    cur_filter = None
    while pos + 8 <= len(data):
        typ, hdr, size = struct.unpack_from('<HHI', data, pos)
        if typ == RES_XML_STRING_POOL:
            strings, _ = _read_string_pool(data, pos)
        elif typ == RES_XML_START_ELEMENT:
            ns, name_i, attr_start, attr_size, attr_count = struct.unpack_from('<iiHHH', data, pos + 16)
            tag = strings[name_i]
            attrs = {}
            for i in range(attr_count):
                a = pos + 16 + attr_start + i * attr_size
                ans, an, raw, vsize, res0, vtype, vdata = struct.unpack_from('<iiiHBBI', data, a)
                key = strings[an]
                if raw != -1: val = strings[raw]
                elif vtype == TYPE_STRING: val = strings[vdata]
                elif vtype == TYPE_INT_BOOLEAN: val = bool(vdata)
                else: val = vdata
                attrs[key] = val
            stack.append(tag)
            if tag == 'manifest': info['package'] = attrs.get('package', '')
            elif tag == 'uses-sdk':
                info['minSdk'] = attrs.get('minSdkVersion'); info['targetSdk'] = attrs.get('targetSdkVersion')
            elif tag == 'uses-permission': info['permissions'].append(attrs.get('name', ''))
            elif tag == 'application': info['application'] = attrs
            elif tag in ('activity', 'activity-alias'):
                cur_activity = {'name': attrs.get('name', ''), 'alias_target': attrs.get('targetActivity'), 'launcher': False, 'tag': tag}
                info['activities'].append(cur_activity)
            elif tag == 'intent-filter': cur_filter = {'main': False, 'launcher': False}
            elif tag == 'action' and cur_filter is not None and attrs.get('name') == 'android.intent.action.MAIN': cur_filter['main'] = True
            elif tag == 'category' and cur_filter is not None and attrs.get('name') == 'android.intent.category.LAUNCHER': cur_filter['launcher'] = True
        elif typ == 0x0103:   # end element
            ns, name_i = struct.unpack_from('<ii', data, pos + 16)
            tag = strings[name_i]
            if stack: stack.pop()
            if tag == 'intent-filter':
                if cur_filter and cur_filter['main'] and cur_filter['launcher'] and cur_activity: cur_activity['launcher'] = True
                cur_filter = None
            elif tag in ('activity', 'activity-alias'): cur_activity = None
        pos += size if size else 8
    return info


def full_class(pkg, name):
    if not name: return ''
    if name.startswith('.'): return pkg + name
    if '.' not in name: return pkg + '.' + name
    return name


def launcher_class(info):
    """The class to patch: the launcher Activity (an alias resolves to its target)."""
    pkg = info['package']
    by_name = {full_class(pkg, a['name']): a for a in info['activities']}
    for a in info['activities']:
        if a['launcher']:
            tgt = a['alias_target'] if a['tag'] == 'activity-alias' and a['alias_target'] else a['name']
            return full_class(pkg, tgt)
    return ''


# ---------------------------------------------------------------------------------------------------------------- checks
ENGINE_LIBS = ('libUnreal.so', 'libUE4.so', 'libUE5.so')
TAMPER_LIBS = {'libpairipcore.so': 'Google PairIP (the app checks its own signature and integrity)'}
TAMPER_CLASSES = [(b'Lcom/google/android/vending/licensing/', 'Google Play licensing library (LVL)'),
                  (b'Lcom/pairip/', 'Google PairIP')]


def dex_names(z):
    return sorted([n for n in z.namelist() if re.fullmatch(r'classes\d*\.dex', n)], key=lambda n: int(re.sub(r'\D', '', n) or 1))


def find_class_dex(z, desc):
    """The dex that defines class descriptor `desc` ('Lcom/x/Y;'): the descriptor is a string of that dex's string table."""
    needle = desc.encode()
    for n in dex_names(z):
        if needle in z.read(n): return n
    return None


def analyze(path):
    r = {'apk': os.path.basename(path), 'ok': True, 'errors': [], 'warnings': [], 'facts': {}}
    try:
        z = zipfile.ZipFile(path)
    except Exception as e:
        r['ok'] = False; r['errors'].append('not a valid APK/zip: %s' % e); return r, None
    names = z.namelist()
    if 'AndroidManifest.xml' not in names or not dex_names(z):
        r['ok'] = False; r['errors'].append('no AndroidManifest.xml / classes.dex: this is not a full APK (a split? an XAPK/APKS bundle must be unpacked first)')
        return r, z
    try:
        man = parse_manifest(z.read('AndroidManifest.xml'))
    except Exception as e:
        r['ok'] = False; r['errors'].append('cannot read AndroidManifest.xml: %s' % e); return r, z
    f = r['facts']
    f['package'] = man['package']; f['minSdk'] = man['minSdk']; f['targetSdk'] = man['targetSdk']
    f['extractNativeLibs'] = man['application'].get('extractNativeLibs')
    app_cls = full_class(man['package'], man['application'].get('name', ''))
    launch = launcher_class(man)
    f['launcher'] = launch; f['application'] = app_cls

    # architecture and engine
    arm64 = [n for n in names if n.startswith('lib/arm64-v8a/')]
    f['arm64_libs'] = len(arm64)
    engine = [n for n in arm64 if os.path.basename(n) in ENGINE_LIBS]
    if not arm64: r['ok'] = False; r['errors'].append('no lib/arm64-v8a/: only 64-bit ARM games are supported')
    if not engine:
        r['ok'] = False; r['errors'].append('no Unreal Engine library (libUnreal.so / libUE4.so) in lib/arm64-v8a/: not an Unreal game, or its libraries are in a split APK')
    else:
        blob = z.read(engine[0])
        m = re.search(rb'\+\+UE[45]\+Release-(\d+\.\d+)', blob)
        ver = m.group(1).decode() if m else None
        if not ver:        # the engine keeps its branch name as a UTF-16 string
            m = re.search(rb'\+\x00\+\x00U\x00E\x00[45]\x00\+\x00R\x00e\x00l\x00e\x00a\x00s\x00e\x00-\x00((?:\d\x00){1,2}\.\x00(?:\d\x00){1,2})', blob)
            ver = m.group(1).decode('utf-16-le') if m else None
        f['engine_lib'] = os.path.basename(engine[0])
        f['engine_version'] = ver
        if not ver: r['warnings'].append('Unreal Engine version not found in %s' % f['engine_lib'])
        f['engine_lib_sha1'] = hashlib.sha1(blob).hexdigest()[:12]

    # what to patch
    target = launch or app_cls
    f['patch_class'] = target
    if not target:
        r['ok'] = False; r['errors'].append('no launcher activity and no Application class in the manifest')
    else:
        desc = 'L' + target.replace('.', '/') + ';'
        d = find_class_dex(z, desc)
        f['patch_dex'] = d
        if not d: r['ok'] = False; r['errors'].append('class %s is not in any classes*.dex' % target)

    # already injected?
    if 'lib/arm64-v8a/libkmod.so' in names or any(b'Lkmod/' in z.read(n) for n in dex_names(z)[:1]):
        r['warnings'].append('this APK already contains kmod (injecting again would replace its files)')
        f['already_injected'] = True
    # anti-tamper
    tamper = []
    for n in arm64:
        if os.path.basename(n) in TAMPER_LIBS: tamper.append(TAMPER_LIBS[os.path.basename(n)])
    for n in dex_names(z):
        blob = z.read(n)
        for needle, what in TAMPER_CLASSES:
            if needle in blob and what not in tamper: tamper.append(what)
    f['anti_tamper'] = tamper
    for t in tamper: r['warnings'].append('anti-tamper: %s. A re-signed copy may refuse to run.' % t)
    if man['targetSdk'] is not None and isinstance(man['targetSdk'], int) and man['targetSdk'] < 24:
        r['warnings'].append('targetSdkVersion %s is old: Android 14+ refuses to install apps below 23' % man['targetSdk'])
    return r, z


# ---------------------------------------------------------------------------------------------------------------- tools
def find_tool(names, extra_dirs):
    for d in extra_dirs:
        for n in names:
            p = os.path.join(d, n)
            if os.path.isfile(p): return p
    for n in names:
        p = shutil.which(n)
        if p: return p
    return None


def sdk_build_tools():
    roots = [os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT'), os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk'), os.path.expanduser('~/Android/Sdk')]
    for r in roots:
        if r and os.path.isdir(os.path.join(r, 'build-tools')):
            vs = sorted(os.listdir(os.path.join(r, 'build-tools')), key=lambda v: [int(x) if x.isdigit() else 0 for x in re.split(r'[.\-]', v)], reverse=True)
            if vs: return os.path.join(r, 'build-tools', vs[0])
    return None


def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if p.returncode != 0:
        raise RuntimeError('%s failed:\n%s%s' % (os.path.basename(cmd[0]), p.stdout, p.stderr))
    return p.stdout


def patch_dex(java, cp, dex_in, dex_out, cls_desc, lib):
    run([java, '-cp', cp, 'DexTool', 'patch', dex_in, dex_out, cls_desc, lib])


def default_dextool_cp(here):
    out = os.path.join(here, 'out')
    studio = os.environ.get('ANDROID_STUDIO', 'D:\\AndroidStudio')
    jars = [os.path.join(studio, 'plugins', 'android', 'lib', j) for j in ('smali-dexlib2-3.0.9.jar', 'smali-util-3.0.9.jar', 'jsr305-2.0.1.jar')]
    guava = os.path.join(studio, 'lib', '*')
    return os.pathsep.join([out] + jars + [guava])


# ---------------------------------------------------------------------------------------------------------------- inject
def inject(args):
    rep, z = analyze(args.apk)
    f = rep['facts']
    if not rep['ok']: return rep
    if args.dry_run: return rep
    here = os.path.dirname(os.path.abspath(__file__))
    java = args.java or find_tool(['java.exe', 'java'], [os.path.join(os.environ.get('JAVA_HOME', ''), 'bin'), 'D:\\AndroidStudio\\jbr\\bin'])
    if not java: raise RuntimeError('java not found (set JAVA_HOME or pass --java)')
    cp = args.dextool_cp or default_dextool_cp(here)
    bt = sdk_build_tools()
    zipalign = find_tool(['zipalign.exe', 'zipalign'], [bt] if bt else [])
    apksigner = find_tool(['apksigner.bat', 'apksigner'], [bt] if bt else [])
    if not (zipalign and apksigner): raise RuntimeError('Android build-tools (zipalign, apksigner) not found')

    work = tempfile.mkdtemp(prefix='kmod_inject_')
    try:
        desc = 'L' + f['patch_class'].replace('.', '/') + ';'
        dex_name = f['patch_dex']
        src_dex = os.path.join(work, 'in.dex'); out_dex = os.path.join(work, 'out.dex')
        open(src_dex, 'wb').write(z.read(dex_name))
        patch_dex(java, cp, src_dex, out_dex, desc, 'kmod')
        libs = {'libkmod.so': args.loader, 'libkmodcore.so': args.core}
        if args.md7: libs['libkmodmd7.so'] = args.md7
        unaligned = os.path.join(work, 'unaligned.apk'); aligned = os.path.join(work, 'aligned.apk')
        sig = re.compile(r'^META-INF/[^/]+\.(SF|RSA|DSA|EC|MF)$')
        stored_libs = f.get('extractNativeLibs') is False
        with zipfile.ZipFile(unaligned, 'w', allowZip64=True) as zo:
            for info in z.infolist():
                if sig.match(info.filename):
                    continue
                if info.filename in ['lib/arm64-v8a/' + k for k in libs]: continue      # replaced below
                ni = zipfile.ZipInfo(info.filename, info.date_time)
                ni.compress_type = info.compress_type; ni.external_attr = info.external_attr
                if info.filename == dex_name: zo.writestr(ni, open(out_dex, 'rb').read())
                elif info.is_dir(): zo.writestr(ni, b'')
                else:
                    with z.open(info) as s, zo.open(ni, 'w', force_zip64=True) as d: shutil.copyfileobj(s, d, 1 << 20)
            for name, path in libs.items():
                ni = zipfile.ZipInfo('lib/arm64-v8a/' + name, (2026, 1, 1, 0, 0, 0))
                ni.compress_type = zipfile.ZIP_STORED if stored_libs else zipfile.ZIP_DEFLATED
                zo.writestr(ni, open(path, 'rb').read())
        run([zipalign, '-p', '-f', '4', unaligned, aligned])
        ks = args.ks
        if not ks:
            ks = os.path.join(os.path.dirname(os.path.abspath(args.out)), 'kmod-injected.keystore')
            if not os.path.exists(ks):
                keytool = find_tool(['keytool.exe', 'keytool'], [os.path.dirname(java)])
                run([keytool, '-genkeypair', '-keystore', ks, '-storepass', args.ks_pass, '-keypass', args.ks_pass, '-alias', 'kmod',
                     '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000', '-dname', 'CN=kmod injected build'])
                rep['warnings'].append('created a signing key: %s (keep it: later builds signed with another key cannot be installed over this one)' % ks)
        run([apksigner, 'sign', '--ks', ks, '--ks-pass', 'pass:' + args.ks_pass, '--key-pass', 'pass:' + args.ks_pass, '--ks-key-alias', args.ks_alias,
             '--out', args.out, aligned])
        rep['output'] = args.out
        rep['patched'] = {'class': f['patch_class'], 'dex': dex_name, 'loadLibrary': 'kmod'}
        rep['libs'] = sorted(libs)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return rep


def print_report(rep, as_json):
    if as_json:
        print(json.dumps(rep, indent=2, ensure_ascii=False)); return
    f = rep['facts']
    print('APK: %s' % rep['apk'])
    for k in ('package', 'engine_lib', 'engine_version', 'minSdk', 'targetSdk', 'launcher', 'application', 'patch_class', 'patch_dex', 'extractNativeLibs'):
        if k in f and f[k] not in (None, ''): print('  %-18s %s' % (k, f[k]))
    for w in rep['warnings']: print('  warning: ' + w)
    for e in rep['errors']: print('  ERROR: ' + e)
    print('  => ' + ('can be injected' if rep['ok'] else 'CANNOT be injected'))
    if rep.get('output'): print('  wrote %s (patched %s in %s)' % (rep['output'], rep['patched']['class'], rep['patched']['dex']))


def main():
    ap = argparse.ArgumentParser(description='kmod injector')
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('check'); c.add_argument('apk'); c.add_argument('--json', action='store_true')
    i = sub.add_parser('inject'); i.add_argument('apk'); i.add_argument('--out', required=True)
    i.add_argument('--loader', required=True); i.add_argument('--core', required=True); i.add_argument('--md7')
    i.add_argument('--ks'); i.add_argument('--ks-pass', default='android'); i.add_argument('--ks-alias', default='kmod')
    i.add_argument('--java'); i.add_argument('--dextool-cp'); i.add_argument('--dry-run', action='store_true'); i.add_argument('--json', action='store_true')
    a = ap.parse_args()
    if a.cmd == 'check':
        rep, _ = analyze(a.apk)
    else:
        for p in (a.loader, a.core) + ((a.md7,) if a.md7 else ()):
            if not os.path.isfile(p): sys.exit('file not found: ' + p)
        rep = inject(a)
    print_report(rep, a.json)
    sys.exit(0 if rep['ok'] else 1)


if __name__ == '__main__':
    main()
