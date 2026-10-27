#!/usr/bin/env python3
# host.py: Управление измерениями synbench и HTTP/HTTPS стендом
# Автор: Елчинян Арен Артурович, 2026, x86_64, Linux ABI
# Подсистема: хостовые измерения Linux и SynapseOS
# Первое изменение: 19.10.2026, последнее изменение: 25.10.2026
# Доступ: общедоступный

"""Измерения выполняются по очереди внутри заранее подготовленных VM Debian, Alpine и SynapseOS

Команды из JSON запускаются с правами указанного SSH-пользователя
В targets задаются VM, пути agent, workload, qwen, model и curl
workdir создаётся через mktemp на каждой VM и сохраняется для разбора ошибок
Старые результаты и образы не перезаписываются

Пример подготовки и запуска:
  make all qwen
  python3 host.py init --output /tmp/synbench-plan.json
  python3 host.py serve --bind 0.0.0.0 --port 8088 --payload-mib 64
  python3 host.py serve --bind 0.0.0.0 --port 8443 --payload-mib 64 \
      --tls-cert /tmp/bench.crt --tls-key /tmp/bench.key
  python3 host.py run --config /tmp/synbench-plan.json --output /tmp/synbench-results

Отредактировать план до запуска, установить бинарники в обе VM и одинаковый curl
В HTTPS-нагрузках сертификат проверяется через CA-файл, hostname bench.test
Сертификат должен содержать subjectAltName DNS:bench.test
SHA-256 модели и бинарников проверяется до нагрузки через команду sha256sum
Модель, CA, образ VM и секретный ключ сервера не входят в пакет
Запись включается через console и video_qmp каждого target
QMP принадлежит тестовой VM, вывод копируется в её настоящий tty и консольный лог
console.mp4 содержит только кадры QEMU, video-timing.json содержит время съёмки и ошибки
Для Linux видимая консоль tty1 должна быть активной, для SynapseOS используется tty0
Размер кадра приводится к общему полю 1920x1080 с сохранением пропорций
Последний кадр удерживается одну секунду после окончания, это отмечено в video-timing.json
VM-параметры в плане являются заявленными, автоматическая проверка QEMU пока не выполняется
matching_artifacts требует одинаковые SHA-256 выбранных файлов в обеих VM
Сборка make portable даёт один статический набор для проверки в обеих ОС
host_pids задаёт PID QEMU и HTTP/HTTPS-серверов для отдельного учёта ресурсов хоста
host_threads сопоставляет метку объекту {pid: QEMU_PID, tid: TID} для путей /proc/PID/task/TID
Только stat/status/schedstat потоков сохраняются отдельно без суммирования
При sched_schedstats=0 нулевая дельта schedstat не доказывает отсутствие ожидания CPU

monitoring_check должен проверять работающий сервис и доставку на synapseos.ru
Он возвращает JSON running=true, destination="synapseos.ru", delivery_confirmed=true
Команда проверяется перед и после каждой нагрузки, код 0 без этих полей не принимается
Отсутствие подтверждения останавливает измерения, сетевой вызов не подменяет мониторинг

Процент CPU процесса задаётся относительно одного ядра, может превышать 100
Физическая память гостя и RSS QEMU имеют разные определения
MemAvailable SynapseOS соответствует MemFree текущей реализации
Сумма RSS разных процессов может повторно учитывать общие страницы
Измеритель опрашивает непосредственно запущенный процесс, shell-обёртки скрывают его потомков
Linux rusage может включать потомков, завершения которых ожидала измеряемая программа
Счётчики Linux vmstat, diskstats и TCP сохраняются только там, где они доступны
Значения sampled peak являются нижними оценками, короткие пики между опросами теряются
collection_ns позволяет оценить цену сбора, время команды включает запуск и опрос
Передачи имеют собственный таймер curl без этой погрешности
Проверка скачивания выполняется после измерения, проверка загрузки на сервер входит в ответ
Чтение файла после записи проверяет байты; наличие кэша зависит от гостевой ОС
Qwen использует token ID 1..1000 и фиксированное число forward, EOS не завершает тест
Qwen-таймер загрузки может не включать все page fault mmap, они видны в prefill
Однопоточная сборка Qwen использует одинаковые флаги x86-64 для обеих ОС
GPU и AVX-оптимизации в данный набор не включены
"""

