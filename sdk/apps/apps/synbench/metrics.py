# metrics.py: Разобрать счётчики ресурсов и сохранить источники измерений
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux ABI
# Подсистема: хостовая обработка результатов Linux и SynapseOS
# Первое изменение: 19.10.2026, последнее изменение: 20.10.2026
# Доступ: общедоступный

import os
import re
import time
from pathlib import Path


SYSTEM_SOURCES = (
    '/proc/meminfo', '/proc/stat', '/proc/vmstat', '/proc/diskstats',
    '/proc/net/dev', '/proc/net/snmp', '/proc/net/netstat', '/proc/loadavg',
    '/proc/pressure/cpu', '/proc/pressure/memory', '/proc/pressure/io',
    '/proc/schedstat', '/proc/sys/kernel/sched_schedstats', '/proc/interrupts', '/proc/softirqs',
    '/sys/fs/cgroup/cpu.stat', '/sys/fs/cgroup/cpu.max',
    '/sys/fs/cgroup/memory.current', '/sys/fs/cgroup/memory.max',
    '/sys/fs/cgroup/memory.events', '/sys/fs/cgroup/io.stat',
)


def read_source(path):
    """Сохранить данные источника вместе с ошибками и признаком обрезки."""
    try:
        with open(path, 'rb') as stream:
            data = stream.read(65537)
        return {'path': str(path), 'raw': data[:65536].decode('latin1'),
                'errno': 0, 'truncated': len(data) > 65536}
    except OSError as error:
        return {'path': str(path), 'raw': None, 'errno': error.errno, 'truncated': False}


def host_sample(pids, thread_pids=None):
    """Снять счётчики процессов и отдельные сырые источники явно указанных потоков."""
    start = time.monotonic_ns()
    if thread_pids is not None and not isinstance(thread_pids, dict):
        raise ValueError('host_threads must map labels to pid/tid objects')
    thread_paths = {}
    for label, identity in (thread_pids or {}).items():
        if (not isinstance(identity, dict) or set(identity) != {'pid', 'tid'} or
                any(type(identity[key]) is not int or identity[key] <= 0 or identity[key] > 2147483647
                    for key in ('pid', 'tid'))):
            raise ValueError('host_threads entries require positive integer pid and tid')
        thread_paths[label] = f"/proc/{identity['pid']}/task/{identity['tid']}"
    sources = {path: read_source(path) for path in SYSTEM_SOURCES}
    # Обнаруживать расположение датчиков и cgroup, не предполагать фиксированный номер
    paths = list(Path('/sys/class/thermal').glob('thermal_zone*/temp'))
    paths += list(Path('/sys/devices/system/cpu').glob('cpu*/cpufreq/scaling_cur_freq'))
    paths += list(Path('/sys/devices/system/cpu').glob('cpu*/thermal_throttle/*throttle_count'))
    for path in paths:
        sources[str(path)] = read_source(path)
    processes = {}
    for label, pid in pids.items():
        processes[label] = {
            name: read_source(f'/proc/{pid}/{name}')
            for name in ('stat', 'status', 'io', 'smaps_rollup', 'schedstat', 'cgroup')
        }
    # TID может исчезнуть между чтениями, read_source сохраняет ошибку каждого источника
    threads = {
        label: {name: read_source(f'{path}/{name}') for name in ('stat', 'status', 'schedstat')}
        for label, path in thread_paths.items()
    }
    return {'monotonic_ns': start, 'wall_time_ns': time.time_ns(), 'sources': sources,
            'processes': processes, 'threads': threads, 'collection_ns': time.monotonic_ns() - start}


def raw(sources, key):
    """Принять только полностью прочитанный источник."""
    item = sources.get(key, {})
    if item.get('errno') or item.get('truncated'):
        return None
    return item.get('raw')


def key_values(text):
    """Преобразовать числовые поля Linux с сохранением единиц в байтах."""
    values = {}
    for line in (text or '').splitlines():
        match = re.fullmatch(r'([A-Za-z_()]+):?\s+(\d+)(?:\s+(kB))?', line.strip())
        if match:
            key, value, unit = match.groups()
            values[key] = int(value) * (1024 if unit else 1)
    return values


