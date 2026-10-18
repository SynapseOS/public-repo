#!/usr/bin/env python3
"""Local webpanel demo: simulated API only, no host management commands."""
import argparse
import base64
import json
import math
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

ASSETS = Path(__file__).resolve().parent / 'files'
STATE_FILE = None
TOKEN = secrets.token_hex(32)
AUTH = 'Basic ' + base64.b64encode(b'root:demo').decode()
START = time.monotonic()
LOCK = threading.Lock()
SERVICES = {name: {'running': running, 'enabled': True} for name, running in
            [('ssh', True), ('nginx', True), ('webpanel', True), ('cron', False)]}
PACKAGES = {'kernel': '0.1.604-r1', 'setup': '0.1.512-r4', 'service': '0.0.0-r1',
            'webpanel': '0.3.1-r1', 'nginx': '1.30.5-r2', 'dropbear': '2025.89-r1',
            'nano': '8.7-r1', 'curl': '8.14.1-r1', 'htop': '3.4.1-r1'}
INSTALLED = set(PACKAGES) - {'nano', 'htop'}
INSTALLED_VERSIONS = {name:PACKAGES[name] for name in INSTALLED}
INSTALLED_VERSIONS['nginx']='1.30.4-r1'
LOGS = {'webpanel.log': '[demo] Панель запущена. Все действия выполняются на тестовых данных.\n',
        'nginx.log': '[notice] Демонстрационный веб-сервер готов\n',
        'ssh.log': '[notice] SSH listening on port 22\n'}
JOBS = {}
DIRECTORIES={'/','/etc','/etc/services','/etc/nginx','/home','/home/demo','/var','/var/www','/var/log','/tmp','/boot','/root'}
FILES={'/etc/hosts':b'127.0.0.1 localhost\n10.0.2.15 synapseos-demo\n','/etc/resolv.conf':b'nameserver 1.1.1.1\nnameserver 8.8.8.8\n','/etc/services/ssh':b'exec /bin/dropbear -F -E\n','/etc/nginx/nginx.conf':b'events { worker_connections 128; }\nhttp { server { listen 8080; root /var/www; } }\n','/home/demo/readme.txt':'SynapseOS: демонстрационные файлы.\n'.encode(),'/var/www/index.html':b'<h1>SynapseOS demo</h1>\n'}
BACKUPS={}
PROCESSES=[dict(pid=1,name='init',state=1,threads=1,memory_bytes=1048576,cpu_percent=0.1,cpu_us='1000000',protected=True),dict(pid=110,name='webpanel',state=1,threads=1,memory_bytes=4194304,cpu_percent=0.5,cpu_us='3250000',protected=True),dict(pid=120,name='nginx',state=1,threads=2,memory_bytes=8388608,cpu_percent=0.8,cpu_us='8200000',protected=True),dict(pid=130,name='dropbear',state=1,threads=1,memory_bytes=2097152,cpu_percent=0.2,cpu_us='1400000',protected=False),dict(pid=174,name='python worker.py',state=0,threads=4,memory_bytes=58720256,cpu_percent=12.4,cpu_us='32000000',protected=False)]

def trim_jobs():
    while len(JOBS)>32:
        key=min(JOBS,key=int);del JOBS[key];BACKUPS.pop(key,None)

def audit_demo(operation,action,name,output,code=0):
    key=str(max((int(k) for k in JOBS),default=0)+1)
    JOBS[key]=dict(values=dict(operation=operation,action=action,name=name),created=int(time.time()),finished=int(time.time()),ready=0,done=True,exit=code,output=output)
    trim_jobs();return int(key)

def allowed_path(path):
    return path.startswith('/') and len(path.encode())<256 and not any(ord(c)<32 or ord(c)==127 or c=='\\' for c in path) and (path=='/' or all(part and part not in ('.','..') for part in path[1:].split('/'))) and any(path==root or path.startswith(root+'/') for root in ('/etc','/var','/home','/root','/tmp','/boot','/'))

def editable(path):
    return path in ('/etc/hosts','/etc/resolv.conf','/etc/nginx/nginx.conf') or path.startswith('/etc/services/') and '/' not in path[14:] and path[14:] not in ('nginx','webpanel')

def file_revision(content):
    value=14695981039346656037
    for byte in content: value=((value^byte)*1099511628211)&((1<<64)-1)
    return f'{value:016x}'

