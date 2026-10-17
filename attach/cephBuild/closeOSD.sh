#!/bin/bash

# 获取 ceph-osd 的进程号
pids=($(ps -aux | grep ceph-osd | grep -v grep | awk '{print $2}'))

# 检查是否有进程
if [ ${#pids[@]} -eq 0 ]; then
  echo "No ceph-osd processes found."
else
  # 循环杀掉每个进程
  for pid in "${pids[@]}"; do
    echo "Killing process with PID: $pid"
    kill -9 $pid
  done
  echo "All ceph-osd processes have been killed."
fi