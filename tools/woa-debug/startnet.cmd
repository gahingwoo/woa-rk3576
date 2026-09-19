@echo off
rem ---------------------------------------------------------------------------
rem startnet.cmd - baked into boot.wim by .github/workflows/winpe.yml.
rem
rem WinPE runs this instead of a logon. wpeinit brings up PnP and networking;
rem after that we look for the collector on the media we booted from and run it
rem there, so its output lands on the USB stick and can be read from Linux.
rem
rem X: is the RAM disk and does not survive a reboot, which is why the output
rem must go to a real volume. The stick is found by looking for the collector
rem itself rather than by drive letter: WinPE assigns letters in an order that
rem depends on what enumerated, and on this board that order has changed
rem between boots.
rem ---------------------------------------------------------------------------
wpeinit

for %%d in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
    if exist "%%d:\woa-debug\collect.cmd" (
        echo Running %%d:\woa-debug\collect.cmd
        call "%%d:\woa-debug\collect.cmd" %%d:
        echo.
        echo Output is in %%d:\woa-debug\out\
        goto :done
    )
)
echo No woa-debug\collect.cmd found on any volume - skipping collection.

:done
