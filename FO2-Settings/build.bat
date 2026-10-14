@echo off
rem Builds "FlatOut 2 Mod Settings.exe" (.NET Framework 4, no extra runtime needed).
cd /d "%~dp0"
set FW=C:\Windows\Microsoft.NET\Framework\v4.0.30319
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /target:winexe /optimize+ /out:"FlatOut 2 Mod Settings.exe" /r:"%FW%\System.dll" /r:"%FW%\System.Core.dll" /r:"%FW%\System.Drawing.dll" /r:"%FW%\System.Windows.Forms.dll" SettingsApp.cs
