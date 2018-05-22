#!/bin/sh

MEMCACHED_DIR=/home/zablotch/memcached
MEMCACHED_CLHT_DIR=/home/zablotch/memcached-clht
NV_MEMCACHED_DIR=/home/zablotch/nv-memcached
MEMTIER_DIR=/home/zablotch/memtier_benchmark
RESULTS_DIR=$NV_MEMCACHED_DIR/results
TEST_RUN_DIR=$RESULTS_DIR/`date '+%Y-%m-%d_%H:%M:%d'`
mkdir $TEST_RUN_DIR

MEMCACHED_SOCKET=/tmp/memcached
MEMCACHED_CLHT_SOCKET=/tmp/memcached-clht
NV_MEMCACHED_SOCKET=/tmp/nv_memcached

RATIOS="0:1 1:10 1:4 1:1" #ratio is Set:Get
INITIAL_SIZES="1000 10000 100000 1000000"


########################
### HELPER FUNCTIONS ###
########################

# arguments:
# $1: socket file
# $2: system parent directory
# $3: system name
RunForSystem() { 

for init in $INITIAL_SIZES;
do
    range=`echo "$init * 2" | bc`;
    for r in $RATIOS;
    do
        # create the results file
        RESULTS_FILE=$TEST_RUN_DIR/$3-$init-$r
        touch $RESULTS_FILE

        # clean the socket file 
        rm -rf $1

        #launch system
        cd $2
        ./memcached -s $1 2>&1 | grep "Recovery" >> $RESULTS_FILE &
        sleep 2

        # warm up the cache
        cd $MEMTIER_DIR
        /usr/bin/time -f %e -o /tmp/warmup_time ./memtier_benchmark --unix-socket=$1 --protocol=memcache_text --run-count=1 --threads=1 --ratio=1:0 --key-pattern=S:S --requests=$init --key-maximum=$range

        # run the actual experiments
        ./memtier_benchmark --unix-socket=$1 --protocol=memcache_text --run-count=5 --ratio=$r --key-pattern=R:R --test-time=5 --key-maximum=$range 2>&1 | grep -A1000 "AGGREGATED AVERAGE RESULTS" >> $RESULTS_FILE

        echo "WARM-UP TIME WAS: " >> $RESULTS_FILE
        cat /tmp/warmup_time >> $RESULTS_FILE

        # kill memcached
        pkill -SIGINT memcached
    done;
done;


}

##########################
### SCRIPT STARTS HERE ###
##########################


# compile everything
cd $MEMCACHED_DIR
make -s

cd $MEMCACHED_CLHT_DIR
make -s

cd $NV_MEMCACHED_DIR
make -s

cd $MEMTIER_DIR
make -s


### MEMCACHED
RunForSystem $MEMCACHED_SOCKET $MEMCACHED_DIR "memcached"

### MEMCACHED_CLHT
RunForSystem $MEMCACHED_CLHT_SOCKET $MEMCACHED_CLHT_DIR "memcached-clht"

### NV_MEMCACHED
RunForSystem $NV_MEMCACHED_SOCKET $NV_MEMCACHED_DIR "nv_memcached"
