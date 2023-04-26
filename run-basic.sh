#! /bin/bash
thds=("50")
overlap=("100")
mh=0
node="us"
region="1"
config="examples/basic.conf"
duration=10
for thd in "${thds[@]}"
do
    for ratio in "${overlap[@]}"
    do
        log_dir="$ratio-overlap-$thd-threads"
        out_file="output file: $node-$ratio-overlap-$thd-t.txt"
        cmd="./build/benchmark -clients $thd -config $config -out_dir=$log_dir -txn_profiles true -txns 0 -r $region -wl basic -duration 60 -params ""records=1,writes=1,value_size=600,overlap_ratio=$ratio"" &> $node-$thd-thread-$ratio-overlap.txt"
        echo "rm -rf $log_dir && mkdir $log_dir"
        cd ~/SLOG/data || exit
        rm -rf "$log_dir"
        mkdir "$log_dir"
        echo "$out_file"
        echo "$cmd"
	    echo $thd
        cd ~/SLOG || exit
# ./build/benchmark -clients ${thd} -config "${config}" -out_dir="data/${log_dir}" -txn_profiles true -txns 0 -r $region -wl basic -duration 60 -params value_size=600,nearest=0,overlap_ratio="$ratio" &> $node-"$thd"-thread-"$ratio"-overlap.txt
./build/benchmark -clients ${thd} -config "${config}" -out_dir="data/${log_dir}" -txn_profiles true -txns 0 -r $region -wl basic -duration $duration -params mh=$mh,value_size=250,nearest=0,overlap_ratio="$ratio" &> $node-"$thd"-thread-"$ratio"-overlap.txt

        sleep $duration
    done
done