def process_metrics(sources, ticks, synapse=False, prefix='process/'):
    """Получить память и счётчики процесса без фиктивного разделения CPU в SynapseOS."""
    values = key_values(raw(sources, prefix + 'status'))
    result = {name + '_bytes': values.get(name) for name in (
        'VmRSS', 'VmSize', 'VmPeak', 'VmHWM', 'VmSwap', 'RssAnon', 'RssFile', 'RssShmem')}
    result['threads'] = values.get('Threads')
    result['migrations'] = values.get('Migrations') if synapse else None
    for source_key, key in (('UserPageFaults', 'user_page_faults'),
                            ('FileDemandPages', 'file_demand_pages'),
                            ('AnonDemandPages', 'anon_demand_pages')):
        result[key] = values.get(source_key) if synapse else None
    if not synapse:
        for key in ('voluntary_ctxt_switches', 'nonvoluntary_ctxt_switches'):
            result[key] = values.get(key)
        result.update({'io_' + key: value for key, value in key_values(raw(sources, prefix + 'io')).items()})
        result.update({'memory_' + key: value
                       for key, value in key_values(raw(sources, prefix + 'smaps_rollup')).items()})
    stat = raw(sources, prefix + 'stat')
    if stat and ticks > 0:
        # Имя процесса может содержать пробелы и скобки
        fields = stat[stat.rfind(')') + 2:].split()
        try:
            user, system = int(fields[11]), int(fields[12])
            result['start_time_ticks'] = int(fields[19])
            result['cpu_seconds'] = (user + system) / ticks
            if not synapse:
                result['cpu_user_seconds'] = user / ticks
                result['cpu_system_seconds'] = system / ticks
                result['minor_faults'] = int(fields[7])
                result['major_faults'] = int(fields[9])
        except (ValueError, IndexError):
            result['cpu_seconds'] = None
    return result


def cpu_counters(sources, synapse):
    """Прочитать реальные счётчики нагрузки по каждому CPU."""
    result = {}
    if synapse:
        text = raw(sources, '/proc/performance') or ''
        if not text.startswith('version 1\n'):
            return result
        for line in text.splitlines():
            fields = line.split()
            if len(fields) == 6 and fields[0] == 'cpu':
                try:
                    cpu, busy, idle, switches, interrupts = map(int, fields[1:])
                    result[str(cpu)] = {'busy': busy, 'total': busy + idle,
                                        'switches': switches, 'interrupts': interrupts}
                except ValueError:
                    continue
    else:
        for line in (raw(sources, '/proc/stat') or '').splitlines():
            fields = line.split()
            if fields and re.fullmatch(r'cpu\d*', fields[0]):
                try:
                    # guest и guest_nice уже включены в user и nice
                    counts = list(map(int, fields[1:9]))
                    total = sum(counts)
                    result[fields[0]] = {'busy': total - counts[3] - counts[4], 'total': total}
                except (ValueError, IndexError):
                    continue
    return result


def normalise(sources, synapse=False):
    """Извлечь доступные системные метрики с единицами в именах полей."""
    result = {}
    for key, value in key_values(raw(sources, '/proc/meminfo')).items():
        if synapse and key in ('Cached', 'Buffers', 'SwapFree', 'SwapTotal'):
            continue
        result['memory/' + key + '_bytes'] = value
    for line in (raw(sources, '/proc/vmstat') or '').splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[1].isdigit():
            result['vm/' + fields[0]] = int(fields[1])
    for line in (raw(sources, '/proc/stat') or '').splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[1].isdigit():
            result['scheduler/' + fields[0]] = int(fields[1])
    for line in (raw(sources, '/proc/net/dev') or '').splitlines():
        if ':' not in line:
            continue
        interface, data = line.split(':', 1)
        fields = data.split()
        if len(fields) == 16 and all(item.isdigit() for item in fields):
            names = ('rx_bytes', 'rx_packets', 'rx_errors', 'rx_drops', 'rx_fifo', 'rx_frame',
                     'rx_compressed', 'rx_multicast', 'tx_bytes', 'tx_packets', 'tx_errors',
                     'tx_drops', 'tx_fifo', 'tx_collisions', 'tx_carrier', 'tx_compressed')
            for name, value in zip(names, fields):
                result[f'network/{interface.strip()}/{name}'] = int(value)
    for line in (raw(sources, '/proc/diskstats') or '').splitlines():
        fields = line.split()
        if len(fields) < 14 or not all(item.isdigit() for item in fields[3:]):
            continue
        names = ('read_ios', 'read_merged', 'read_sectors', 'read_ms', 'write_ios', 'write_merged',
                 'write_sectors', 'write_ms', 'in_flight', 'busy_ms', 'weighted_ms',
                 'discard_ios', 'discard_merged', 'discard_sectors', 'discard_ms', 'flush_ios', 'flush_ms')
        for name, value in zip(names, fields[3:]):
            result[f'disk/{fields[2]}/{name}'] = int(value)
    for path in ('/proc/net/snmp', '/proc/net/netstat'):
        lines = (raw(sources, path) or '').splitlines()
        for index in range(0, len(lines) - 1, 2):
            names, values = lines[index].split(), lines[index + 1].split()
            if not names or len(names) != len(values) or names[0] != values[0]:
                continue
            for name, value in zip(names[1:], values[1:]):
                if value.lstrip('-').isdigit():
                    result[f'protocol/{names[0].rstrip(":")}/{name}'] = int(value)
    for source_name, source in sources.items():
        if source_name.startswith('/proc/pressure/'):
            for line in (raw(sources, source_name) or '').splitlines():
                parts = line.split()
                for field in parts[1:]:
                    if '=' in field:
                        name, value = field.split('=', 1)
                        try:
                            result[f'pressure/{source_name.rsplit("/", 1)[-1]}/{parts[0]}/{name}'] = float(value)
                        except ValueError:
                            continue
        if source_name.endswith(('/temp', '/scaling_cur_freq', '_throttle_count')):
            value = raw(sources, source_name)
            if value and value.strip().lstrip('-').isdigit():
                result[source_name] = int(value)
        if source_name.startswith('/sys/fs/cgroup/'):
            result.update({source_name + '/' + key: value for key, value in key_values(source.get('raw')).items()})
    return result


