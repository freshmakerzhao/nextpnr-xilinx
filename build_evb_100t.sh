#编译Debug版本，若要修改Release版本，去CMakelists.txt中修改第218行
rm -rf /home/liguolong/gitlab-eda/nextpnr-xilinx-gtp-dev-dev/nextpnr-xilinx/build
rm -rf /home/liguolong/gitlab-eda/nextpnr-xilinx-gtp-dev/nextpnr-xilinx/xilinx/xc7a100tfgg484-3.bba
rm -rf /home/liguolong/gitlab-eda/nextpnr-xilinx-gtp-dev/nextpnr-xilinx/xilinx/xc7a100tfgg484-3.bin
source ~/lgl_scripts/open_proxy.sh
cd /home/liguolong/gitlab-eda/nextpnr-xilinx-gtp-dev/nextpnr-xilinx
git submodule init
git submodule update --recursive
mkdir build
cd build
cmake -DARCH=xilinx ..
make -j20
python3 ../xilinx/python/bbaexport.py --device xc7a100tfgg484-3 --bba ../xilinx/xc7a100tfgg484-3.bba
../build/bbasm --l ../xilinx/xc7a100tfgg484-3.bba ../xilinx/xc7a100tfgg484-3.bin