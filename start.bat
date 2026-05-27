@echo off
set PROJECT=C:\local\source\MatricePadTempo
call "%PROJECT%\.venv310\Scripts\activate.bat"
python "%PROJECT%\template.py" %*
deactivate
