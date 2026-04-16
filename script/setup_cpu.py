#!/usr/bin/python3
import yaml
from ssh_connect import ssh_command

def main():
    with open('../script/global_config.yaml', 'r') as f:
        g_cfg = yaml.safe_load(f)

    username = g_cfg['username']
    password = g_cfg['password']
    
    # 核心指令：自動補齊 uname -r、安裝，並用 cpupower 鎖定時脈
    cmd = '''
    sudo apt-get update -y > /dev/null && 
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y linux-tools-common linux-tools-$(uname -r) > /dev/null && 
    sudo cpupower frequency-set -g performance > /dev/null && 
    cpupower frequency-info | grep "current CPU frequency"
    '''
    
    all_nodes = g_cfg['servers'] + g_cfg['clients']
    
    print("=== 開始為所有機器安裝 cpupower 並設定最高時脈 ===")
    for node in all_nodes:
        ip = node['ip']
        print(f"正在處理 IP: {ip} ...", end=" ", flush=True)
        
        ssh, stdin, stdout, stderr = ssh_command(ip, username, password, cmd)
        exit_status = stdout.channel.recv_exit_status()
        
        if exit_status == 0:
            res = stdout.read().decode('utf-8').replace('\n', ' ').strip()
            print(f" [成功] 驗證結果: {res}")
        else:
            err = stderr.read().decode('utf-8').strip()
            print(f" [失敗] 錯誤訊息: {err}")
            
        ssh.close()

if __name__ == '__main__':
    main()
