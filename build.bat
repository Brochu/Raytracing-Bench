@echo off

set rflags=-nologo -v -Fo.\objects\resources.res
set res-files=.\src\resources.rc

set cflags=-nologo -std:c++17 -EHsc -Zi -MT -MP -WX -Fo.\objects\
set includes=-I .\includes\
set defines=-D"BUILD_WINDOWS=1" -D"BUILD_DEBUG=1"
set src-files=.\src\~unity.cpp

set lflags=-NOLOGO -DEBUG -LIBPATH:.\libs\
SET libs="kernel32.lib" "user32.lib" "gdi32.lib" "d3d12.lib" "dxguid.lib" "dxgi.lib" "d3dcompiler.lib"

rc %rflags% %res-files%
cl %cflags% %includes% %defines% %src-files% -link %lflags% .\objects\resources.res %libs% -SUBSYSTEM:WINDOWS -OUT:rtbench.exe
