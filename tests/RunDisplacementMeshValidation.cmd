@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++latest /EHsc /MDd /I src tests\DisplacementMeshGeneratorValidation.cpp src\Renderer\Geometry\DisplacementMeshGenerator.cpp /Fobuild\ /Febuild\DisplacementMeshGeneratorValidation.exe
if errorlevel 1 exit /b 1
build\DisplacementMeshGeneratorValidation.exe
