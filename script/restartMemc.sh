#!/bin/bash

addr=$(head -1 ../memcached.conf)
port=$(awk 'NR==2{print}' ../memcached.conf)

# kill old me
ssh ${addr} "sudo cat /tmp/memcached.pid | xargs sudo kill" || true

sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"

# launch memcached
ssh ${addr} "sudo memcached -u root -l ${addr} -p  ${port} -c 10000 -d -P /tmp/memcached.pid"
sleep 1

# init 
echo -e "set ServerNum 0 0 1\r\n0\r\nquit\r" | nc ${addr} ${port}
echo -e "set ClientNum 0 0 1\r\n0\r\nquit\r" | nc ${addr} ${port}