DESCRIPTIONS = {'nano':'Текстовый редактор', 'htop':'Монитор процессов', 'curl':'Загрузка по HTTP и HTTPS',
 'kernel':'Ядро SynapseOS', 'setup':'Менеджер пакетов', 'service':'Супервизор служб',
 'nginx':'HTTP-сервер', 'webpanel':'Панель управления сервером', 'dropbear':'SSH-сервер'}

def save_state():
    if STATE_FILE:
        temporary=STATE_FILE.with_suffix('.tmp')
        temporary.write_text(json.dumps({'jobs':JOBS,'services':SERVICES,'installed':sorted(INSTALLED),'installed_versions':INSTALLED_VERSIONS,'logs':LOGS,'directories':sorted(DIRECTORIES),'files':{k:base64.b64encode(v).decode() for k,v in FILES.items()},'backups':BACKUPS,'processes':PROCESSES},ensure_ascii=False))
        temporary.replace(STATE_FILE)

def history_item(key,job):
    return dict(id=int(key),user='root',created=job['created'],finished=job.get('finished',0),done=job['done'],exit=job['exit'],unknown=job.get('unknown',False),operation=job['values']['operation'],action=job['values'].get('action',''),name=job['values'].get('name',''))


def finish(job):
    if job['done'] or time.monotonic() < job['ready']:
        return
    v = job['values']; op = v.get('operation'); action = v.get('action'); name = v.get('name')
    result = ''; code = 0
    if op == 'services':
        result = ''.join(f"{n:16} {'running' if s['running'] else 'stopped':9} pid={120+i if s['running'] else 0} enabled={'yes' if s['enabled'] else 'no'} retry=0s\n" for i, (n,s) in enumerate(SERVICES.items()))
    elif op == 'packages':
        names = sorted(INSTALLED if action == 'installed' else PACKAGES)
        result = ''.join(f"{n} {INSTALLED_VERSIONS[n] if action=="installed" else PACKAGES[n]} [demo]{' (installed)' if n in INSTALLED else ''} {DESCRIPTIONS[n] if action == 'catalog' else ''}\n" for n in names)
    elif op == 'package-info':
        if name not in PACKAGES: code=1;result=f'Пакет {name} не найден.\n'
        else: result=f'{name} {PACKAGES[name]} [demo] {DESCRIPTIONS[name]}\n  x86_64 synapse-x86_64-v1, 262144 bytes, application\n  requires kernel >= 0.1.554-r1\n'+('  requires ncurses >= 6.5\n' if name=='nano' else '')
    elif op in ('package-plan','package-updates'):
        if op=='package-plan' and name not in PACKAGES: code=1;result=f'Пакет {name} не найден.\n'
        elif op=='package-updates':
            pending=[n for n in INSTALLED if INSTALLED_VERSIONS.get(n)!=PACKAGES[n]]
            result=''.join(f'Upgrade {n} {INSTALLED_VERSIONS.get(n)} -> {PACKAGES[n]} (demo)\n' for n in sorted(pending))+f'Plan: {len(pending)} package changes, {4*len(pending)} file operations, {262144*len(pending)} download bytes\n'
        else: result=f'{action}: {name} {PACKAGES[name]} (demo)\nPlan: 1 package changes, 4 file operations, {0 if action=="remove" else 262144} download bytes\nПредварительный просмотр: состояние системы не изменено.\n'
    elif op == 'disk-usage':
        directory=v['path'];sizes={}
        for path,content in FILES.items():
            if path.startswith(directory.rstrip('/')+'/'):
                child=path[len(directory.rstrip('/'))+1:].split('/')[0];sizes[child]=sizes.get(child,0)+len(content)
        result=f'Логический размер файлов в {directory} (demo)\n'+''.join(f'{size}\t{name}\n' for name,size in sizes.items())+f'Итого: {sum(sizes.values())} байт.\nСканирование завершено.\n'
    elif op == 'network':
        result = 'lo       127.0.0.1/8\nvtnet0   10.0.2.15/24\n'
    elif op == 'service':
        if name not in SERVICES:
            code = 1; result = f'Служба {name} не найдена.\n'
        else:
            if action in ('start', 'restart', 'stop'): SERVICES[name]['running'] = action != 'stop'
            else: SERVICES[name]['enabled'] = action == 'enable';SERVICES[name]['running'] = action == 'enable'
            result = f'{name}: {action} requested (demo)\n'
            LOGS.setdefault(name+'.log','')
            LOGS[name+'.log'] += f'[supervisor 110] {action} pid=120 detail=0 time={int(time.time())}\n'
    elif op == 'package':
        if action == 'update': result = 'Обновление демонстрационного каталога…\nКаталог обновлён.\n'
        elif name not in PACKAGES: code = 1; result = f'Пакет {name} не найден.\n'
        else:
            if action == 'remove': INSTALLED.discard(name);INSTALLED_VERSIONS.pop(name,None)
            else: INSTALLED.add(name);INSTALLED_VERSIONS[name]=PACKAGES[name]
            result = f'{action}: {name} {PACKAGES[name]} (demo)\nГотово.\n'
    job.update(done=True, exit=code, output=result,finished=int(time.time()))
    if op in ('service','package'): LOGS['webpanel.log'] += f'[info] {time.strftime("%Y-%m-%d %H:%M:%S")} {op} {action} {name or ""}: exit={code}\n'
    save_state()

