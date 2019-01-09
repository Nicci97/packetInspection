make clean
make
clear
#sudo ./build/ndpiReader.dpdk -c 1 --vdev=net_pcap0,iface=eno1 -- -v 1 -w output.txt
./ndpiReader -i ../tests/pcap/ookla.pcap
