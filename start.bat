@echo off
call "%~dp0.venv310\Scripts\activate.bat"
python "%~dp0template.py" %*
deactivate
