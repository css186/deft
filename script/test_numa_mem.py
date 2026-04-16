#!/usr/bin/python3

import os
import re
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


def parse_int_field(line, key):
    match = re.search(rf'{key}=([0-9]+)', line)
    return int(match.group(1)) if match else None


def format_bytes(num_bytes):
    units = ["B", "KiB", "MiB", "GiB", "TiB"]
    value = float(num_bytes)
    for unit in units:
        if value < 1024.0 or unit == units[-1]:
            return f"{value:.2f} {unit}"
        value /= 1024.0


# start benchmark

subprocess.run(f'make -j', shell=True)

num_servers = 2
num_clients = 4

threads_CN_arr = [1, 2, 4, 8, 12, 16, 24, 32]
key_space_arr = [400e6]
read_ratio_arr = [95]
zipf_arr = [0.99]

# threads_CN_arr = [1, 2, 4, 8, 12, 16, 24, 32]
# key_space_arr = [100e5, 500e5, 100e6, 400e6, 800e6, 1200e6]
# read_ratio_arr = [100, 80, 60, 40, 20, 0]
# zipf_arr = [0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.99]

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
        num_prefill_threads = 30

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
            if 'numa_nodes' in g_cfg['servers'][i]:
                nodes = g_cfg['servers'][i]['numa_nodes']
                numa_bind_str = ','.join(map(str, nodes)) if isinstance(nodes, list) else str(nodes)
            else:
                numa_bind_str = str(rdma_nic_id)
                
            print(f'issue server {i} {ip} bind:{numa_bind_str} (RDMA NIC {rdma_nic_id})')
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
            if 'numa_nodes' in g_cfg['clients'][i]:
                nodes = g_cfg['clients'][i]['numa_nodes']
                numa_bind_str = ','.join(map(str, nodes)) if isinstance(nodes, list) else str(nodes)
            else:
                numa_bind_str = str(rdma_nic_id)
                
            print(f'issue client {i} {ip} bind:{numa_bind_str} (RDMA NIC {rdma_nic_id})')
            cmd = f'cd {exe_path} && sudo numactl --membind={numa_bind_str} --cpunodebind={numa_bind_str} ./{g_cfg["client_app"]} --server_count {num_servers} --client_count {num_clients} --numa_id {rdma_nic_id} --num_prefill_threads {num_prefill_threads} --num_bench_threads {num_threads} --key_space {key_space} --read_ratio {read_ratio} --zipf {zipf} > ../log/client_{i}.log 2>&1'
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

        fp.write("[Memory Statistics]\n")
        dsm_used_bytes = []
        dsm_usable_bytes = []
        server_rss_kb = []
        server_hugetlb_kb = []
        for i in range(num_servers):
            ip = g_cfg['servers'][i]['ip']
            remote_cmd = (
                f'cd {exe_path} && '
                f'grep "\\[Memory Utilization\\]" ../log/server_{i}.log | tail -1'
            )
            ssh, stdin, stdout, stderr = ssh_command(ip, username, password, remote_cmd)
            mem_line = stdout.read().decode("utf-8").strip()
            ssh.close()

            if mem_line:
                fp.write(f"Server {i}: {mem_line}\n")
                print(f"  Memory {i}: {mem_line}")
                used_bytes = parse_int_field(mem_line, "used_bytes")
                usable_bytes = parse_int_field(mem_line, "usable_bytes")
                if used_bytes is not None and usable_bytes is not None:
                    dsm_used_bytes.append(used_bytes)
                    dsm_usable_bytes.append(usable_bytes)

            proc_cmd = (
                f'cd {exe_path} && '
                f'grep "\\[Process Memory\\] Role server" ../log/server_{i}.log | tail -1'
            )
            ssh, stdin, stdout, stderr = ssh_command(ip, username, password, proc_cmd)
            proc_line = stdout.read().decode("utf-8").strip()
            ssh.close()

            if proc_line:
                fp.write(f"Server {i} Process: {proc_line}\n")
                vmrss = parse_int_field(proc_line, "VmRSS_kB")
                hugetlb = parse_int_field(proc_line, "HugetlbPages_kB")
                if vmrss is not None:
                    server_rss_kb.append(vmrss)
                if hugetlb is not None:
                    server_hugetlb_kb.append(hugetlb)

        client_rss_kb = []
        client_hugetlb_kb = []
        client_payload_bytes = []
        client_live_objects = []
        for i in range(num_clients):
            proc_subproc = subprocess.run(
                f'grep "\\[Process Memory\\] Role client" ../log/client_{i}.log | tail -1',
                stdout=subprocess.PIPE, shell=True)
            proc_line = proc_subproc.stdout.decode("utf-8").strip()
            if proc_line:
                fp.write(f"Client {i} Process: {proc_line}\n")
                vmrss = parse_int_field(proc_line, "VmRSS_kB")
                hugetlb = parse_int_field(proc_line, "HugetlbPages_kB")
                if vmrss is not None:
                    client_rss_kb.append(vmrss)
                if hugetlb is not None:
                    client_hugetlb_kb.append(hugetlb)

            obj_subproc = subprocess.run(
                f'grep "\\[Object Stats\\] Role client" ../log/client_{i}.log | tail -1',
                stdout=subprocess.PIPE, shell=True)
            obj_line = obj_subproc.stdout.decode("utf-8").strip()
            if obj_line:
                fp.write(f"Client {i} Object: {obj_line}\n")
                payload_bytes = parse_int_field(obj_line, "payload_bytes")
                live_objects = parse_int_field(obj_line, "live_objects")
                if payload_bytes is not None:
                    client_payload_bytes.append(payload_bytes)
                if live_objects is not None:
                    client_live_objects.append(live_objects)

        if dsm_usable_bytes:
            total_used = sum(dsm_used_bytes)
            total_usable = sum(dsm_usable_bytes)
            cluster_util = 0.0 if total_usable == 0 else total_used * 100.0 / total_usable
            fp.write(f"Average Memory Utilization: {cluster_util:.2f}%\n")
            fp.write(f"Cluster DSM Used: {format_bytes(total_used)} / {format_bytes(total_usable)} ({cluster_util:.2f}%)\n")
            if client_payload_bytes:
                total_payload = sum(client_payload_bytes)
                object_eff = 0.0 if total_used == 0 else total_payload * 100.0 / total_used
                fp.write(f"Approx Object-Level Efficiency: {object_eff:.4f}%\n")
                fp.write(f"Cluster Live Payload: {format_bytes(total_payload)}\n")
            else:
                fp.write("Approx Object-Level Efficiency: N/A\n")
                fp.write("Cluster Live Payload: N/A\n")
        else:
            fp.write("Average Memory Utilization: N/A\n")
            fp.write("Cluster DSM Used: N/A\n")
            fp.write("Approx Object-Level Efficiency: N/A\n")
            fp.write("Cluster Live Payload: N/A\n")

        if client_live_objects:
            fp.write(f"Cluster Live Objects: {sum(client_live_objects)}\n")
        else:
            fp.write("Cluster Live Objects: N/A\n")

        if server_rss_kb:
            fp.write(f"Average Server RSS: {sum(server_rss_kb) / len(server_rss_kb):.0f} kB\n")
        else:
            fp.write("Average Server RSS: N/A\n")

        if server_hugetlb_kb:
            fp.write(f"Average Server HugetlbPages: {sum(server_hugetlb_kb) / len(server_hugetlb_kb):.0f} kB\n")
        else:
            fp.write("Average Server HugetlbPages: N/A\n")

        if client_rss_kb:
            fp.write(f"Average Client RSS: {sum(client_rss_kb) / len(client_rss_kb):.0f} kB\n")
        else:
            fp.write("Average Client RSS: N/A\n")

        if client_hugetlb_kb:
            fp.write(f"Average Client HugetlbPages: {sum(client_hugetlb_kb) / len(client_hugetlb_kb):.0f} kB\n")
        else:
            fp.write("Average Client HugetlbPages: N/A\n")

        fp.write("\n")
        fp.flush()

        subprocess.run(f'../script/killall.py', stdout=subprocess.DEVNULL, shell=True)

        if job_id < len(product_list) - 1:
            time.sleep(10)
