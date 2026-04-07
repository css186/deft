"""2 Nodes"""

import geni.portal as portal
import geni.rspec.pg as pg
import geni.rspec.emulab as elab

pc = portal.Context()
request = pc.makeRequestRSpec()

NODE_COUNT = 2
HARDWARE_TYPE = 'd6515'
IMAGE = 'urn:publicid:IDN+emulab.net+image+emulab-ops//UBUNTU18-64-STD'

lan = request.LAN('rdma-lan')
lan.bandwidth = 100000000

for i in range(NODE_COUNT):
    name = "node-%d" % (i)
    node = request.RawPC(name)
    node.hardware_type = HARDWARE_TYPE
    node.disk_image = IMAGE
    
    iface = node.addInterface("if1")
    iface.addAddress(pg.IPv4Address("10.10.1.%d" % (i + 1), "255.255.255.0"))
    lan.addInterface(iface)

    if i == 0:
        nfs_bs = node.Blockstore("nfs-data", "/nfs_share")
        nfs_bs.size = "50GB"
        node.addService(pg.Execute(shell="sh", command="sudo chmod 777 /nfs_share"))
        
pc.printRequestRSpec(request)
