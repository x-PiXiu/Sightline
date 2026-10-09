@echo off
rem Sightline 第 2 期：启动服务器（WSL；日志直接显示在本窗口，Ctrl+C 关服）
wsl bash -c "cd /mnt/e/workspace/Demo/Sightline/server && ./sightline_server"
pause