class Handler(BaseHTTPRequestHandler):
    def respond(self, status, body, kind='text/plain; charset=utf-8'):
        if not isinstance(body, bytes): body = body.encode()
        self.send_response(status); self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body))); self.send_header('Cache-Control', 'no-store')
        self.end_headers(); self.wfile.write(body)
    def data(self, value): self.respond(200, json.dumps(value, ensure_ascii=False), 'application/json; charset=utf-8')
    def authenticated(self):
        if self.headers.get('Authorization') == AUTH: return True
        self.respond(401, 'Authentication required'); return False
    def do_GET(self):
        url = urlsplit(self.path); query = parse_qs(url.query)
        if not url.path.startswith('/api/'):
            name = 'index.html' if url.path == '/' else url.path.lstrip('/')
            if name not in ('index.html','app.js','style.css'): return self.respond(404, 'Not found')
            kind = {'index.html':'text/html; charset=utf-8','app.js':'text/javascript; charset=utf-8','style.css':'text/css; charset=utf-8'}[name]
            return self.respond(200, (ASSETS/name).read_bytes(), kind)
        if not self.authenticated(): return
        with LOCK:
            if self.management_get(url,query):return
            if url.path == '/api/system':
                return self.data(dict(csrf=TOKEN, hostname='SynapseOS — демо', release='0.1.604 (демонстрация)', uptime=7380+int(time.monotonic()-START), cpu_core_percent=round(9+4*math.sin(time.monotonic()/8),1), memory_used=384*1048576, memory_total=2048*1048576))
            if url.path == '/api/history':
                for job in JOBS.values(): finish(job)
                return self.data([history_item(key,job) for key,job in sorted(JOBS.items(),key=lambda pair:int(pair[0]),reverse=True)])
            if url.path == '/api/service':
                name=query.get('name',[''])[0]
                if name not in SERVICES: return self.respond(404,'Service not found')
                service=SERVICES[name]
                return self.data(dict(name=name,state='running' if service['running'] else 'stopped',pid=120 if service['running'] else 0,supervisor=110,desired=service['running'],autostart=service['enabled'],retry=0,heartbeat_age=1,memory_bytes=4194304 if service['running'] else -1,cpu_seconds=3.25 if service['running'] else -1,config=f'# {name} (demo)\nexec /bin/{"dropbear -F -E" if name=="ssh" else name}\n',config_truncated=False,events=''.join(line+'\n' for line in LOGS.get(name+'.log','').splitlines() if line.startswith('[supervisor '))))
            if url.path == '/api/logs': return self.data(sorted(LOGS))
            if url.path == '/api/log':
                name = query.get('name',[''])[0]
                return self.respond(200, LOGS[name]) if name in LOGS else self.respond(404, 'Log not found')
            if url.path == '/api/job':
                job = JOBS.get(query.get('id',[''])[0])
                if not job: return self.respond(404,'Job expired')
                finish(job); return self.data({k:job[k] for k in ('done','exit','output')})
            self.respond(404,'Unknown API')
    def management_get(self,url,query):
        if url.path=='/api/processes': self.data(PROCESSES);return True
        if url.path=='/api/disks':
            self.data([dict(path=p,device=1 if p!='/tmp' else 2,total=20*1024**3 if p!='/tmp' else 512*1024**2,free=14*1024**3 if p!='/tmp' else 480*1024**2,available=14*1024**3 if p!='/tmp' else 480*1024**2,files=100000,files_free=95000) for p in ('/','/home','/tmp','/var/log')]);return True
        if url.path not in ('/api/files','/api/file','/api/download'):return False
        path=query.get('path',[''])[0]
        if not allowed_path(path):self.respond(400,'Недопустимый путь.');return True
        if url.path=='/api/files':
            if path not in DIRECTORIES:self.respond(404,'Каталог недоступен.');return True
            prefix=path.rstrip('/')+'/'
            entries=[]
            for name in sorted(DIRECTORIES|FILES.keys()):
                if not name.startswith(prefix) or not name[len(prefix):] or '/' in name[len(prefix):]:continue
                entries.append(dict(name=name[len(prefix):],type='directory' if name in DIRECTORIES else 'file',size=len(FILES.get(name,b'')),modified=int(time.time()),mode=420))
            self.data(dict(path=path,entries=entries,truncated=False));return True
        if path not in FILES:self.respond(404,'Файл недоступен.');return True
        content=FILES[path]
        if url.path=='/api/download':self.respond(200,content,'application/octet-stream');return True
        try:
            text=content.decode('utf-8');is_text=all(ord(c)>=32 or c in '\n\r\t' for c in text)
        except UnicodeDecodeError:text=None;is_text=False
        self.data(dict(path=path,size=len(content),text=is_text,content=text[:65536] if is_text else None,truncated=len(content)>65536,editable=is_text and len(content)<=65536 and editable(path),revision=file_revision(content)));return True
    def management_post(self,url,query,body):
        if url.path=='/api/process':
            values={k:v[0] for k,v in parse_qs(body.decode()).items()}
            target=next((p for p in PROCESSES if str(p['pid'])==values.get('pid')),None)
            if not target:self.respond(404,'Процесс уже завершён.');return True
            if target['protected']:self.respond(400,'Системный процесс защищён.');return True
            if target['name']!=values.get('name') or int(values.get('cpu_us','0'))>int(target['cpu_us']):self.respond(409,'Процесс изменился.');return True
            if values.get('signal') not in ('TERM','KILL'):self.respond(400,'Неверный сигнал.');return True
            PROCESSES.remove(target);job=audit_demo('process',values['signal'],values['pid'],f"PID {target['pid']}: сигнал {values['signal']} отправлен (demo).\n");save_state();self.data({'job':job});return True
        if url.path not in ('/api/usage','/api/config','/api/upload','/api/mkdir'):return False
        path=query.get('path',[''])[0]
        if not allowed_path(path):self.respond(400,'Недопустимый путь.');return True
        if url.path=='/api/usage':
            if path not in DIRECTORIES:self.respond(404,'Каталог недоступен.');return True
            key=str(max((int(k) for k in JOBS),default=0)+1)
            JOBS[key]=dict(values=dict(operation='disk-usage',path=path),created=int(time.time()),ready=time.monotonic()+1,done=False,exit=-1,output='Сканирование демонстрационного каталога…\n');trim_jobs();save_state();self.data({'job':int(key)});return True
        if url.path in ('/api/upload','/api/mkdir'):
            if not any(path.startswith(root+'/') for root in ('/home','/root','/var/www','/tmp')):self.respond(400,'Загрузка в этот каталог не разрешена.');return True
            if path in FILES or path in DIRECTORIES:self.respond(409,'Файл или каталог уже существует.');return True
            if path.rsplit('/',1)[0] not in DIRECTORIES:self.respond(404,'Каталог недоступен.');return True
            if url.path=='/api/upload':FILES[path]=body
            else:DIRECTORIES.add(path)
            job=audit_demo('file','upload' if url.path=='/api/upload' else 'mkdir','file',f'{path}: готово (demo).\n');save_state();self.data({'job':job});return True
        if not editable(path) or path not in FILES:self.respond(400,'Редактирование этого файла не поддерживается.');return True
        if self.headers.get('X-Webpanel-Revision')!=file_revision(FILES[path]):self.respond(409,'Файл изменился после открытия. Откройте его заново; ваши правки сохранены в редакторе.');return True
        restore=self.headers.get('X-Webpanel-Restore')
        if restore:
            backup=BACKUPS.get(restore)
            if not backup or backup['path']!=path:self.respond(404,'Копия не относится к этому файлу.');return True
            body=base64.b64decode(backup['content'])
        try:
            text=body.decode('utf-8');valid=len(body)<=65536 and '\x00' not in text
            lines=[line.strip() for line in text.splitlines() if line.strip() and not line.lstrip().startswith('#')]
            if path.startswith('/etc/services/'):
                import shlex
                valid=valid and len(lines)==1 and lines[0].startswith('exec ') and not any(c in lines[0] for c in ';|&<>$`') and len(shlex.split(lines[0]))>1
            elif path=='/etc/nginx/nginx.conf':valid=valid and 'invalid' not in text and text.count('{')==text.count('}') and 'events' in text
            elif path=='/etc/hosts':
                import ipaddress
                for line in lines:parts=line.split('#')[0].split();ipaddress.ip_address(parts[0]);valid=valid and len(parts)>1
            elif path=='/etc/resolv.conf':
                import ipaddress
                for line in lines:
                    parts=line.split('#')[0].split();valid=valid and len(parts)>1 and parts[0] in ('nameserver','search','domain','options')
                    if parts[0]=='nameserver':ipaddress.ip_address(parts[1])
        except (ValueError,IndexError):valid=False
        if valid:
            backup={'path':path,'content':base64.b64encode(FILES[path]).decode()};FILES[path]=body
            job=audit_demo('config','restore' if restore else 'save','config',f'{path}: конфигурация проверена и сохранена (demo).\nСлужба автоматически не перезапускается.\n');BACKUPS[str(job)]=backup
        else:job=audit_demo('config','restore' if restore else 'save','config','Неверный формат конфигурации. Исходный файл не изменён.\n',1)
        save_state();self.data(dict(job=job,saved=valid,revision=file_revision(body),backup=job if valid else 0));return True
    def do_POST(self):
        if not self.authenticated():return
        if self.headers.get('X-Webpanel-Token')!=TOKEN:return self.respond(403,'Request token required')
        try:length=int(self.headers.get('Content-Length',0))
        except ValueError:return self.respond(400,'Invalid body')
        url=urlsplit(self.path);query=parse_qs(url.query)
        limit=65536 if url.path=='/api/config' else 1048576 if url.path=='/api/upload' else 512
        if length<0 or length>limit:return self.respond(413,'Body too large')
        body=self.rfile.read(length)
        with LOCK:
            for job in JOBS.values():finish(job)
            if any(not job['done'] for job in JOBS.values()):return self.respond(409,'Another task is running')
            if self.management_post(url,query,body):return
            if url.path!='/api/run':return self.respond(404,'Unknown API')
            values={k:v[0] for k,v in parse_qs(body.decode()).items()}
            op,action,name=(values.get(k,'') for k in ('operation','action','name'))
            if op not in ('services','packages','network','service','package','package-info','package-plan','package-updates'):return self.respond(400,'Invalid operation')
            if op=='service' and (action not in ('start','stop','restart','enable','disable') or name in ('nginx','webpanel')):return self.respond(400,'Manage panel services from SSH')
            if op in ('package','package-plan') and (action not in ('update','install','upgrade','remove') or action=='remove' and name in ('kernel','init','sh','sbase','ubase','setup','service','nginx','webpanel','dropbear')):return self.respond(400,'Protected package or invalid action')
            key=str(max((int(k) for k in JOBS),default=0)+1)
            JOBS[key]=dict(values=values,created=int(time.time()),ready=time.monotonic()+(3 if op in ('service','package') else .6),done=False,exit=-1,output='Выполняется демонстрационная операция…\n')
            trim_jobs();save_state();self.data({'job':int(key)})

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',type=int,default=8444)
    parser.add_argument('--state',type=Path)
    args=parser.parse_args();STATE_FILE=args.state
    if STATE_FILE and STATE_FILE.exists():
        data=json.loads(STATE_FILE.read_text());JOBS.update(data['jobs']);SERVICES.update(data['services'])
        INSTALLED.clear();INSTALLED.update(data['installed']);INSTALLED_VERSIONS.update(data.get('installed_versions',{}));LOGS.update(data['logs'])
        DIRECTORIES.update(data.get('directories',[]));FILES.update({k:base64.b64decode(v) for k,v in data.get('files',{}).items()});BACKUPS.update(data.get('backups',{}))
        if 'processes' in data:PROCESSES[:]=data['processes']
        for job in JOBS.values():
            if not job['done']: job.update(done=True,exit=-1,unknown=True)
    else:
        LOGS['ssh.log']='2026-10-12 14:30 [info] SSH listening on port 22\n[supervisor 110] started pid=119 detail=0 time=1791804600\n2026-10-12 14:31 [warning] Child restarted\n[supervisor 110] exited pid=119 detail=137 time=1791804660\n[supervisor 110] started pid=120 detail=0 time=1791804661\n'
        LOGS['nginx.log']='2026-10-12 14:30 [info] Server ready\n2026-10-12 14:31 [error] Upstream connection refused\n2026-10-12 14:32 [warning] Upstream retry\n2026-10-12 14:33 [info] Upstream restored\n'
    server=ThreadingHTTPServer(('127.0.0.1',args.port),Handler)
    print(f'Demo: http://127.0.0.1:{server.server_port} — root password: demo',flush=True)
    server.serve_forever()
