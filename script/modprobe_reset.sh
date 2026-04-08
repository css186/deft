declare -A nodes=(
    [amd025]=10.10.1.1
    [amd023]=10.10.1.2
    [amd018]=10.10.1.3
    [amd028]=10.10.1.4
    [amd013]=10.10.1.5
    [amd014]=10.10.1.6
    [amd005]=10.10.1.7
    [amd008]=10.10.1.8
    [amd009]=10.10.1.9
    [amd015]=10.10.1.10
    [amd010]=10.10.1.11
    [amd019]=10.10.1.12
)

for host in "${!nodes[@]}"; do
    ip=${nodes[$host]}
    echo "=== $host -> $ip ==="
    sudo ssh -o StrictHostKeyChecking=no ${host}.utah.cloudlab.us "
        # Load driver now
        modprobe mlx5_core
        # Make driver load permanent
        echo mlx5_core >> /etc/modules
        # Configure interface permanently
        cat >> /etc/network/interfaces <<EOF

auto enp65s0f0
iface enp65s0f0 inet static
    address ${ip}
    netmask 255.255.255.0
EOF
        # Bring up now
        ip link set enp65s0f0 up
        ip addr add ${ip}/24 dev enp65s0f0 2>/dev/null
        echo OK
    "
done
