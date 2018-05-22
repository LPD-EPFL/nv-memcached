#!/bin/sh

# USAGE: parse.sh [RESULT DIR]
# IMPORTANT: make sure the ratios and sizes below are the same as in run.sh
DIR=$1
 
RATIOS="0:1 1:10 1:4 1:1" #ratio is Set:Get
INITIAL_SIZES="1000 10000 100000 1000000"

cd $DIR
touch aggregates


PrintForSystem() {
echo $1 >> aggregates;
printf "%-16s" " " >> aggregates;
for i in $INITIAL_SIZES;
do
    printf "%-16s" $i >> aggregates
done;
printf "\n\n" >> aggregates

for r in $RATIOS;
do
    printf "%-16s" $r >> aggregates;
    for i in $INITIAL_SIZES;
    do
        file=$1-$i-$r
        TPUT=`grep "Totals" $file | tr -s ' ' | cut -d ' ' -f 2`
        printf "%-16.2f" $TPUT >> aggregates  
    done;
    printf "\n" >> aggregates
done;
printf "\n\n\n" >> aggregates

printf "%-16s" " " >> aggregates;
for i in $INITIAL_SIZES;
do
    printf "%-16s" $i >> aggregates
done;
printf "\n\n" >> aggregates

for r in $RATIOS;
do
    printf "%-16s" $r >> aggregates;
    for i in $INITIAL_SIZES;
    do
        file=$1-$i-$r
        TPUT=`grep "Totals" $file | tr -s ' ' | cut -d ' ' -f 2`
        
        if [ "$1" = nv_memcached ]; then
            RECOVERY_TIME=`grep "Recovery" $file | grep -o -E '[0-9]+'`;
            printf "%-16d" $RECOVERY_TIME >> aggregates; 

        else
            WARMUP_TIME=`grep -A1 "WARM-UP" $file | grep -o -E '[0-9]+\.[0-9]+'`;
            printf "%-16.2f" $WARMUP_TIME >> aggregates; 
        fi
            
    done;
    printf "\n" >> aggregates
done;
printf "\n\n\n" >> aggregates
}


PrintForSystem memcached
PrintForSystem memcached-clht
PrintForSystem nv_memcached