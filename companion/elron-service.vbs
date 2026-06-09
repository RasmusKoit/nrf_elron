' Launch the Elron companion service hidden (no console window).
' Put a copy in your Startup folder to run it automatically at logon:
'   copy elron-service.vbs "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\"
' Defaults to %USERPROFILE%\elron_companion; change the path below if yours differs.

Dim sh
Set sh = CreateObject("WScript.Shell")
sh.CurrentDirectory = sh.ExpandEnvironmentStrings("%USERPROFILE%") & "\elron_companion"
sh.Run "uv run elron_push.py --serve", 0, False
