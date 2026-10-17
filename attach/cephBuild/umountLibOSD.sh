# 循环卸载 /var/lib/ceph/osd/ceph-0 到 ceph-15
for osd_id in {0..15}; do
    mount_point="/var/lib/ceph/osd/ceph-${osd_id}"
    
    # 检查挂载点是否存在
    if mount | grep -q "$mount_point"; then
        echo "正在卸载: $mount_point"
        sudo umount "$mount_point"
        
        if [ $? -eq 0 ]; then
            echo "成功卸载 $mount_point"
        else
            echo "卸载 $mount_point 失败！"
        fi
    else
        echo "$mount_point 未挂载，跳过"
    fi
done

echo "所有 OSD 挂载点处理完成"