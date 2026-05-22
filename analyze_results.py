import csv
from pathlib import Path
import re

RAW_DIR = Path('results/raw')
SUMMARY_CSV = Path('results/summary.csv')
FINAL_TXT = Path('results/final_summary.txt')

FIO_PATTERN = re.compile(r'\b([0-9]+\.?[0-9]*)\s*(MiB|GiB|kB|KB|B)/s|bw=([0-9.]+)([KMG]i?B)/s|lat.*=\s*([0-9.]+)\s*us|IOPS=([0-9.]+)', re.IGNORECASE)

cnt=0

def parse_csv(path: Path):
    rows = []
    with path.open() as f:
        reader = csv.DictReader(f)
        for r in reader:
            try:
                rows.append({
                    'timestamp': float(r.get('timestamp', 0.0)),
                    'util': float(r.get('util', 0.0)),
                    'ipc': float(r.get('ipc', 0.0)),
                    'iowait': float(r.get('iowait', 0.0)),
                    'mpki': float(r.get('mpki', 0.0)),
                    'freq': float(r.get('freq', 0.0)),
                    'power': float(r.get('power', 0.0)),
                    'action': r.get('action', ''),
                    'source': r.get('source', ''),
                    'rl_enabled': r.get('rl_enabled', 'false').strip().upper() in ('1', 'TRUE', 'RL')
                })
            except ValueError:
                continue
    return rows


def summarize_run(path: Path):
    with path.open() as f:
        reader = csv.DictReader(f)
        fieldnames = reader.fieldnames or []
        if 'avg_power_w' in fieldnames and 'duration_s' in fieldnames:
            row = next(reader, None)
            if not row:
                return None
            return {
                'run_name': path.stem.replace('.summary', ''),
                'mode': row.get('mode', 'BASELINE'),
                'csv_file': str(path),
                'duration_s': round(float(row.get('duration_s', 0.0)), 1),
                'avg_power_w': round(float(row.get('avg_power_w', 0.0)), 3),
                'avg_ipc': round(float(row.get('avg_ipc', 0.0)), 3),
                'avg_util': round(float(row.get('avg_util', 0.0)), 3),
                'actions_per_minute': round(float(row.get('actions_per_minute', 0.0)), 2),
                'throughput_mib_s': round(float(row.get('throughput_mib_s', -1.0)), 3),
                'latency_us': round(float(row.get('latency_us', 0.0)), 3),
                'iops': round(float(row.get('iops', 0.0)), 3)
            }

    rows = parse_csv(path)
    if not rows:
        return None
    start = rows[0]['timestamp']
    end = rows[-1]['timestamp']
    duration = max(0.0, end - start)
    avg_power = sum(r['power'] for r in rows) / len(rows)
    avg_ipc = sum(r['ipc'] for r in rows) / len(rows)
    avg_util = sum(r['util'] for r in rows) / len(rows)
    avg_util_n=avg_util/100.0
    actions = [r['action'] for r in rows if r['action']]
    action_counts = len(actions)
    actions_per_minute = action_counts / (duration / 60.0) if duration > 0 else 0.0
    avg_freq = sum(r['freq'] for r in rows) / len(rows)
    avg_freq/=3000.0
    avg_iowait = sum(r['iowait'] for r in rows) / len(rows)
    avg_iowait/=100.0
    throughput = avg_ipc*avg_util_n*avg_freq
    latency = (1 / (throughput +1e-6)) if throughput > 0 else float('inf')
    iopc=throughput*(1-avg_iowait)
    mode = 'RL' if any(r['rl_enabled'] for r in rows) else 'BASELINE'
    return {
        'run_name': path.stem,
        'mode': mode,
        'csv_file': str(path),
        'duration_s': round(duration, 1),
        'avg_power_w': round(avg_power, 3),
        'avg_ipc': round(avg_ipc, 3),
        'avg_util': round(avg_util, 3),
        'actions_per_minute': round(actions_per_minute, 2),
        'throughput_mib_s': round(throughput, 3),
        'latency_us': round(latency, 3),
        'iops': round(iopc, 3)
    }


def compare_runs(rows):
    baseline = next((r for r in rows if r['mode'] == 'BASELINE'), None)
    rl = next((r for r in rows if r['mode'] == 'RL'), None)
    lines = []
    if not baseline or not rl:
        lines.append('Need both BASELINE and RL runs to compare results.')
        return lines
    def pct_change(old, new):
        try:
            return (new - old) / old * 100.0 if old != 0 else float('inf')
        except Exception:
            return float('nan')
    power_delta = pct_change(baseline['avg_power_w'], rl['avg_power_w'])
    ipc_delta = pct_change(baseline['avg_ipc'], rl['avg_ipc'])
    util_delta = pct_change(baseline['avg_util'], rl['avg_util'])
    throughput_delta = pct_change(baseline['throughput_mib_s'], rl['throughput_mib_s']) if baseline['throughput_mib_s'] and rl['throughput_mib_s'] else float('nan')
    iops_delta = pct_change(baseline['iops'], rl['iops']) if baseline['iops'] and rl['iops'] else float('nan')
    latency_delta = pct_change(baseline['latency_us'], rl['latency_us']) if baseline['latency_us'] and rl['latency_us'] else float('nan')
    lines.append(f"Baseline average power: {baseline['avg_power_w']:.3f} W, RL average power: {rl['avg_power_w']:.3f} W, ({power_delta:+.1f}%).")
    lines.append(f"Baseline average IPC: {baseline['avg_ipc']:.3f}, RL average IPC: {rl['avg_ipc']:.3f}, ({ipc_delta:+.1f}%).")
    lines.append(f"Baseline throughput: {baseline['throughput_mib_s']} MiB/s, RL throughput: {rl['throughput_mib_s']} MiB/s ({throughput_delta:+.1f}%).")
    lines.append(f"Baseline latency: {baseline['latency_us']} us, RL latency: {rl['latency_us']} us ({latency_delta:+.1f}%).")
    lines.append(f"Baseline IOPS: {baseline['iops']}, RL IOPS: {rl['iops']} ({iops_delta:+.1f}%).")
    global cnt
    if(power_delta<0):
        cnt+=1
    if(ipc_delta>0):
        cnt+=1
    if(throughput_delta>0):
        cnt+=1
    if(latency_delta<0):
        cnt+=1
    if(iops_delta>0):
        cnt+=1

    return lines


def main():
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    runs = []
    paths = sorted(RAW_DIR.glob('*.csv'))
    paths = [p for p in paths if not p.name.endswith('.summary.csv')]

    for path in paths:
        summary = summarize_run(path)
        if summary:
            runs.append(summary)
    if not runs:
        print('No run summary or raw CSV runs found in results/raw.')
        return

    SUMMARY_CSV.parent.mkdir(parents=True, exist_ok=True)
    with SUMMARY_CSV.open('w', newline='') as f:
        fieldnames = ['run_name', 'mode', 'csv_file', 'duration_s', 'avg_power_w', 'avg_ipc', 'avg_util', 'actions_per_minute', 'throughput_mib_s', 'latency_us', 'iops']
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in runs:
            writer.writerow(row)

    final_lines = compare_runs(runs)
    FINAL_TXT.parent.mkdir(parents=True, exist_ok=True)
    FINAL_TXT.write_text('\n'.join(final_lines) + '\n')
    print(f'Summary written to {SUMMARY_CSV} and {FINAL_TXT}')
    print(cnt,end='')


if __name__ == '__main__':
    main()
