#!/usr/bin/python3
import os
import rados
import time



total_write_time = 0
total_read_time = 0

def traverse_directory_one_layer(directory):
    try:
        # 获取指定目录下的所有条目
        entries = os.listdir(directory)
        # 过滤出子目录的绝对路径
        subdirectories = [os.path.abspath(os.path.join(directory, entry)) for entry in entries if os.path.isdir(os.path.join(directory, entry))]
        return subdirectories
    except FileNotFoundError:
        print(f"目录 '{directory}' 未找到。")
        return []
    except PermissionError:
        print(f"没有权限访问目录 '{directory}'。")
        return []


def traverse_directory_write(directory, conf_path, pool_name):
    # 配置 Ceph 集群连接
    cluster = rados.Rados(conffile=conf_path)
    cluster.connect()

    # 检查存储池是否存在
    if not cluster.pool_exists(pool_name):
        print(f"存储池 '{pool_name}' 不存在。")
        cluster.shutdown()
        exit(1)

    # 打开存储池上下文
    ioctx = cluster.open_ioctx(pool_name)

    for root, _ , files in os.walk(directory):
        for file_name in files:
            file_path = os.path.join(root, file_name)
            process_file(file_path, ioctx)

    ioctx.close()
    cluster.shutdown()


def process_file(file_path, ioctx):
    global total_write_time
    chunk_size = 2 * 1024 * 1024

    if file_path.endswith('.log'):
        with open(file_path, 'rb') as file:
            file_size = os.path.getsize(file_path)
            if file_size > chunk_size:
                # 文件大于 2 MB，进行切片存储
                chunk_index = 0
                while chunk := file.read(chunk_size):
                    object_name = f"{file_path}_chunk_{chunk_index}"
                    start_time = time.time()
                    ioctx.write(object_name, chunk)
                    end_time = time.time()
                    total_write_time += (end_time - start_time) 

                    chunk_index += 1
            else:
                # 文件小于等于 2 MB，直接存储
                data = file.read()
                start_time = time.time()
                ioctx.write(file_path, data)
                end_time = time.time()
                total_write_time += (end_time - start_time) 

def measure_read_performance(cluster_conf, pool_name):
    try:
        # 连接到 Ceph 集群
        cluster = rados.Rados(conffile=cluster_conf)
        cluster.connect()
        print(f"Connected to cluster: {cluster.get_fsid()}")

        # 打开指定的池
        if not cluster.pool_exists(pool_name):
            print(f"Pool '{pool_name}' does not exist.")
            return

        ioctx = cluster.open_ioctx(pool_name)

        # 初始化总读取时间和总读取字节数
        total_read_time = 0
        total_read_bytes = 0

        # 遍历池中的所有对象并测量读取时间
        for obj in ioctx.list_objects():
            object_name = obj.key
            try:
                start_time = time.time()
                data = ioctx.read(object_name, 4*1024*1024)
                end_time = time.time()

                read_time = end_time - start_time
                read_bytes = len(data)

                total_read_time += read_time
                total_read_bytes += read_bytes

                # print(f"Read object '{object_name}': {read_bytes} bytes in {read_time:.6f} seconds")

            except rados.Error as e:
                print(f"Failed to read object '{object_name}': {e}")

        # 计算平均读取速度（吞吐量）
        if total_read_time > 0:
            throughput = total_read_bytes / total_read_time
            print(f"\nTotal read bytes: {total_read_bytes} bytes")
            print(f"Total read time: {total_read_time:.6f} seconds")
            print(f"Average throughput: {throughput:.2f} bytes/second")
        else:
            print("No objects were read.")

        # 关闭 IO 上下文
        ioctx.close()

    except rados.Error as e:
        print(f"Error: {e}")

    finally:
        # 关闭集群连接
        cluster.shutdown()

def delete_all_objects(pool_name):
    try:
        # 连接 Ceph 集群
        cluster = rados.Rados(conffile='/etc/ceph/ceph.conf')
        cluster.connect()

        # 访问指定 pool
        ioctx = cluster.open_ioctx(pool_name)

        # 列出所有对象
        objects = ioctx.list_objects()
        for obj in objects:
            obj_name = obj.key
            ioctx.remove_object(obj_name)
            # print(f"Deleted object: {obj_name}")

        # 关闭 pool 连接
        ioctx.close()
        cluster.shutdown()
        print(f"Pool '{pool_name}' 已清空。")

    except Exception as e:
        print(f"删除失败: {e}")

if __name__ == '__main__':    
    #所有目录 写性能测试
    # directory_to_scan = '/home/cyf/ssd/benchmark_log_files/'  # 替换为您要扫描的目录路径
    # workloads = traverse_directory_one_layer(directory_to_scan)
    # workloads = sorted(workloads)

    # delete_all_objects('po1')
    
    # for workload in workloads:
    #     traverse_directory_write(workload, '/etc/ceph/ceph.conf', 'po1') 
    #     print(f"{workload} total write time: {total_write_time:.6f} s")

    #     measure_read_performance('/etc/ceph/ceph.conf', 'po1')
    
    #     delete_all_objects('po1')
    #     total_write_time = 0
    #     total_read_time = 0
    
    # 单个目录
    workload = '/home/cyf/ssd/benchmark_log_files/Zookeeper'
    traverse_directory_write(workload, '/etc/ceph/ceph.conf', 'po1') 
    print(f"{workload} total write time: {total_write_time:.6f} s")
    measure_read_performance('/etc/ceph/ceph.conf', 'po1')

