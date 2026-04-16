#!/usr/bin/python3
import os
import time
import yaml
from ssh_connect import ssh_command

def get_rocev2_params(ip, username, password):
    # 自動分析伺服器上的 show_gids
    cmd = "show_gids"
    ssh, stdin, stdout, stderr = ssh_command(ip, username, password, cmd)
    exit_status = stdout.channel.recv_exit_status()
    lines = stdout.read().decode('utf-8').splitlines()
    ssh.close()
    
    for line in lines:
        # 從輸出的海量資訊中找出擁有自己 IP 且協定是 v2 的那一條
        if ip in line and 'v2' in line:
            parts = line.split()
            dev = parts[0]
            port = parts[1]
            gid_index = parts[2]
            return dev, port, gid_index
    return None, None, None

def main():
    with open('../script/global_config.yaml', 'r') as f:
        g_cfg = yaml.safe_load(f)

    username = g_cfg['username']
    password = g_cfg['password']

    # Server 0
    server_ip = g_cfg['servers'][0]['ip']
    server_nic_id = g_cfg['servers'][0]['numa_id']
    if 'numa_nodes' in g_cfg['servers'][0]:
        nodes = g_cfg['servers'][0]['numa_nodes']
        server_bind = ','.join(map(str, nodes)) if isinstance(nodes, list) else str(nodes)
    else:
        server_bind = str(server_nic_id)

    # Client 0
    client_ip = g_cfg['clients'][0]['ip']
    client_nic_id = g_cfg['clients'][0]['numa_id']
    if 'numa_nodes' in g_cfg['clients'][0]:
        nodes = g_cfg['clients'][0]['numa_nodes']
        client_bind = ','.join(map(str, nodes)) if isinstance(nodes, list) else str(nodes)
    else:
        client_bind = str(client_nic_id)

    print(">>> 正在自動抓取與實驗 IP 對應的正確 RDMA Device 與 RoCEv2 GID...")
    s_dev, s_port, s_gid = get_rocev2_params(server_ip, username, password)
    c_dev, c_port, c_gid = get_rocev2_params(client_ip, username, password)

    if not s_dev or not c_dev:
        print("抓取失敗！請確認 show_gids 有印出對應的 v2 資訊")
        return

    print(f"  > Server 0 ({server_ip}) :: Device: {s_dev}, Port: {s_port}, GID: {s_gid}")
    print(f"  > Client 0 ({client_ip}) :: Device: {c_dev}, Port: {c_port}, GID: {c_gid}")

    # 注意參數順序：IP 必須放在 Client 指令的最後面！ 
    server_cmd = f'sudo numactl --membind={server_bind} --cpunodebind={server_bind} ib_read_lat -d {s_dev} -i {s_port} -x {s_gid}'
    
    client_cmd = f'sudo numactl --membind={client_bind} --cpunodebind={client_bind} ib_read_lat -a -d mlx5_2 -i 1 -x 3 {server_ip}'    
    
    print(f"\n[Server] 執行指令: {server_cmd}")
    server_ssh, _, server_stdout, _ = ssh_command(server_ip, username, password, server_cmd)
    
    time.sleep(2)

    print(f"[Client] 執行指令: {client_cmd}\n")
    client_ssh, _, client_stdout, client_stderr = ssh_command(client_ip, username, password, client_cmd)

    print(">>> 等待測量結果中...\n")
    exit_status = client_stdout.channel.recv_exit_status()
    
    if exit_status == 0:
        print(client_stdout.read().decode('utf-8'))
    else:
        print("執行失敗！Stderr 錯誤訊息如下:")
        print(client_stderr.read().decode('utf-8'))

    # 收尾
    os.system('../script/killall.py')
    server_ssh.close()
    client_ssh.close()

if __name__ == '__main__':
    main()
