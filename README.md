# BPC and Z-cache

## (IMPORTANT) How to setup
This repo points to the official gem5 repository as a submodule. Therefore, our relevant files cannot be inside the `gem5` directory because
they would be tracked by that repository (which we don't). 
Therefore, to build and run simulations, run the `init.sh` script to move our modules from the project into gem5. 
To push anything, it needs to be in the repo's root (or outside gem5)
