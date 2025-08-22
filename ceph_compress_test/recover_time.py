import rados
import json
import time
from datetime import datetime

def monitor_recovery_duration(conf_path, check_interval=0.1):
    """
    监控Ceph集群恢复持续时间
    只要不是active+clean状态就判定为恢复状态
    :param conf_path: Ceph配置文件路径
    :param check_interval: 检查间隔(秒)
    """
    cluster = None
    recovery_start = None
    last_status = None
    
    try:
        # 连接到Ceph集群
        cluster = rados.Rados(conffile=conf_path)
        cluster.connect()
        print("成功连接到Ceph集群，开始监控... (Ctrl+C停止)")
        print("等待检测到恢复状态...")

        while True:
            # 获取集群状态
            cmd = {'prefix': 'status', 'format': 'json'}
            ret, buf, err = cluster.mon_command(json.dumps(cmd), b'', timeout=5)
            
            if ret != 0:
                print(f"获取集群状态失败: {err}")
                time.sleep(check_interval)
                continue
                
            status = json.loads(buf.decode('utf-8'))
            pg_status = status.get('pgmap', {}).get('pgs_by_state', [])
            
            # 检查是否有非active+clean状态
            is_active_clean_only = all(
                state['state_name'] == 'active+clean' 
                for state in pg_status
            )
            
            current_time = datetime.now()
            
            # 状态变化检测
            if not is_active_clean_only and last_status != 'recovering':
                # 进入恢复状态
                recovery_start = current_time
                last_status = 'recovering'
                print(f"\n[{current_time}] 检测到集群进入恢复状态")
                print("当前PG状态分布:")
                for state in pg_status:
                    print(f"  {state['state_name']}: {state['count']}")
            elif is_active_clean_only and last_status == 'recovering':
                # 恢复完成
                recover_data_size = 4*1024*1024*1024
                recovery_duration = (current_time - recovery_start).total_seconds()
                last_status = 'active+clean'
                # 恢复速度
                recovery_speed = recover_data_size / recovery_duration
                print("\n恢复完成，持续时间:")
                print(f"  {recovery_duration:.2f}秒")
                print("恢复速度:")
                print(f"  {recovery_speed:.2f}Bytes/s")
                # print(f"[{current_time}] 集群恢复完成，持续时间: {recovery_duration:.2f}秒")
                print("当前PG状态分布:")
                for state in pg_status:
                    print(f"  {state['state_name']}: {state['count']}")
            
            time.sleep(check_interval)
            
    except KeyboardInterrupt:
        print("\n监控已停止")
    except Exception as e:
        print(f"发生错误: {e}")
    finally:
        if cluster:
            cluster.shutdown()

if __name__ == "__main__":
    # 配置参数
    ceph_conf = "/etc/ceph/ceph.conf"  # Ceph配置文件路径
    interval = 0.1  # 检查间隔(秒)
    
    monitor_recovery_duration(ceph_conf, interval)