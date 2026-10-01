@echo off
echo [Sightline] 启动 WSL 服务器（8888 端口）……
wsl bash -c "pgrep -f 'build/sightline_server' >/dev/null && echo '[Sightline] 服务器已在运行' || (cd /mnt/e/workspace/Demo/Sightline/server && setsid nohup ./build/sightline_server > /tmp/sightline_server.log 2>&1 < /dev/null & sleep 1 && echo '[Sightline] 服务器已启动')"
wsl bash -c "ss -tln | grep -q 8888 && echo '[Sightline] 端口 8888 监听 OK'"
pause
