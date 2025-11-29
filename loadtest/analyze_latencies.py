#!/usr/bin/env python3
import csv, statistics, sys
file='latencies.csv'
lat=[]
with open(file) as f:
    r=csv.DictReader(f)
    for row in r:
        if row['latency_ms'] and row['latency_ms']!='':
            try:
                lat.append(float(row['latency_ms']))
            except:
                pass
if not lat:
    print("No latencies found")
    sys.exit(0)
print("count:", len(lat))
print("mean: %.2f ms" % (statistics.mean(lat)))
print("median: %.2f ms" % (statistics.median(lat)))
print("p90: %.2f ms" % (sorted(lat)[int(len(lat)*0.9)]))
print("p95: %.2f ms" % (sorted(lat)[int(len(lat)*0.95)]))
print("max: %.2f ms" % max(lat))
