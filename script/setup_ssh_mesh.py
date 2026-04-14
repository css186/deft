#!/usr/bin/python3
"""
CloudLab SSH Mesh Setup Script
Reads IPs from global_config.yaml, generates a keypair on this node,
and uses 'sudo ssh' (root's pre-provisioned channel) to distribute
the public key to all nodes' authorized_keys.

Must be run from the build/ directory.
"""

import yaml
import subprocess
import os
import sys

with open('../script/global_config.yaml', 'r') as f:
    g_cfg = yaml.safe_load(f)


def get_all_unique_ips():
    """Extract unique IPs from both clients and servers."""
    ip_set = set()
    ips = []
    for node in g_cfg.get('clients', []) + g_cfg.get('servers', []):
        ip = node['ip']
        if ip not in ip_set:
            ip_set.add(ip)
            ips.append(ip)
    return ips


def generate_keypair():
    """Generate a fresh RSA keypair for the current user."""
    home = os.path.expanduser('~')
    key_path = os.path.join(home, '.ssh', 'id_rsa')
    pub_path = key_path + '.pub'

    # Remove old keys if they exist
    for f in [key_path, pub_path]:
        if os.path.exists(f):
            os.remove(f)

    print('[SSH Mesh] Generating new RSA keypair...')
    ret = subprocess.run(
        ['ssh-keygen', '-m', 'PEM', '-t', 'rsa', '-N', '', '-f', key_path, '-q'],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    if ret.returncode != 0:
        print('[FATAL] ssh-keygen failed: ' + ret.stderr.decode())
        sys.exit(1)

    with open(pub_path, 'r') as f:
        pubkey = f.read().strip()

    print('[SSH Mesh] Keypair generated successfully.')
    return pubkey


def distribute_key(ip, username, pubkey):
    """Use sudo ssh (root channel) to append pubkey to a node's authorized_keys."""
    cmd = [
        'sudo', 'ssh',
        '-o', 'StrictHostKeyChecking=no',
        '-o', 'UserKnownHostsFile=/dev/null',
        '-o', 'LogLevel=ERROR',
        ip,
        'echo "{}" >> /users/{}/.ssh/authorized_keys'.format(pubkey, username)
    ]
    ret = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if ret.returncode == 0:
        print('[SUCCESS] Key deployed to {}'.format(ip))
    else:
        print('[FAILED]  {} : {}'.format(ip, ret.stderr.decode().strip()))
    return ret.returncode == 0


def setup_ssh_config():
    """Write SSH config to disable host key checking for automation scripts."""
    config_path = os.path.expanduser('~/.ssh/config')
    config_content = "Host *\n\tStrictHostKeyChecking no\n\tUserKnownHostsFile /dev/null\n"
    with open(config_path, 'w') as f:
        f.write(config_content)
    os.chmod(config_path, 0o600)
    print('[SSH Mesh] SSH config updated.')


def verify(ip, username):
    """Verify that normal (non-root) SSH works to the given IP."""
    ret = subprocess.run(
        ['ssh', '-o', 'StrictHostKeyChecking=no',
         '-o', 'UserKnownHostsFile=/dev/null',
         '-o', 'LogLevel=ERROR',
         '-o', 'BatchMode=yes',
         '{}@{}'.format(username, ip),
         'hostname'],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    return ret.returncode == 0, ret.stdout.decode().strip()


if __name__ == '__main__':
    username = g_cfg['username']
    ips = get_all_unique_ips()

    print('=============================================')
    print(' CloudLab SSH Mesh Setup')
    print(' Nodes: {}'.format(len(ips)))
    print('=============================================')

    # Step 1: Generate keypair
    pubkey = generate_keypair()

    # Step 2: Write SSH config
    setup_ssh_config()

    # Step 3: Distribute public key to all nodes via root channel
    print('\n--- Distributing public key via sudo ssh ---')
    failures = []
    for ip in ips:
        ok = distribute_key(ip, username, pubkey)
        if not ok:
            failures.append(ip)

    if failures:
        print('\n[WARNING] Failed to deploy key to: {}'.format(', '.join(failures)))
        sys.exit(1)

    # Step 4: Verify connectivity
    print('\n--- Verifying normal SSH connectivity ---')
    all_ok = True
    for ip in ips:
        ok, hostname = verify(ip, username)
        if ok:
            print('[VERIFIED] {} -> {}'.format(ip, hostname))
        else:
            print('[FAILED]   {} cannot connect'.format(ip))
            all_ok = False

    if all_ok:
        print('\n===== ALL {} NODES CONNECTED! SSH mesh is ready. ====='.format(len(ips)))
    else:
        print('\n[ERROR] Some nodes failed verification.')
        sys.exit(1)
