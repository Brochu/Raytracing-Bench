@echo off

set cflags=-nologo -std:c++17 -EHsc -Zi -MT -MP -WX -Fo.\objects\
set includes=-I .\includes\
set defines=-D"BUILD_WINDOWS=1"
set src-files=.\src\main.cpp
set lflags=-NOLOGO -DEBUG -LIBPATH:.\libs\
SET libs="kernel32.lib" "user32.lib" "gdi32.lib" "d3d12.lib" "dxguid.lib" "dxgi.lib" "d3dcompiler.lib"

cl %cflags% %includes% %defines% %src-files% -link %lflags% %libs% -SUBSYSTEM:WINDOWS -OUT:rtbench.exe