def numeric_summary(series):
    """Сохранить диапазон и среднее наблюдений без трактовки счётчиков как скоростей."""
    values = {}
    for sample in series:
        for name, value in sample.items():
            if value is not None:
                values.setdefault(name, []).append(value)
    return {name: {'first': items[0], 'last': items[-1], 'min': min(items), 'max': max(items),
                   'sample_mean': sum(items) / len(items), 'observations': len(items)}
            for name, items in values.items()}


def normalised_events(events):
    """Сохранить доступные измерения каждого снимка для графиков и последующего анализа."""
    metadata = next((event for event in events if event.get('event') == 'metadata'), {})
    synapse = metadata.get('os') == 'SynapseOS'
    result = []
    previous_cpu = {}
    previous_process = None
    previous_time = None
    for event in events:
        if event.get('event') != 'sample':
            continue
        values = normalise(event['sources'], synapse)
        values.update({'process/' + name: value for name, value in
                       process_metrics(event['sources'], metadata.get('clock_ticks', 0), synapse).items()})
        current_process = values.get('process/cpu_seconds')
        if current_process is not None and previous_process is not None and previous_time is not None:
            duration = (event['monotonic_ns'] - previous_time) / 1e9
            cpu_time = current_process - previous_process
            values['process/cpu_percent_one_core'] = (
                cpu_time / duration * 100 if duration > 0 and cpu_time >= 0 else None)
        previous_process = current_process
        previous_time = event['monotonic_ns']
        cpus = cpu_counters(event['sources'], synapse)
        for core in cpus.keys() & previous_cpu.keys():
            total = cpus[core]['total'] - previous_cpu[core]['total']
            busy = cpus[core]['busy'] - previous_cpu[core]['busy']
            values[f'cpu/{core}/busy_percent'] = 100 * busy / total if total > 0 and 0 <= busy <= total else None
        previous_cpu = cpus
        result.append({'monotonic_ns': event['monotonic_ns'], 'phase': event['phase'], 'values': values})
    return result


def summarise_host(samples):
    """Отдельно свести процессы QEMU, управляющей программы и сервера хоста."""
    result = {'system': numeric_summary([normalise(item['sources']) for item in samples]), 'processes': {}}
    ticks = os.sysconf('SC_CLK_TCK')
    labels = set().union(*(sample['processes'].keys() for sample in samples))
    for label in labels:
        series = [process_metrics(item['processes'][label], ticks, prefix='')
                  for item in samples if label in item['processes']]
        summary = numeric_summary(series)
        result['processes'][label] = summary
        if 'start_time_ticks' in summary and summary['start_time_ticks']['min'] != summary['start_time_ticks']['max']:
            result['processes'][label] = {'error': 'PID was reused, process measurements are not comparable'}
            continue
        if samples and len(samples) >= 2 and 'cpu_seconds' in summary:
            elapsed = (samples[-1]['monotonic_ns'] - samples[0]['monotonic_ns']) / 1e9
            cpu = summary['cpu_seconds']['last'] - summary['cpu_seconds']['first']
            summary['cpu_percent_one_core'] = cpu / elapsed * 100 if elapsed > 0 and cpu >= 0 else None
    return result