import argparse
import hashlib
import http.server
import json
import math
import os
import shlex
import signal
import socket
import ssl
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

from metrics import host_sample, normalised_events, summarise, summarise_host
from recording import ScreenRecorder


def write_json(path, value):
    """Записать один результат, отклоняя NaN и бесконечность."""
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n')


def record_host(stop, destination, pids, interval, errors, thread_pids=None):
    """Сохранять временной ряд хоста до сигнала завершения."""
    try:
        with destination.open('x') as output:
            while True:
                output.write(json.dumps(host_sample(dict(pids), thread_pids), allow_nan=False) + '\n')
                output.flush()
                if stop.wait(interval):
                    output.write(json.dumps(host_sample(dict(pids), thread_pids), allow_nan=False) + '\n')
                    break
    except (OSError, ValueError) as error:
        errors.append(str(error))


def command_for(target, command):
    """Передать удалённую команду SSH с корректным экранированием аргументов."""
    if target['transport'] == 'local':
        return command
    if target['transport'] != 'ssh':
        raise ValueError('transport must be local or ssh')
    argv = ['ssh', '-T', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10']
    if target.get('identity_file'):
        argv += ['-i', target['identity_file']]
    if target.get('known_hosts'):
        argv += ['-o', 'UserKnownHostsFile=' + target['known_hosts'],
                 '-o', 'StrictHostKeyChecking=yes']
    return argv + ['-p', str(target.get('port', 22)), '--',
                   target['destination'], shlex.join(command)]


def load_events(path):
    """Прочитать только полные JSON-события, выявляя повреждённый вывод."""
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def run_one(target, workload, index, directory, settings):
    """Измерить команду внутри выбранной VM и независимо сохранить расход ресурсов хоста."""
    directory.mkdir()
    timeout = workload.get('timeout_seconds', settings.get('timeout_seconds', 300))
    command = [target.get('agent', '/bin/synbench'), '--interval-ms',
               str(settings.get('interval_ms', 100)), '--timeout', str(timeout)]
    if target.get('console'):
        command += ['--console', target['console']]
        if target.get('compact_console'):
            command += ['--label', workload['name']]
    command += ['--', *workload['argv']]
    stop = threading.Event()
    collector_errors = []
    observed_pids = {'controller': os.getpid(), **settings.get('host_pids', {})}
    monitor = threading.Thread(target=record_host,
                               args=(stop, directory / 'host.jsonl',
                                     observed_pids,
                                     settings.get('interval_ms', 100) / 1000, collector_errors,
                                     settings.get('host_threads')), daemon=True)
    monitor.start()
    result = {'target': target['name'], 'workload': workload['name'], 'repeat': index,
              'argv': workload['argv'], 'started_ns': time.time_ns()}
    process = None
    video = None
    try:
        if target.get('video_qmp') and not settings.get('continuous_video'):
            video = ScreenRecorder(target['video_qmp'], directory)
            video.start()
            observed_pids['video_encoder'] = video.encoder.pid
        with (directory / 'guest.jsonl').open('x') as output, (directory / 'command.log').open('xb') as log:
            process = subprocess.Popen(command_for(target, command), stdin=subprocess.DEVNULL,
                                       stdout=output, stderr=log, start_new_session=True)
            try:
                result['transport_exit_code'] = process.wait(timeout=timeout + 30)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                result['controller_timeout'] = True
        try:
            events = load_events(directory / 'guest.jsonl')
            result['metrics'] = summarise(events)
            write_json(directory / 'series.json', normalised_events(events))
        except (ValueError, KeyError, TypeError) as error:
            result['parse_error'] = str(error)
        log = (directory / 'command.log').read_text(errors='replace')
        result['workload_results'] = []
        for line in log.splitlines():
            if line.startswith('SYNBENCH_RESULT '):
                try:
                    item = json.loads(line.removeprefix('SYNBENCH_RESULT '))
                    result['workload_results'].append(item)
                except ValueError:
                    result['workload_result_parse_error'] = True
        finish = result.get('metrics', {}).get('finish') or {}
        metadata = result.get('metrics', {}).get('metadata', {})
        result['success'] = (result.get('transport_exit_code') == 0 and finish.get('exit_code') == 0
                             and not finish.get('signal') and not finish.get('timed_out')
                             and not result.get('parse_error') and not result.get('workload_result_parse_error'))
        if metadata.get('os') != target.get('expected_os'):
            result.update({'success': False, 'os_mismatch': True})
        for item in result['workload_results']:
            byte_count = item.get('transfer_bytes')
            duration = item.get('transfer_seconds')
            if byte_count is not None and duration is not None and duration > 0:
                item['transfer_mb_s'] = byte_count / duration / 1e6
                item['transfer_mib_s'] = byte_count / duration / (1024 * 1024)
            if item.get('verified') is False:
                result['success'] = False
        if workload.get('verify') and result['success']:
            result['verification'] = verify_result(target, workload['verify'])
            result['success'] = result['verification']['success']
    except OSError as error:
        result.update({'success': False, 'error': str(error)})
    finally:
        if process is not None and process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        stop.set()
        monitor.join()
        if video is not None:
            result['video'] = video.stop()
            if result['video']['errors'] or result['video']['frames'] == 0:
                result['success'] = False
    if collector_errors:
        result.update({'success': False, 'host_collector_errors': collector_errors})
    if (directory / 'host.jsonl').exists():
        result['host_metrics'] = summarise_host(load_events(directory / 'host.jsonl'))
    write_json(directory / 'result.json', result)
    return result


def validate_config(config):
    """Проверить план до запуска команд и создания результатов."""
    if not config.get('targets') or not config.get('workloads'):
        raise ValueError('targets and workloads must be nonempty')
    if not 10 <= config.get('interval_ms', 100) <= 60000:
        raise ValueError('interval_ms must be 10..60000')
    if not 1 <= config.get('repeats', 5) <= 100:
        raise ValueError('repeats must be 1..100')
    for kind in ('targets', 'workloads'):
        names = []
        for item in config[kind]:
            name = item['name']
            if not isinstance(name, str) or not name or any(
                    character not in 'abcdefghijklmnopqrstuvwxyz0123456789-_' for character in name):
                raise ValueError('Names must contain lowercase ASCII, digits, - or _')
            names.append(name)
        if len(names) != len(set(names)):
            raise ValueError('Duplicate names')
    for item in config['workloads']:
        if not isinstance(item['argv'], list) or not item['argv'] or not all(
                isinstance(arg, str) and '\0' not in arg for arg in item['argv']):
            raise ValueError('argv must be a nonempty array of strings')
        timeout = item.get('timeout_seconds', config.get('timeout_seconds', 300))
        if not isinstance(timeout, int) or not 1 <= timeout <= 86400:
            raise ValueError('timeout_seconds must be 1..86400')
    for target in config['targets']:
        command_for(target, ['true'])
        if config.get('require_video') and (not target.get('video_qmp') or not target.get('console')):
            raise ValueError('Each recorded target needs console and video_qmp')
        if not target.get('monitoring_check') or target.get('expected_os') not in ('Linux', 'SynapseOS'):
            raise ValueError('Each target needs monitoring_check and expected_os')
        if target['transport'] == 'ssh' and target['expected_os'] == 'Linux' and not target.get('vm'):
            raise ValueError('Record Linux VM configuration in target.vm')
        for artifact in target.get('artifacts', []):
            checksum = artifact.get('sha256')
            if checksum is not None and (not isinstance(checksum, str) or len(checksum) != 64 or any(
                    char not in '0123456789abcdef' for char in checksum)):
                raise ValueError('Set the real lowercase SHA-256 before running the suite')
    for name, pid in config.get('host_pids', {}).items():
        if not isinstance(name, str) or not isinstance(pid, int) or pid <= 0:
            raise ValueError('host_pids must map names to positive PID values')


def checked_command(target, command, timeout=30):
    """Получить короткий вывод служебной команды вне измеряемого интервала."""
    process = subprocess.run(command_for(target, command), stdin=subprocess.DEVNULL,
                             capture_output=True, timeout=timeout, check=True)
    return process.stdout.decode(errors='strict').strip()


def verify_result(target, verification):
    """Проверить SHA скачанного файла либо ответ о проверке upload на сервере."""
    try:
        output = checked_command(target, verification['argv'], timeout=120)
        if verification['kind'] == 'sha256':
            success = output.split()[0] == verification['sha256']
        elif verification['kind'] == 'upload':
            answer = json.loads(output)
            success = (answer.get('verified') is True and answer.get('sha256') == verification['sha256']
                       and answer.get('bytes') == verification['bytes'])
        else:
            raise ValueError('Unknown verification kind')
        return {'success': success, 'output': output}
    except (OSError, ValueError, IndexError, subprocess.SubprocessError) as error:
        return {'success': False, 'error': str(error)}


def monitoring_check(target):
    """Требовать содержательное подтверждение работы и доставки мониторинга."""
    answer = json.loads(checked_command(target, target['monitoring_check']))
    if (answer.get('running') is not True or answer.get('destination') != 'synapseos.ru'
            or answer.get('delivery_confirmed') is not True):
        raise RuntimeError(f"{target['name']}: monitoring is not confirmed for synapseos.ru")
    return {'checked_ns': time.time_ns(), 'evidence': answer}


def expand_workload(workload, target):
    """Подставить пути конкретной VM в массивы аргументов без shell."""
    def expand(value):
        if isinstance(value, str):
            for key in ('scratch', 'model', 'qwen', 'workload', 'curl', 'ca'):
                value = value.replace('{' + key + '}', target.get(key, '{' + key + '}'))
            return value
        if isinstance(value, list):
            return [expand(item) for item in value]
        if isinstance(value, dict):
            return {key: expand(item) for key, item in value.items()}
        return value
    return expand(workload)


def aggregate(results):
    """Свести одинаковые числовые показатели без удаления неудачных прогонов."""
    groups = {}
    for result in results:
        key = result['target'] + '/' + result['workload']
        group = groups.setdefault(key, {'attempts': 0, 'failures': 0, 'values': {}, 'checksums': []})
        group['attempts'] += 1
        group['failures'] += int(not result['success'])
        if not result['success']:
            continue
        values = dict(result.get('metrics', {}))
        for item in result.get('workload_results', []):
            values.update(item)
            for checksum_key in ('checksum', 'token_checksum'):
                if checksum_key in item:
                    group['checksums'].append(item[checksum_key])
        for name, value in values.items():
            if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value):
                group['values'].setdefault(name, []).append(value)
    for group in groups.values():
        for name, values in list(group['values'].items()):
            ordered = sorted(values)
            group['values'][name] = {'count': len(values), 'min': min(values), 'max': max(values),
                                     'median': statistics.median(values), 'mean': statistics.mean(values),
                                     'p95_nearest_rank': ordered[math.ceil(0.95 * len(ordered)) - 1],
                                     'stdev': statistics.stdev(values) if len(values) > 1 else None}
    return groups


