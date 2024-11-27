#!/bin/bash

set -eux
rm -rf logs/*

bin/raft_server -sv 2 --server_id 0 --create 0 >/dev/null &
bin/raft_server -sv 2 --server_id 1 >/dev/null &
bin/raft_server -sv 2 --server_id 2 >/dev/null &
bin/raft_server -sv 2 --server_id 3 >/dev/null &
sleep 15 

bin/raft_client -sv 2 -g 0 --add 1 --server 0 >/dev/null &
sleep 1
bin/raft_client -sv 2 -g 0 --add 2 --server 0 >/dev/null &
sleep 1
bin/raft_client -sv 2 -g 0 -m "message:0" --server 0 >/dev/null &
sleep 1
bin/raft_client -sv 2 -g 0 -m "message:1" --server 0 >/dev/null &
bin/raft_client -sv 2 -g 0 -m "message:2" --server 0 >/dev/null &
sleep 1
bin/raft_client -sv 2 -g 0 -m "message:3" --server 0 >/dev/null &
bin/raft_client -sv 2 -g 0 -m "message:4" --server 0 >/dev/null &
bin/raft_client -sv 2 -g 0 -m "message:5" --server 0 >/dev/null &
sleep 1
bin/raft_client -sv 2 -g 0 --add 3 --server 0 >/dev/null &
sleep 1
bin/raft_client -sv 2 -g 0 -m "message:6" --server 0 >/dev/null &
bin/raft_client -sv 2 -g 0 -m "message:7" --server 3 >/dev/null &
