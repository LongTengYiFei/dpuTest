import rados
import json
import time
from datetime import datetime

def is_in_recovery_state(pg_status):
    """
    判断是否处于恢复状态：存在recovering、degraded、backfilling等状态
    """
    recovery_keywords = ['recovering', 'backfilling', 'degraded', 'remapped', 
                        'backfill_wait', 'backfill_toofull', 'incomplete']
    
    for state in pg_status:
        state_name = state['state_name']
        # 如果状态中包含任何恢复相关的关键词
        if any(keyword in state_name for keyword in recovery_keywords):
            return True
    return False

def monitor_recovery_duration(conf_path, check_interval=0.5):
    """
    监控Ceph集群恢复持续时间
    检测到recovering、degraded等状态判定为恢复状态
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
            num_pgs = status.get('pgmap', {}).get('num_pgs', 0)
            
            # 使用正确的恢复状态判断
            in_recovery = is_in_recovery_state(pg_status)
            
            current_time = datetime.now()
            
            # 状态变化检测
            if in_recovery and last_status != 'recovering':
                # 进入恢复状态
                recovery_start = current_time
                last_status = 'recovering'
                print(f"\n[{current_time}] 检测到集群进入恢复状态")
                print("当前PG状态分布:")
                for state in pg_status:
                    if state['count'] > 0:
                        print(f"  {state['state_name']}: {state['count']}")
                        
            elif not in_recovery and last_status == 'recovering':
                # 恢复完成
                recovery_duration = (current_time - recovery_start).total_seconds()
                last_status = 'active+clean'
                
                # 获取恢复数据量估算
                pgmap = status.get('pgmap', {})
                bytes_recovered = pgmap.get('recovering_bytes_per_sec', 0) * recovery_duration
                
                print(f"\n[{current_time}] 恢复完成!")
                print(f"恢复持续时间: {recovery_duration:.2f}秒 ({recovery_duration/60:.2f}分钟)")
                if bytes_recovered > 0:
                    print(f"估算恢复数据量: {bytes_recovered/1024/1024:.2f} MB")
                    print(f"平均恢复速度: {bytes_recovered/recovery_duration/1024/1024:.2f} MB/s")
                print("恢复完成后PG状态分布:")
                for state in pg_status:
                    if state['count'] > 0:
                        print(f"  {state['state_name']}: {state['count']}")
            
            # 如果正在恢复中，定期显示进度
            elif last_status == 'recovering':
                elapsed = (current_time - recovery_start).total_seconds()
                # 每10秒打印一次进度
                if int(elapsed) % 10 == 0:
                    recovering_pgs = sum(state['count'] for state in pg_status 
                                       if is_in_recovery_state([state]))
                    total_pgs = sum(state['count'] for state in pg_status)
                    print(f"[{current_time}] 恢复中... 已进行{elapsed:.1f}秒, "
                          f"恢复中PG: {recovering_pgs}/{total_pgs}")
            
            time.sleep(check_interval)
            
    except KeyboardInterrupt:
        print("\n监控已停止")
        if recovery_start and last_status == 'recovering':
            print("警告: 恢复被中断，未完成!")
    except Exception as e:
        print(f"发生错误: {e}")
    finally:
        if cluster:
            cluster.shutdown()

if __name__ == "__main__":
    # 配置参数
    ceph_conf = "/etc/ceph/ceph.conf"  # Ceph配置文件路径
    interval = 0.5  # 检查间隔(秒)
    
    monitor_recovery_duration(ceph_conf, interval)