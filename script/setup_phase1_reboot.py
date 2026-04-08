#!/usr/bin/python3
import yaml
import time
from ssh_connect import ssh_command
import concurrent.futures

with open('../script/global_config.yaml', 'r') as f:
    g_cfg = yaml.safe_load(f)

def run_phase1_on_node(ip, username, password):
    print(f'[Phase 1] Connecting to Node {ip} to update GRUB and reboot...')

    cmd = """
    # check iommu
    if ! grep -q "amd_iommu=off" /etc/default/grub; then
        echo "Modifying GRUB for IOMMU bypass..."
        sudo sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="/GRUB_CMDLINE_LINUX_DEFAULT="iommu=pt amd_iommu=off/' /etc/default/grub
        sudo update-grub
    else
        echo "GRUB already configured."
    fi
    # reboot
    nohup sh -c 'sleep 10 && sudo reboot' > /dev/null 2>&1 &
    """

    try:
        ssh, stdin, stdout, stderr = ssh_command(ip, username, password, cmd)
        time.sleep(1)
        ssh.close()
        print(f'[Phase 1 - SUCCESS] Node {ip} has been instructed to reboot.')
    except Exception as e:
        print(f'[Phase 1 - ERROR] Failed to run Phase 1 on {ip}: {e}')

def setup_phase1():
    ip_set = set()
    username = g_cfg['username']
    password = g_cfg['password']

    all_nodes = g_cfg['clients'] + g_cfg['servers']

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(all_nodes)) as executor:
        futures = []
        for i in range(len(all_nodes)):
            ip = all_nodes[i]['ip']
            if ip in ip_set:
                continue
            ip_set.add(ip)
            futures.append(executor.submit(run_phase1_on_node, ip, username, password))

        concurrent.futures.wait(futures)

if __name__ == '__main__':
    print("=====================================================")
    print(" PHASE 1: Modifying GRUB for AMD IOMMU & Rebooting")
    print("=====================================================")
    setup_phase1()
    print("All nodes have been instructed to reboot.")
    print("Please wait ~3 minutes before running setup_phase2_install.py.")
