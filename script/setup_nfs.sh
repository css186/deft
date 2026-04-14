#!/bin/bash
# 讓其他 5 台 Client 節點建立永久的 NFS 掛載設定

clients=(mn-0 mn-1 cn-1 cn-2 cn-3)

for host in "${clients[@]}"; do
    echo "=== 正在設定 $host 的永久掛載 ==="
    
    # 透過預設的 SSH 免密碼通道連線
    ssh -o StrictHostKeyChecking=no $host "
        # 1. 確保掛載點存在
        sudo mkdir -p /nfs_share
        
        # 2. 檢查是否已經寫入過，避免重複寫入造成開機錯誤
        if ! grep -q '10.10.1.1:/nfs_share' /etc/fstab; then
            echo '10.10.1.1:/nfs_share /nfs_share nfs defaults,_netdev 0 0' | sudo tee -a /etc/fstab
            echo '[SUCCESS] 成功寫入 /etc/fstab'
        else
            echo '[SKIP] /etc/fstab 已經有紀錄了'
        fi
        
        # 3. 觸發系統重新讀取並立即掛載
        sudo mount -a
    "
done

echo "================================================="
echo "所有節點配置完成！"
echo "================================================="
