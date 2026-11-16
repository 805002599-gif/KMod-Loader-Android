import sys, zipfile, shutil, re
args = sys.argv[1:]
classes2 = None
if '--classes2' in args:
    i = args.index('--classes2'); classes2 = args[i + 1]; del args[i:i + 2]
src, dst, newdex = args[0:3]
newlibs = args[3:]
zin = zipfile.ZipFile(src)
zout = zipfile.ZipFile(dst, 'w', allowZip64=True)
sig = re.compile(r'^META-INF/[^/]+\.(SF|RSA|DSA|EC|MF)$')
n = 0
for info in zin.infolist():
    if sig.match(info.filename):
        print('drop', info.filename); continue
    ni = zipfile.ZipInfo(info.filename, info.date_time)
    ni.compress_type = info.compress_type
    ni.external_attr = info.external_attr
    if info.filename == 'classes.dex':
        with open(newdex, 'rb') as f: zout.writestr(ni, f.read())
        print('replaced classes.dex')
    elif info.is_dir():
        zout.writestr(ni, b'')
    else:
        with zin.open(info) as s, zout.open(ni, 'w', force_zip64=True) as d:
            shutil.copyfileobj(s, d, 1 << 20)
    n += 1
import os
for newlib in newlibs:
    ni = zipfile.ZipInfo('lib/arm64-v8a/' + os.path.basename(newlib), (2026, 11, 15, 0, 0, 0))
    ni.compress_type = zipfile.ZIP_DEFLATED
    with open(newlib, 'rb') as f: zout.writestr(ni, f.read())
if classes2:
    ni = zipfile.ZipInfo('classes2.dex', (2026, 11, 16, 0, 0, 0))
    ni.compress_type = zipfile.ZIP_DEFLATED
    with open(classes2, 'rb') as f: zout.writestr(ni, f.read())
    print('added classes2.dex')
zout.close()
print('entries copied', n)
