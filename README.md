# srsRAN

[![Build Status](https://github.com/srsran/srsRAN_4G/actions/workflows/ccpp.yml/badge.svg?branch=master)](https://github.com/srsran/srsRAN_4G/actions/workflows/ccpp.yml)
[![CodeQL](https://github.com/srsran/srsRAN_4G/actions/workflows/codeql.yml/badge.svg?branch=master)](https://github.com/srsran/srsRAN_4G/actions/workflows/codeql.yml)
[![Coverity](https://scan.coverity.com/projects/28268/badge.svg)](https://scan.coverity.com/projects/srsran_4g_agpl)

srsRAN is an open source 4G software radio suite developed by [SRS](http://www.srs.io). For 5G RAN, see our new O-RAN CU/DU solution - [srsRAN Project](https://www.github.com/srsran/srsran_project).

See the [srsRAN 4G project pages](https://www.srsran.com) for information, guides and project news.

The srsRAN suite includes:
  * srsUE - a full-stack SDR 4G UE application with prototype 5G features
  * srsENB - a full-stack SDR 4G eNodeB application
  * srsEPC - a light-weight 4G core network implementation with MME, HSS and S/P-GW

For application features, build instructions and user guides see the [srsRAN 4G documentation](https://docs.srsran.com/projects/4g/).

For license details, see LICENSE file.

# Changes

This version of srsRAN is modified to improve output for analysis (printing the UE capabilities) and has been modified to disable authentication. In theory this should mean every phone can connect to it, if the phone doesn't require a two-way verification. 

# Installation
## Install dependencies
```bash
sudo apt-get install build-essential cmake libfftw3-dev libmbedtls-dev libboost-program-options-dev libconfig++-dev libsctp-dev
```
### SoapySDR
For the BladeRF SoapySDR must also be installed: [https://github.com/pothosware/SoapySDR]

## Clone and build srsRAN
```bash
git clone https://github.com/andyvpoeijer188/srsRAN_4G.git
cd srsRAN_4G
mkdir build
cd build
cmake ../
make
make test
sudo make install
srsran_install_configs.sh user
```

# Usage
srsEPC and srsENB must be ran simultaniously on the host machine. This can be done using the following commands in seperate terminals
```bash
sudo srsepc
sudo srsenb
```
srsEPC will print the UE information to the terminal. 
