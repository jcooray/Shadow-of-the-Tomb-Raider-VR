# Builds the installer: stages the release files into installer/stage and runs Inno Setup's compiler (ISCC).
# Needs the release build (cmake --build build-release --config Release --target vrmod_dxgi_proxy) and the Cheeky fork's
# release build (branch sottr-vr of github.com/jcooray/CheekyFoveatedDLSS, its bin/Release). Output:
# installer/output/Shadow-of-the-Tomb-Raider-VR-<ver>-Setup.exe
# Usage: python tools/make_installer.py  (the CHEEKY_BIN and ISCC environment variables override the paths below)
import os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHEEKY = os.environ.get('CHEEKY_BIN', r'D:\Projects\Cheeky-sottr-vr\bin\Release')
ISCC = os.environ.get('ISCC', r'D:\DevTools\InnoSetup\ISCC.exe')
inst = os.path.join(ROOT, 'installer')
stage = os.path.join(inst, 'stage')
version = re.search(r'SOTTRVR_VERSION_STRING "([^"]+)"', open(os.path.join(ROOT, 'src', 'version.h')).read()).group(1)

shutil.rmtree(stage, ignore_errors=True)
os.makedirs(os.path.join(stage, 'cheeky'))
def cp(src, dst):
    shutil.copy2(src, os.path.join(stage, dst))
    print('  ' + dst)
print('staging ' + version)
cp(os.path.join(ROOT, 'build-release', 'bin', 'Release', 'dxgi.dll'), 'dxgi.dll')
cp(os.path.join(inst, 'files', 'README.txt'), 'README.txt')
cp(os.path.join(inst, 'files', 'before_install.txt'), 'before_install.txt')
cp(os.path.join(ROOT, 'LICENSE'), 'LICENSE.txt')
cp(os.path.join(ROOT, 'THIRD_PARTY_NOTICES.md'), 'THIRD_PARTY_NOTICES.md')
cp(os.path.join(CHEEKY, 'CheekyFoveatedDLSS', 'CheekyFoveatedDLSSHost.dll'), r'cheeky\CheekyFoveatedDLSSHost.dll')
cp(os.path.join(CHEEKY, 'CheekyFoveatedDLSS', 'CheekyFoveatedDLSSRuntime.dll'), r'cheeky\CheekyFoveatedDLSSRuntime.dll')
cp(os.path.join(inst, 'files', 'CheekyFoveatedDLSS.ini'), r'cheeky\CheekyFoveatedDLSS.ini')
cp(os.path.join(inst, 'files', 'CheekyOverlay.ini'), r'cheeky\CheekyOverlay.ini')
with open(os.path.join(stage, 'cheeky', 'CHEEKY_LICENSE.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
    f.write(open(os.path.join(inst, 'files', 'CHEEKY_NOTICE.txt'), encoding='utf-8').read())
    f.write(open(os.path.join(os.path.dirname(os.path.dirname(CHEEKY)), 'LICENSE'), encoding='utf-8').read())
print(r'  cheeky\CHEEKY_LICENSE.txt')
r = subprocess.run([ISCC, '/Qp', '/DAppVersion=' + version, os.path.join(inst, 'SOTTR-VR.iss')])
sys.exit(r.returncode)
