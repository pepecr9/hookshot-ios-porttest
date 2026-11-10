"""Builds an unsigned test IPA on Windows with LLVM (clang + ld64.lld) and an extracted iPhoneOS SDK, without a Mac.
Same steps as the CI job: compile the test and ios/main.m, link with UIKit/Foundation, Info.plist from ios/Info.plist, zip Payload/.
Usage: python win_build_ipa.py --sdk <iPhoneOS.sdk> [--llvm "C:/Program Files/LLVM/bin"] [--out <folder>] [test]
Tests: e4c (default). The IPA is unsigned: KSign (or another signer) signs it on install."""
import argparse, pathlib, re, subprocess, sys, zipfile

TESTS = {   # name: (source, entry, executable, bundle suffix, display name, report file, extra link args)
    'e4c': ('e4c.c', 'e4c_all', 'HSVMemTestC', 'hsvmemtestc', 'HS E4-C', 'e4c_vmem.txt', []),
}
HERE = pathlib.Path(__file__).resolve().parent

def run(cmd):
    print('>', ' '.join(map(str, cmd)))
    r = subprocess.run(list(map(str, cmd)), capture_output=True, text=True)
    if r.stdout.strip(): print(r.stdout.strip())
    if r.stderr.strip(): print(r.stderr.strip())
    if r.returncode: sys.exit(f'failed ({r.returncode})')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('test', nargs='?', default='e4c', choices=sorted(TESTS))
    ap.add_argument('--sdk', required=True)
    ap.add_argument('--llvm', default='C:/Program Files/LLVM/bin')
    ap.add_argument('--out', default=str(HERE / 'win_out'))
    a = ap.parse_args()
    src, entry, exe, bid, disp, report, extra = TESTS[a.test]
    sdk, llvm, out = pathlib.Path(a.sdk), pathlib.Path(a.llvm), pathlib.Path(a.out)
    clang = llvm / 'clang.exe' if (llvm / 'clang.exe').exists() else llvm / 'clang'
    if not (sdk / 'SDKSettings.json').exists(): sys.exit(f'no iPhoneOS SDK at {sdk}')
    obj, app = out / f'obj_{a.test}', out / 'Payload' / f'{exe}.app'
    obj.mkdir(parents=True, exist_ok=True); app.mkdir(parents=True, exist_ok=True)
    t = ['-target', 'arm64-apple-ios26.0', '-isysroot', sdk]
    run([clang, *t, '-O2', '-Wall', '-Werror', '-c', HERE / src, '-o', obj / 'test.o'])
    run([clang, *t, '-fobjc-arc', f'-DPT_ENTRY={entry}', f'-DPT_OUTFILE="{report}"', '-c', HERE / 'ios/main.m', '-o', obj / 'main.o'])
    run([clang, *t, '-fuse-ld=lld', '-fobjc-arc', obj / 'test.o', obj / 'main.o', '-framework', 'UIKit', '-framework', 'Foundation', *extra, '-o', app / exe])
    plist = (HERE / 'ios/Info.plist').read_text(encoding='utf-8')
    plist = plist.replace('es.pepecr9.hookshotporttest', f'es.pepecr9.{bid}').replace('<string>HSPortTest<', f'<string>{exe}<').replace('HS PortTest', disp)
    assert f'es.pepecr9.{bid}' in plist and f'<string>{exe}<' in plist, 'Info.plist substitution failed'
    (app / 'Info.plist').write_text(plist, encoding='utf-8', newline='\n')
    ipa = out / f'{exe.replace("HSVMemTestC", "HSVMemTest-E4C")}-unsigned.ipa'
    with zipfile.ZipFile(ipa, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted((out / 'Payload').rglob('*')):
            if f.is_file():
                zi = zipfile.ZipInfo(str(f.relative_to(out)).replace('\\', '/'))
                zi.compress_type = zipfile.ZIP_DEFLATED
                zi.external_attr = (0o100755 if f.name == exe else 0o100644) << 16   # the executable bit iOS needs
                z.writestr(zi, f.read_bytes())
    print(f'IPA: {ipa} ({ipa.stat().st_size} bytes)')

if __name__ == '__main__':
    main()