def checksum_comparison(results):
    """Выявить различающиеся результаты вычислений между ОС и повторениями."""
    groups = {}
    for result in results:
        if not result['success']:
            continue
        for item in result.get('workload_results', []):
            for key in ('checksum', 'token_checksum'):
                if key in item:
                    groups.setdefault(result['workload'] + '/' + key, set()).add(item[key])
    return {name: {'values': sorted(values), 'match': len(values) == 1} for name, values in groups.items()}


def run_suite(args):
    """Чередовать ОС между повторами и требовать явной команды проверки мониторинга."""
    config = json.loads(args.config.read_text())
    validate_config(config)
    args.output.mkdir(parents=True, exist_ok=False)
    write_json(args.output / 'config.json', config)
    prepared = []
    for target in config['targets']:
        target = dict(target)
        scratch_parent = target.get('scratch_parent', '/tmp').rstrip('/')
        if not scratch_parent.startswith('/') or '\n' in scratch_parent:
            raise ValueError('Scratch parent must be an absolute path')
        target['scratch'] = checked_command(target, ['mktemp', '-d',
                                                     scratch_parent + '/synbench-XXXXXXXX'])
        if not target['scratch'].startswith(scratch_parent + '/synbench-') or '\n' in target['scratch']:
            raise ValueError('Unexpected scratch path')
        evidence = {'scratch': target['scratch'], 'hashes': {}, 'roles': {},
                    'vm_configuration': target.get('vm'), 'vm_configuration_verified': False}
        if target.get('expected_distribution'):
            evidence['os_release'] = checked_command(target, ['cat', '/etc/os-release'])
            release = dict(line.split('=', 1) for line in evidence['os_release'].splitlines() if '=' in line)
            actual_id = release.get('ID', '').strip('"')
            if actual_id != target['expected_distribution']:
                raise ValueError(target['name'] + ': unexpected distribution ID: ' + actual_id)
        for artifact in target.get('artifacts', []):
            output = checked_command(target, ['sha256sum', artifact['path']], timeout=120)
            actual = output.split()[0]
            evidence['hashes'][artifact['path']] = actual
            if artifact.get('role'):
                evidence['roles'][artifact['role']] = actual
            if artifact.get('sha256') and artifact['sha256'] != actual:
                raise ValueError('Artifact checksum mismatch: ' + artifact['path'])
        if any('https-' in item['name'] for item in config['workloads']):
            version = checked_command(target, [target['curl'], '--version'])
            evidence['curl_version'] = version
            protocol_line = next((line for line in version.splitlines() if line.startswith('Protocols:')), '')
            if 'https' not in protocol_line.split():
                raise ValueError(target['name'] + ': curl has no HTTPS support')
        write_json(args.output / (target['name'] + '-identity.json'), evidence)
        target['identity'] = evidence
        prepared.append(target)
    for role in config.get('matching_artifacts', []):
        hashes = [target['identity']['roles'].get(role) for target in prepared]
        if None in hashes or len(set(hashes)) != 1:
            raise ValueError('Comparable artifacts must have identical hashes: ' + role)
    results = []
    recorder = None
    if config.get('continuous_video'):
        if len(prepared) != 1 or config.get('repeats', 5) != 1:
            raise ValueError('Continuous video needs one target and one repeat')
        video_dir = Path(config['video_directory'])
        video_dir.mkdir(parents=True, exist_ok=False)
        recorder = ScreenRecorder(prepared[0]['video_qmp'], video_dir)
        recorder.start()
    try:
        for repeat in range(1, config.get('repeats', 5) + 1):
            targets = prepared if repeat % 2 else list(reversed(prepared))
            for target in targets:
                for workload in config['workloads']:
                    name = f"{repeat:02d}-{target['name']}-{workload['name']}"
                    before = monitoring_check(target)
                    result = run_one(target, expand_workload(workload, target), repeat, args.output / name, config)
                    result['monitoring_before'] = before
                    try:
                        result['monitoring_after'] = monitoring_check(target)
                    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
                        result.update({'success': False, 'monitoring_error': str(error)})
                    write_json(args.output / name / 'result.json', result)
                    results.append(result)
                    write_json(args.output / 'results.json', results)
                    write_json(args.output / 'summary.json', aggregate(results))
                    write_json(args.output / 'correctness.json', checksum_comparison(results))
                    print(name, 'PASS' if result['success'] else 'FAIL', flush=True)
                    if config.get('inter_test_delay_seconds'):
                        time.sleep(config['inter_test_delay_seconds'])
    finally:
        if recorder is not None:
            video_result = recorder.stop()
            write_json(args.output / 'video-result.json', video_result)
    checksums = checksum_comparison(results)
    return int(any(not result['success'] for result in results) or any(
        not comparison['match'] for comparison in checksums.values()) or
        (recorder is not None and (video_result['errors'] or video_result['frames'] == 0)))