def summarise(events):
    """Свести временной ряд, сохраняя недоступные показатели как null."""
    metadata = next((event for event in events if event.get('event') == 'metadata'), {})
    synapse = metadata.get('os') == 'SynapseOS'
    ticks = metadata.get('clock_ticks', 0)
    samples = [event for event in events if event.get('event') == 'sample']
    running = [event for event in samples if event.get('phase') == 'running']
    finish = next((event for event in reversed(events) if event.get('event') == 'finish'), None)
    process = [process_metrics(event['sources'], ticks, synapse) for event in running]
    result = {'metadata': metadata, 'finish': finish, 'samples': len(running),
              'process_sampled_peak_rss_bytes': None, 'process_sampled_peak_virtual_bytes': None,
              'system_memory': {}, 'cpu_by_core': {}, 'source_errors': {},
              'sampling_collection_peak_ns': max((event['collection_ns'] for event in samples), default=None),
              'missing_reason': 'Source absent, unreadable, truncated, or not implemented by this OS',
              'sampled_peaks_are_lower_bounds': True}
    result['observed_metrics'] = numeric_summary([
        item['values'] for item in normalised_events(events) if item['phase'] == 'running'])
    for source_key, output_key in (
        ('VmRSS_bytes', 'process_sampled_peak_rss_bytes'),
        ('VmSize_bytes', 'process_sampled_peak_virtual_bytes'),
        ('migrations', 'process_sampled_migrations_lower_bound'),
        ('user_page_faults', 'process_sampled_user_page_faults_lower_bound'),
        ('file_demand_pages', 'process_sampled_file_demand_pages_lower_bound'),
        ('anon_demand_pages', 'process_sampled_anon_demand_pages_lower_bound'),
    ):
        values = [item[source_key] for item in process if item.get(source_key) is not None]
        result[output_key] = max(values, default=None)
    for key in ('MemTotal', 'MemFree', 'MemAvailable', 'Cached', 'Buffers', 'SwapFree', 'SwapTotal', 'Slab'):
        if synapse and key in ('Cached', 'Buffers', 'SwapFree', 'SwapTotal'):
            result['system_memory'][key] = {'min_bytes': None, 'max_bytes': None,
                                            'reason': 'Compatibility constant in current SynapseOS procfs'}
            continue
        values = [key_values(raw(event['sources'], '/proc/meminfo')).get(key) for event in running]
        values = [value for value in values if value is not None]
        result['system_memory'][key] = {'min_bytes': min(values, default=None),
                                        'max_bytes': max(values, default=None)}
    if len(samples) >= 2:
        first = cpu_counters(samples[0]['sources'], synapse)
        last = cpu_counters(samples[-1]['sources'], synapse)
        for core in first.keys() & last.keys():
            total = last[core]['total'] - first[core]['total']
            busy = last[core]['busy'] - first[core]['busy']
            result['cpu_by_core'][core] = {
                'busy_percent': 100 * busy / total if total > 0 and 0 <= busy <= total else None}
            for name in ('switches', 'interrupts'):
                if name in first[core] and last[core][name] >= first[core][name]:
                    result['cpu_by_core'][core][name] = last[core][name] - first[core][name]
    for event in samples:
        for name, item in event['sources'].items():
            if item.get('errno') or item.get('truncated'):
                result['source_errors'][name] = {'errno': item.get('errno'), 'truncated': item.get('truncated')}
    # ru_maxrss является максимумом за всё время только на проверенном Linux
    if finish and finish.get('rusage'):
        usage = finish['rusage']
        result['process_lifetime_peak_rss_bytes'] = usage['max_rss_kib'] * 1024
        result['process_cpu_seconds'] = (usage['user_us'] + usage['system_us']) / 1e6
        elapsed = finish.get('elapsed_ns', 0) / 1e9
        result['process_cpu_percent_one_core'] = result['process_cpu_seconds'] / elapsed * 100 if elapsed else None
    else:
        result['process_lifetime_peak_rss_bytes'] = None
        values = [item.get('cpu_seconds') for item in process if item.get('cpu_seconds') is not None]
        result['process_sampled_cpu_seconds_lower_bound'] = max(values, default=None)
        result['process_cpu_seconds'] = None
        result['process_cpu_percent_one_core'] = None
    return result
