from genericpath import isdir
import re
import matplotlib.pyplot as plt
import numpy as np
import os

file_path_list = [
    "tpcc_mh_0_t64_c1",
    "tpcc_mh_0_t64_c2",
    "tpcc_mh_0_t64_c4",
    "tpcc_mh_0_t64_c8",
    "tpcc_mh_0_t64_c16",
    "tpcc_mh_0_t64_c32",
    "tpcc_mh_0_t64_c64",
    "tpcc_mh_0_t64_c128",
]

if __name__ == "__main__":
    x, avg_tps = [], []
    clients = []
    for path in [os.path.join(os.getcwd(), "data",i) for i in file_path_list]:
        print(path)
        if not re.match("(.)*_c\d+", path):
            print("continue")
            continue
        f = open(os.path.join(os.getcwd(), "data", path, "log_mh_0"))
        lines = f.readlines()
        for line in lines:
            if re.match("(.)*Num clients:\s+\d+", line):
                flag = re.match("Num clients:\s+\d+", line)
                clients.append(int(flag.group(0).split(' ')[2]))
                # print(int(flag.group(0).split(' ')[2]))
            elif re.match("(.)*Avg. TPS:\s+\d+", line):
                flag = re.match("(.)*Avg. TPS:\s+\d+", line)
                avg_tps.append(int(flag.group(0).split(' ')[2]))
                # print(int(flag.group(0).split(' ')[2]))
        f.close()
    width = 0.35
    x_tick = np.arange(len(clients)) + 1
    plt.bar(x_tick, np.array(avg_tps), width=width, label='TPS')
    plt.xticks(x_tick, clients)
    plt.xlabel("clients num")
    plt.ylabel("TPS")
    plt.title("SLOG TPC-C 50% Multi-home")
    plt.legend()
    plt.savefig("test")

    # plt.plot(x, qps, label=path + " avg qps: " + str("{:.2f}".format(numpy.mean(qps))))
    # plt.xlabel("time (s)")
    # plt.ylabel("qps")
    # plt.legend()
    # plt.savefig("qps")

    # plt.plot(x, tps, label=path + " avg tps: " + str("{:.2f}".format(numpy.mean(tps))))
    # plt.xlabel("time (s)")
    # plt.ylabel("tps")
    # plt.legend()
    # plt.savefig("tps")
