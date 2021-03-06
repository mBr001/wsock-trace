::
:: Generates a list of GUIDs for the Windows Filtering Platform.
:: I.e. parses this from the preprocessed outout of <fwpmu.h>:
::
:: DEFINE_GUID(
::   FWPM_LAYER_.* = {
::   0xc86fd1bf,
::   0x21cd,
::   0x497e,
::   0xa0, 0xbb, 0x17, 0x42, 0x5c, 0x88, 0x5c, 0x58
:: );
::
@echo off
setlocal
set CFLAGS=-nologo -E -D_WIN32_WINNT=0xA000
set GREP_OPT=--before-context=1 --after-context=5

echo `#include <fwpmu.h>`          > %TEMP%\gen-fwpm-guid.c
cl %CFLAGS% %TEMP%\gen-fwpm-guid.c > %TEMP%\gen-fwpm-guid.tmp

grep %GREP_OPT% '   FWPM_LAYER_.*,' %TEMP%\gen-fwpm-guid.tmp | ^
     sed -e 's/--//' -e 's/DEFINE_GUID(/_DEFINE_GUID (/'
