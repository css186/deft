#!/usr/bin/python3

import yaml
from ssh_connect import ssh_command

with open('../script/global_config.yaml', 'r') as f:
    g_cfg = yaml.safe_load(f)

def killall():
    ip_set = set()
    username=g_cfg['username']
    password=g_cfg['password']
    all_nodes = g_cfg['clients'] + g_cfg['servers']
    for i in range(len(all_nodes)):
        ip = all_nodes[i]['ip']
        if ip in ip_set:
            continue
        ip_set.add(ip)
        print(f'killall {ip}')

        cmd = f'killall -9 {g_cfg["client_app"]} > /dev/null; killall -9 {g_cfg["server_app"]} > /dev/null'
        ssh, stdin, stdout, stderr = ssh_command(ip, username, password, cmd)
        ssh.close()

if __name__ == '__main__':
    killall()
    print('done.')
