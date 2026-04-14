#!/usr/bin/python3
import os
import yaml
import paramiko

with open('../script/global_config.yaml', 'r') as f:
    g_cfg = yaml.safe_load(f)

def ssh_command(ip, username, password, cmd):
    ssh = paramiko.SSHClient()
    ssh.load_system_host_keys()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    key_path = os.path.expanduser('~/.ssh/id_rsa')
    ssh.connect(ip, username=username, key_filename=key_path, look_for_keys=True)
    stdin, stdout, stderr = ssh.exec_command(cmd)
    return ssh, stdin, stdout, stderr
