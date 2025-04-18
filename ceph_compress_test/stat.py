import rados
import json

def get_ceph_status(conf_path, pool_name=None):
    try:
        # 连接到Ceph集群
        cluster = rados.Rados(conffile=conf_path)
        cluster.connect()
        print("成功连接到Ceph集群")

        # 如果指定了存储池，检查是否存在
        if pool_name:
            if not cluster.pool_exists(pool_name):
                print(f"存储池 '{pool_name}' 不存在。")
                cluster.shutdown()
                return None
            
            # 打开存储池上下文
            ioctx = cluster.open_ioctx(pool_name)
            print(f"成功打开存储池 '{pool_name}' 的IO上下文")

        # 获取集群整体状态
        cmd = {'prefix': 'status', 'format': 'json'}
        ret, buf, err = cluster.mon_command(json.dumps(cmd), b'', timeout=5)
        
        if ret != 0:
            print(f"获取集群状态失败: {err}")
            cluster.shutdown()
            return None
            
        status = json.loads(buf.decode('utf-8'))
        print("\n集群整体状态:")
        print(f"健康状态: {status.get('health', {}).get('status')}")
        print(f"已用空间: {status.get('pgmap', {}).get('bytes_used', 0) / 1024**3:.2f} GB")
        print(f"总空间: {status.get('pgmap', {}).get('bytes_total', 0) / 1024**3:.2f} GB")

        # 获取PG状态分布
        pg_status = status.get('pgmap', {}).get('pgs_by_state', [])
        print("\nPG状态分布:")
        for state in pg_status:
            print(f"{state['state_name']}: {state['count']}")

        # 获取更详细的PG状态信息 - 增加错误处理和兼容性检查
        cmd = {'prefix': 'pg dump', 'format': 'json'}
        ret, buf, err = cluster.mon_command(json.dumps(cmd), b'', timeout=10)
        
        if ret == 0:
            try:
                pg_dump = json.loads(buf.decode('utf-8'))
                
                # 检查不同版本的返回结构
                pg_stats = pg_dump.get('pg_stats', pg_dump.get('pg_map', {}).get('pg_stats', []))
                
                if not pg_stats:
                    print("\n警告: 无法获取详细的PG统计信息，返回的数据结构可能已更改")
                else:
                    # 分析特定状态的PG
                    for target_state in ['active+clean', 'recovering', 'backfill']:
                        target_pgs = [
                            pg for pg in pg_stats 
                            if isinstance(pg, dict) and target_state in pg.get('state', '').split('+')
                        ]
                        print(f"\n{target_state}状态的PG数量: {len(target_pgs)}")
                        
                        # 如果需要，可以打印出具体的PG ID
                        if len(target_pgs) > 0 and len(target_pgs) < 5:
                            print(f"具体PG: {[pg.get('pgid', '未知') for pg in target_pgs]}")
            except Exception as e:
                print(f"\n解析PG详细信息时出错: {e}")
        else:
            print(f"\n获取PG详细信息失败: {err}")

        # 获取OSD状态
        cmd = {'prefix': 'osd dump', 'format': 'json'}
        ret, buf, err = cluster.mon_command(json.dumps(cmd), b'', timeout=5)
        
        if ret == 0:
            try:
                osd_dump = json.loads(buf.decode('utf-8'))
                up_osds = [osd for osd in osd_dump.get('osds', []) if osd.get('up', 0) == 1]
                in_osds = [osd for osd in osd_dump.get('osds', []) if osd.get('in', 0) == 1]
                print(f"\nOSD状态: 总数 {len(osd_dump.get('osds', []))}, 运行中 {len(up_osds)}, 在集群中 {len(in_osds)}")
            except Exception as e:
                print(f"\n解析OSD信息时出错: {e}")
        else:
            print(f"\n获取OSD信息失败: {err}")

        return status

    except Exception as e:
        print(f"发生错误: {e}")
        return None
    finally:
        if 'cluster' in locals():
            cluster.shutdown()

if __name__ == "__main__":
    # 示例用法
    conf_path = "/etc/ceph/ceph.conf"  # 你的ceph配置文件路径
    pool_name = "po1"  # 你要检查的存储池名称
    
    status = get_ceph_status(conf_path, pool_name)
    if status:
        print("\n成功获取集群状态信息")