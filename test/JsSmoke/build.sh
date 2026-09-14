#!/bin/bash
# Build the JsSmoke UWP app (ARM) that runs JS through the ported mozjs-155.
#
# App.cpp (C++/WinRT) is compiled with MSVC cl.exe, which supports SEH-based
# C++ exceptions on ARM32 (clang cannot). smoke.cpp (the JSAPI embedding) is
# compiled exception-free with clang-cl, matching how mozjs itself was built.
set -e
VS="C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC/14.44.35207"
SDK="C:/Program Files (x86)/Windows Kits/10"; SDKV="10.0.22621.0"
DIST="C:/Users/User/Documents/GitHub/gecko-w10m/engine/obj-arm-uwp/dist"
LLVM="/c/Program Files/LLVM/bin"
CL="$VS/bin/Hostx64/arm/cl.exe"
export INCLUDE="$VS/include;$SDK/Include/$SDKV/ucrt;$SDK/Include/$SDKV/shared;$SDK/Include/$SDKV/um;$SDK/Include/$SDKV/winrt;$SDK/Include/$SDKV/cppwinrt"
export LIB="$VS/lib/arm/store;$VS/lib/arm;$SDK/Lib/$SDKV/ucrt/arm;$SDK/Lib/$SDKV/um/arm"
export PATH="$VS/bin/Hostx64/arm:$VS/bin/Hostx64/x64:$PATH"
export MSYS2_ARG_CONV_EXCL="*"
cd "$(dirname "$0")"

echo "=== compile smoke.cpp (clang-cl, exception-free, JSAPI) ==="
"$LLVM/clang-cl.exe" -c smoke.cpp -o smoke.o \
  --target=thumbv7-windows-msvc -Xclang -target-feature -Xclang +hwdiv \
  -std:c++20 -GR- -D_HAS_EXCEPTIONS=0 \
  -DWINAPI_FAMILY=WINAPI_FAMILY_APP -DGECKO_W10M=1 -D_ARM_ -DWIN32 -D_WIN32 \
  -DXP_WIN=1 -DNOMINMAX -DUNICODE -D_UNICODE \
  -FIgecko_w10m_arm_intrin.h -I"$DIST/include"

echo "=== compile App.cpp (MSVC cl.exe ARM, C++/WinRT + SEH) ==="
"$CL" /nologo /c App.cpp /Fo:App.obj /std:c++20 /EHsc /GR- \
  /DWINAPI_FAMILY=WINAPI_FAMILY_APP /D_ARM_ /DWIN32 /D_WIN32 /DNOMINMAX \
  /DUNICODE /D_UNICODE

echo "=== link JsSmoke.exe ==="
"$LLVM/lld-link.exe" App.obj smoke.o /OUT:JsSmoke.exe /APPCONTAINER \
  /SUBSYSTEM:WINDOWS,10.0 /ENTRY:wWinMainCRTStartup /MACHINE:ARM \
  "$DIST/lib/mozjs-155.lib" "$DIST/lib/mozglue.lib" WindowsApp.lib
echo "LINK OK: $(ls -la JsSmoke.exe | awk '{print $5}') bytes"
