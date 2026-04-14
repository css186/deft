#!/usr/bin/python3

import os
import sys
import subprocess
import time
import yaml
import killall
from itertools import product
from time import gmtime, strftime
from ssh_connect import ssh_command

if not os.path.exists('../result'):
    os.makedirs('../result')
if not os.path.exists('../log'):
    os.makedirs('../log')

def get_res_name(s):
    postfix = ""
    if len(sys.argv) > 1:
        postfix = sys.argv[1]
    return '../result/' + s + "-" + postfix + strftime("-%m-%d-%H-%M", gmtime())  + ".txt"

# async
def exec_command(command):
    return subprocess.Popen(command, shell=True)


# start benchmark

subprocess.run(f'make -j', shell=True)

num_servers = 2
num_clients = 4

# 已依據需求幫你更新為 32 個 Threads
threads_CN_arr = [32]
key_space_arr = [400e6]
read_ratio_arr = [50]
zipf_arr = [0.99]

# threads_CN_arr = [1] + [i for i in range(3, 31, 3)]
# key_space_arr = [100e6, 200e6, 800e6, 1200e6]
# read_ratio_arr = [80, 60, 40, 20, 0]
# zipf_arr = [0.6, 0.7, 0.8, 0.9]

print(threads_CN_arr)
print(key_space_arr)
print(read_ratio_arr)
print(zipf_arr)

file_name = get_res_name("bench")

with open('../script/global_config.yaml', 'r') as f:
    g_cfg = yaml.safe_load(f)

exe_path = f'{g_cfg["src_path"]}/{g_cfg["app_rel_path"]}'
username = g_cfg['username']
password = g_cfg['password']

print(exe_path)


with open(file_name, 'w') as fp:
    product_list = list(product(key_space_arr, read_ratio_arr, zipf_arr, threads_CN_arr))

    for job_id, (key_space, read_ratio, zipf, num_threads) in enumerate( product_list):
        key_space = int(key_space)
        # 已幫你更新為 32
        num_prefill_threads = 32

        print(f'start: {num_threads * num_clients} {num_clients} {num_threads} {key_space} {read_ratio} {zipf}')
        fp.write(f'total_threads: {num_threads * num_clients} num_servers: {num_servers} num_clients: {num_clients} num_threads: {num_threads} key_space: {key_space} read_ratio: {read_ratio} zipf: {zipf}\n')
        fp.flush()

        # start servers:
        subprocess.run(f'../script/restartMemc.sh', shell=True)

        server_sshs = []
        server_stdouts = []
        for i in range(num_servers):
            ip = g_cfg['servers'][i]['ip']
            rdma_nic_id = g_cfg['servers'][i]['numa_id']
            # 動態解析 yaml 裡面的 numa_nodes 陣列
            if 'numa_nodes' in g_cfg['servers'][i]:
                nodes = g_cfg['servers'][i]['numa_nodes']
                numa_bind_str = ','.join(map(str, nodes)) if isinstance(nodes, list) else str(nodes)
            else:
                numa_bind_str = str(rdma_nic_id)
                
            print(f'issue server {i} {ip} bind:{numa_bind_str} (RDMA NIC {rdma_nic_id})')
            # 確保使用 membind (Local Allocation)，拋棄 interleave
            cmd = f'cd {exe_path} && sudo sh -c "echo 3 > /proc/sys/vm/drop_caches" && sudo numactl --membind={numa_bind_str} --cpunodebind={numa_bind_str} ./{g_cfg["server_app"]} --server_count {num_servers} --client_count {num_clients} --numa_id {rdma_nic_id} > ../log/server_{i}.log 2>&1'

            ssh, stdin, stdout, stderr = ssh_command(ip, username, password, cmd)
            server_sshs.append((ssh, stderr))
            server_stdouts.append(stdout)
            time.sleep(1)

        time.sleep(1)
        # start clients:

        client_sshs = []
        client_stdouts = []
        for i in range(num_clients):
            ip = g_cfg['clients'][i]['ip']
            rdma_nic_id = g_cfg['clients'][i]['numa_id']
            # 動態解析 yaml 裡面的 numa_nodes 陣列
            if 'numa_nodes' in g_cfg['clients'][i]:
                nodes = g_cfg['clients'][i]['numa_nodes']
                numa_bind_str = ','.join(map(str, nodes)) if isinstance(nodes, list) else str(nodes)
            else:
                numa_bind_str = str(rdma_nic_id)
                
            print(f'issue client {i} {ip} bind:{numa_bind_str} (RDMA NIC {rdma_nic_id})')
            # 同樣確保使用 membind
            cmd = f'cd {exe_path} && sudo numactl --membind={numa_bind_str} --cpunodebind={numa_bind_str} gdb -q -batch -ex run -ex bt -ex quit --args ./{g_cfg["client_app"]} --server_count {num_servers} --client_count {num_clients} --numa_id {rdma_nic_id} --num_prefill_threads {num_prefill_threads} --num_bench_threads {num_threads} --key_space {key_space} --read_ratio {read_ratio} --zipf {zipf} > ../log/client_{i}.log 2>&1'
            ssh, stdin, stdout, stderr = ssh_command(ip, username, password, cmd)
            client_sshs.append((ssh, stderr))
            client_stdouts.append(stdout)
            if i == 0:
                time.sleep(1)
            time.sleep(1)

        # wait
        finish = False
        has_error = False
        while not finish and not has_error:
            time.sleep(1)
            finish = True
            for i in range(num_servers):
                if server_stdouts[i].channel.exit_status_ready():
                    if server_stdouts[i].channel.recv_exit_status() != 0:
                        has_error = True
                        err = server_sshs[i][1].read().decode().strip()
                        print(f'server {i} error! SSH stderr: {err}')
                        break
                else:
                    finish = False
            
            if not has_error:
                for i in range(num_clients):
                    if client_stdouts[i].channel.exit_status_ready():
                        if client_stdouts[i].channel.recv_exit_status() != 0:
                            has_error = True
                            err = client_sshs[i][1].read().decode().strip()
                            print(f'client {i} error! SSH stderr: {err}')
                            break
                    else:
                        finish = False

        if has_error:
            killall.killall()
        
        for i in range(num_servers):
            server_sshs[i][0].close()
        for i in range(num_clients):
            client_sshs[i][0].close()


        loading_subproc = subprocess.run(f'grep "Loading Results" ../log/client_0.log', stdout=subprocess.PIPE, shell=True)
        tmp = loading_subproc.stdout.decode("utf-8")
        print(tmp.strip())
        fp.write(tmp)

        res_subproc = subprocess.run(f'grep "Final Results" ../log/client_0.log', stdout=subprocess.PIPE, shell=True)
        tmp = res_subproc.stdout.decode("utf-8")
        print(tmp)
        fp.write(tmp + "\n")
        fp.flush()

        subprocess.run(f'../script/killall.py', stdout=subprocess.DEVNULL, shell=True)

        if job_id < len(product_list) - 1:
            time.sleep(10)
