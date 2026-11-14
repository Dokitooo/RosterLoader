#!/bin/sh
# RosterLoader: bin/SLAM/RosterLoader.asi (SLAM mod) + bin/plain/RosterLoader.asi (plain .asi) - clang-cl + lld-link (LLVM 18)
set -e
export PATH="/usr/lib/llvm-18/bin:$PATH"
cd "$(dirname "$0")/.."
: "${MGW:?}"; : "${GEN:?}"
VER=${MOD_VERSION:-"1.2"}
O=build/obj; mkdir -p $O bin/SLAM bin/plain
for d in build/linux/*.def; do llvm-dlltool -m i386 -k -d $d -l $O/$(basename $d .def).lib; done
CF="$EXTRA --target=i686-pc-windows-msvc /c /O2 /GS- /Gs999999 /Zl /W3 -fms-extensions -Islam"
WH="-imsvc $GEN -imsvc $MGW/crt -imsvc $MGW/include -imsvc $MGW/defaults/include -D_X86_ -fgnuc-version=4.2.1 /Oi-"
D="'-DMOD_NAME=\"RosterLoader\"' \"-DMOD_VERSION=\\\"$VER\\\"\" /DMOD_HAS_STOP"
eval clang-cl $CF $WH $D /Fo$O/rl.obj src/rosterloader.c
eval clang-cl $CF $WH $D /Fo$O/slam_glue.obj slam/slam_glue.c
eval clang-cl $CF $WH $D /Fo$O/slam_lite.obj slam/slam_lite.c
L="lld-link /brepro /dll /nodefaultlib /entry:DllMain@12 /subsystem:windows,5.01 /machine:x86 /safeseh:no /opt:ref /dynamicbase /nxcompat /noimplib"
$L /out:bin/SLAM/RosterLoader.asi  $O/rl.obj $O/slam_glue.obj $O/kernel32.lib $O/user32.lib
$L /out:bin/plain/RosterLoader.asi $O/rl.obj $O/slam_lite.obj $O/kernel32.lib $O/user32.lib
ls -l bin/*/RosterLoader.asi