def init_config(args):
    """Составить редактируемый план без запуска VM или нагрузок."""
    block = bytes(range(256)) * 4096
    digest = hashlib.sha256()
    for _ in range(64):
        digest.update(block)
    checksum = digest.hexdigest()
    targets = []
    for name, system, port in (
            ('debian', 'Linux', 2223), ('alpine', 'Linux', 2224), ('synapse', 'SynapseOS', 2222)):
        targets.append({'name': name, 'expected_os': system, 'transport': 'ssh',
                        'destination': 'root@127.0.0.1', 'port': port, 'agent': '/bin/synbench',
                        'workload': '/bin/synbench-workload', 'qwen': '/bin/synbench-qwen',
                        'curl': '/bin/curl', 'model': '/data/Qwen3-0.6B.bin', 'ca': '/data/bench.crt',
                        'console': '/dev/tty0' if system == 'SynapseOS' else '/dev/tty1',
                        'video_qmp': f'/tmp/synbench-{name}/qmp',
                        'monitoring_check': ['/bin/check-benchmark-monitoring'],
                        'artifacts': [{'path': '/bin/synbench', 'role': 'agent'},
                                      {'path': '/bin/curl', 'role': 'curl'},
                                      {'path': '/bin/synbench-qwen', 'role': 'qwen'},
                                      {'path': '/bin/synbench-workload', 'role': 'workload'},
                                      {'path': '/data/Qwen3-0.6B.bin', 'role': 'model',
                                       'sha256': 'SET_MODEL_SHA256'}],
                        'vm': {'machine': 'q35', 'accelerator': 'kvm', 'cpu': 'host', 'vcpus': 4,
                               'memory_mib': 2048, 'nic': 'virtio-net', 'network_backend': 'user'}})
        if system == 'Linux':
            targets[-1]['expected_distribution'] = name
    workloads = []
    for protocol, port in (('http', 8088), ('https', 8443)):
        host = '10.0.2.2' if protocol == 'http' else 'bench.test'
        base = f'{protocol}://{host}:{port}'
        common = ['{curl}', '--fail', '--silent', '--show-error', '--noproxy', '*', '--http1.1',
                  '--connect-timeout', '10', '--max-time', '120']
        if protocol == 'https':
            common += ['--cacert', '{ca}', '--resolve', f'bench.test:{port}:10.0.2.2']
        for direction in ('download', 'upload'):
            size_field = 'size_download' if direction == 'download' else 'size_upload'
            report = ('SYNBENCH_RESULT {"transfer_seconds":%{time_total},"transfer_bytes":%{'
                      + size_field + '},"dns_seconds":%{time_namelookup},"connect_seconds":%{time_connect},'
                      '"tls_complete_seconds":%{time_appconnect},"first_byte_seconds":%{time_starttransfer},'
                      '"http_status":%{http_code}}\n')
            if direction == 'download':
                argv = common + ['--output', '{scratch}/payload', '--write-out', report, base + '/payload']
                verification = {'kind': 'sha256', 'argv': ['sha256sum', '{scratch}/payload'], 'sha256': checksum}
            else:
                argv = common + ['--upload-file', '{scratch}/payload', '--output', '{scratch}/upload.json',
                                 '--write-out', report, base + '/upload']
                verification = {'kind': 'upload', 'argv': ['cat', '{scratch}/upload.json'],
                                'sha256': checksum, 'bytes': 64 * 1024 * 1024}
            workloads.append({'name': protocol + '-' + direction, 'argv': argv, 'verify': verification})
    workloads += [
        {'name': 'cpu', 'argv': ['{workload}', 'cpu', '200000000']},
        {'name': 'memory-copy', 'argv': ['{workload}', 'memory', '32', '64']},
        {'name': 'file-read-after-write', 'argv': ['{workload}', 'file', '{scratch}', '64']},
        {'name': 'qwen3-06b', 'argv': ['{qwen}', '{model}', '32', '128'], 'timeout_seconds': 600},
    ]
    config = {'schema': 1, 'interval_ms': 100, 'repeats': 5, 'require_video': True, 'targets': targets,
              'host_pids': {}, 'matching_artifacts': ['model', 'curl', 'qwen', 'workload'],
              'workloads': workloads}
    with args.output.open('x') as output:
        json.dump(config, output, ensure_ascii=False, indent=2)
        output.write('\n')
    return 0


