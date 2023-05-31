# MCMQHost

## Prerequisites
MCMQHost requires libboost and protobuf compiler to build. On Ubuntu, these can be installed with
```sh
sudo apt-get install libboost-all-dev protobuf-compiler
```

## Build & Run
To build MCMQHost, run 
```sh
mkdir build
cd build
conan install .. --build=missing
cmake .. -DUSE_INTERPROCESS=ON
make -j
```

This will build all applications under `bin`.

### Loading data
Datasets can be loaded onto the device with:
```sh
bin/loaddata -b vfio -g <IOMMU group> -d <PCIe device slot> -c ../ssdconfig.yaml -L <device library> -f <dataset file> [-n <namespace ID>]
```

IOMMU group and PCIe device slot of the StorPU SSD can be found with `lspci -vv`. `<device library>` is the path to the `libtest.so` compiled from the device library code (See [](../storpu/README.org)). `<dataset file>` is the path to the dataset to be loaded. This command loads the dataset into a newly-created namespace. Optionally, `-n` can be used to specify the namespace to load the data.

### Synthetic workloads
To run the synthetic applications, invoke either
```sh
bin/synthetic-mcmq -b vfio -g <IOMMU group> -d <PCIe device slot> -c ../ssdconfig.yaml -w <workload name> [-b <bits>] -s <data size>
```
for pure host execution or 
```sh
bin/synthetic-storpu -b vfio -g <IOMMU group> -d <PCIe device slot> -c ../ssdconfig.yaml -L <device library> -w <workload name> [-b <bits>] -s <data size>
```
for StorPU execution. `<data size>` specifies the size of the input dataset in bytes. `<workload name>` can be `stats`, `knn` or `grep`. For `stats`, `-b` can be used to specify the bits of the input elements (32/64). 
