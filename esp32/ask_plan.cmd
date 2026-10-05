@echo off
rem Asks the gadget's Muse for the plan it made with Mat, saves it to plan.md and opens it.
cd /d "%~dp0"
python tools\muse\chat.py --port COM8 --out plan.md "This is Claude, Mat's coding assistant, asking on Mat's behalf. Lay out the plan the two of you made today in full: the goal, what changes from what exists now, decisions already made, constraints, priorities and order of work, and anything you want built, changed or removed. Be complete and concrete."
echo.
echo Saved to plan.md
start notepad plan.md
pause