def serve(args):
    """Предоставить одинаковые данные по HTTP либо HTTPS и проверять загруженный файл."""
    block = bytes(range(256)) * 4096
    size = args.payload_mib * len(block)
    digest = hashlib.sha256()
    for _ in range(args.payload_mib):
        digest.update(block)
    expected = digest.hexdigest()

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def setup(self):
            super().setup()
            self.connection.settimeout(300)

        def do_GET(self):
            if self.path != '/payload':
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Length', str(size))
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('X-Content-SHA256', expected)
            self.end_headers()
            try:
                for _ in range(args.payload_mib):
                    self.wfile.write(block)
            except (OSError, TimeoutError) as error:
                print(f'GET transfer interrupted: {error}', file=sys.stderr, flush=True)
                self.close_connection = True

        def do_PUT(self):
            try:
                length = int(self.headers.get('Content-Length', '-1'))
            except ValueError:
                length = -1
            if self.path != '/upload' or length != size or self.headers.get('Transfer-Encoding'):
                self.close_connection = True
                self.send_error(400)
                return
            checksum = hashlib.sha256()
            remaining = length
            try:
                while remaining:
                    data = self.rfile.read(min(remaining, 1024 * 1024))
                    if not data:
                        self.close_connection = True
                        return
                    checksum.update(data)
                    remaining -= len(data)
            except (OSError, TimeoutError) as error:
                print(f'PUT transfer interrupted: {error}', file=sys.stderr, flush=True)
                self.close_connection = True
                return
            verified = checksum.hexdigest() == expected
            body = json.dumps({'bytes': length, 'sha256': checksum.hexdigest(), 'verified': verified}).encode()
            self.send_response(200 if verified else 422)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(body)

    if bool(args.tls_cert) != bool(args.tls_key):
        raise ValueError('Both --tls-cert and --tls-key are required')
    server = http.server.ThreadingHTTPServer((args.bind, args.port), Handler)
    if args.tls_cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.tls_cert, args.tls_key)
        server.socket = context.wrap_socket(server.socket, server_side=True, do_handshake_on_connect=False)
    print(json.dumps({'port': server.server_port, 'payload_bytes': size, 'sha256': expected,
                      'tls': bool(args.tls_cert), 'server_pid': os.getpid()}), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


def main():
    """Выбрать измерения VM, подготовку конфигурации или тестовый сервер."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='action', required=True)
    run = sub.add_parser('run')
    run.add_argument('--config', type=Path, required=True)
    run.add_argument('--output', type=Path, required=True)
    run.set_defaults(function=run_suite)
    init = sub.add_parser('init')
    init.add_argument('--output', type=Path, required=True)
    init.set_defaults(function=init_config)
    server = sub.add_parser('serve')
    server.add_argument('--bind', default='127.0.0.1')
    server.add_argument('--port', type=int, default=8088)
    server.add_argument('--payload-mib', type=int, choices=range(1, 8193), default=64)
    server.add_argument('--tls-cert', type=Path)
    server.add_argument('--tls-key', type=Path)
    server.set_defaults(function=serve)
    args = parser.parse_args()
    try:
        return args.function(args)
    except (ValueError, KeyError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    raise SystemExit(main())
