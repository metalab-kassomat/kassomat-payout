#!/bin/bash

export LD_LIBRARY_PATH=.

./payoutd -d /dev/kassomat 
# set full directory name, or it will not launch from another folder (like /home/user/kassomat-payout/run.sh)
