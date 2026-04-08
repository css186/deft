#!/usr/bin/python3
import yaml
from ssh_connect import ssh_command
import concurrent.futures

with open('../script/global_config.yaml', 'r') as f:
    g_cfg = yaml.safe_load(f)

def run_phase2_on_node(ip, username, password):
    print(f'[Phase 2] Starting comprehensive installation on Node {ip} ...')

    cmd = """
export DEBIAN_FRONTEND=noninteractive
set -e

# 1. Update & RDMA Core
sudo apt-get update
sudo apt-get install -y infiniband-diags rdma-core wget curl git

# 2. Install MLNX_OFED 4.9 Driver
cd /tmp
if [ ! -d "MLNX_OFED_LINUX-4.9-5.1.0.0-ubuntu18.04-x86_64" ]; then
    echo "Downloading MLNX_OFED..."
    wget -q -O ofed.tgz https://content.mellanox.com/ofed/MLNX_OFED-4.9-5.1.0.0/MLNX_OFED_LINUX-4.9-5.1.0.0-ubuntu18.04-x86_64.tgz
    tar -xf ofed.tgz
    rm -rf /tmp/MLNX_OFED
    mv MLNX_OFED_LINUX-4.9-5.1.0.0-ubuntu18.04-x86_64 /tmp/MLNX_OFED
fi
cd /tmp/MLNX_OFED
echo "Installing MLNX_OFED (This takes 5-10 minutes)..."
sudo ./mlnxofedinstall --with-mme --with-vma --force
sudo ldconfig

# 3. Python dependencies
sudo apt-get install -y python3-pip
python3 -m pip install --upgrade pip setuptools wheel
pip3 install paramiko pyyaml

# 4. Standard APT Dependencies
sudo apt-get install -y memcached libboost-coroutine-dev libboost-context-dev libboost-system-dev libmemcached-dev libgflags-dev numactl

# 5. CityHash compilation and install (Local to OS)
cd /tmp
if [ ! -d "cityhash" ]; then
    git clone https://github.com/google/cityhash.git
fi
cd cityhash
./configure
make -j$(nproc)
sudo make install
sudo ldconfig

# 6. GCC-11 Upgrade
sudo apt-get install -y software-properties-common
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get install -y gcc-11 g++-11
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-11 100 --slave /usr/bin/g++ g++ /usr/bin/g++-11

# 7. KitWare CMake Upgrade
sudo apt-get remove --purge -y cmake || true
sudo apt-get install -y lsb-release
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | sudo tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null
sudo apt-add-repository -y "deb https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main"
sudo apt-get update
sudo apt-get install -y cmake

echo "ALL PHASE 2 INSTALLATIONS COMPLETE!"
    """
    # Wrap command: redirect all output to a log file to prevent paramiko buffer deadlock
    wrapped_cmd = f"bash -c '{cmd}' > /tmp/phase2_install.log 2>&1; echo $?"

    try:
        ssh, stdin, stdout, stderr = ssh_command(ip, username, password, wrapped_cmd)
        result = stdout.read().decode('utf-8').strip()
        exit_status = int(result) if result.isdigit() else 1

        if exit_status == 0:
            print(f'[Phase 2 - SUCCESS] Node {ip} setup completed.')
        else:
            print(f'[Phase 2 - FAILED] Node {ip} exit code: {exit_status}')
            # Fetch the last 1000 chars of the remote log
            ssh2, _, log_out, _ = ssh_command(ip, username, password, 'tail -30 /tmp/phase2_install.log')
            print(log_out.read().decode('utf-8'))
            ssh2.close()
        ssh.close()
    except Exception as e:
        print(f'[Phase 2 - ERROR] SSH failed on Node {ip}: {e}')

def setup_phase2():
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
            futures.append(executor.submit(run_phase2_on_node, ip, username, password))

        concurrent.futures.wait(futures)

if __name__ == '__main__':
    print("=====================================================")
    print(" PHASE 2: Parallel OFED, CityHash, GCC & Dependencies")
    print("=====================================================")
    setup_phase2()
    print("All tasks finished.")
