#!/bin/bash
# script/modprobe_reset.sh

cd "$(dirname "$0")"

PYTHON_SCRIPT=$(cat << 'EOF'
import sys

domain = ""
with open("/etc/hosts") as f:
    for line in f:
        parts = line.split()
        for p in parts:
            if p.startswith("localhost.") and "cloudlab.us" in p:
                domain = p.replace("localhost.", "", 1)
                break

ip_to_fqdn = {}
with open("/etc/hosts") as f:
    for line in f:
        if line.startswith("10.10.1."):
            parts = line.split()
            ip = parts[0]                                                                                                                                           # 取最後一個別名當主機名 (例如: cn-1)
            short_name = parts[-1]
            ip_to_fqdn[ip] = f"{short_name}.{domain}" if domain else short_name

import yaml
try:
    with open('global_config.yaml', 'r') as f:
        cfg = yaml.safe_load(f)
    ips = set()                                                                                                                                             for s in cfg.get('servers', []): ips.add(s['ip'])
    for c in cfg.get('clients', []): ips.add(c['ip'])

    for ip in ips:
        if ip in ip_to_fqdn:
            print(f"{ip},{ip_to_fqdn[ip]}")                                                                                                             except Exception as e:
    pass
EOF                                                                                                                                                     )
MAPPINGS=$(python3 -c "$PYTHON_SCRIPT")

if [ -z "$MAPPINGS" ]; then
    echo "Cannot parse HOST, please check global_config.yaml"
    exit 1
fi

                                                                                                                                                      for map in $MAPPINGS; do
    ip=$(echo $map | cut -d',' -f1)
    fqdn=$(echo $map | cut -d',' -f2)
    echo "=== Restart driver and setup connection: $ip (via $fqdn) ==="

    ssh -o StrictHostKeyChecking=no brianch@$fqdn "
        sudo rmmod rpcrdma 2>/dev/null || true
        sudo /etc/init.d/openibd restart
        sudo modprobe mlx5_core

        sudo sed -i '/enp65s0f0/d' /etc/network/interfaces
        sudo sed -i '/${ip}/d' /etc/network/interfaces

        sudo bash -c 'cat >> /etc/network/interfaces <<NET_EOF
auto enp65s0f0                                                                                                                                          iface enp65s0f0 inet static                                                                                                                                 address ${ip}
    netmask 255.255.255.0
NET_EOF'

        sudo ip link set enp65s0f0 up
        sudo ip addr add ${ip}/24 dev enp65s0f0 2>/dev/null
        echo 'OK'
    "
done

echo "Complete！All RDMA drivers are initialized (Device Memory has started)。"
