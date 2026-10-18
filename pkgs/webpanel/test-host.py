#!/usr/bin/env python3
"""Exercise the real native backend using temporary fixture paths, never host services."""
import base64
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from urllib.parse import quote, urlencode

ROOT=Path(__file__).resolve().parents[2]
AUTH='Basic '+base64.b64encode(b'root:host-test').decode()
with tempfile.TemporaryDirectory(prefix='webpanel-host-') as directory:
    root=Path(directory)
    for name in ('state','logs','services','run','bin'): (root/name).mkdir()
    (root/'services/ssh').write_text('# SSH service\nexec /bin/dropbear -F -E\n')
    (root/'services/ssh.disabled').touch()
    (root/'run/ssh').mkdir()
    (root/'run/ssh/status').write_text(f'{int(time.monotonic())} 42 43 running 1 0\n')
    (root/'logs/ssh.log').write_text('[supervisor 42] started pid=43 detail=0 time=1700000000\n[error] example failure\n[supervisor 42] exited pid=43 detail=137 time=1700000100\n')
    (root/'logs/plain.log').write_text('Привет\n\x1b[31mred\x1b[0m\n')
    for name in ('service','setup','ip'):
        script=root/'bin'/name
        script.write_text('''#!/bin/sh
printf '%s\\n' "$*"
case "$*" in
  "--yes upgrade slow") sleep 0.5; printf 'finished after restart\\n';;
  list) printf 'ssh running pid=43 enabled=no\\n';;
  info*) printf 'nano 8.7 [fixture] Text editor\\n  requires ncurses >= 6.5\\n';;
  --dry-run*) printf 'Plan: 1 package changes, 2 file operations, 1234 download bytes\\n';;
esac
''')
        script.chmod(0o755)
    file_root=root/'fs'
    for name in ('etc/services','etc/nginx','home/test','var/log','var/www','tmp','boot'):(file_root/name).mkdir(parents=True,exist_ok=True)
    (file_root/'etc/hosts').write_text('127.0.0.1 localhost\n')
    (file_root/'etc/resolv.conf').write_text('nameserver 1.1.1.1\n')
    (file_root/'etc/services/ssh').write_text('exec /bin/dropbear -F -E\n')
    (file_root/'etc/nginx/nginx.conf').write_text('events {}\nhttp {}\n')
    (file_root/'home/test/data.bin').write_bytes(bytes(range(256)))
    (file_root/'home/test/link').symlink_to(root/'services')
    (file_root/'home/test/fifo').touch()
    (file_root/'etc/nginx').chmod(0o755)
    nginx=root/'bin/nginx';nginx.write_text('#!/bin/sh\nif grep -q invalid "$3"; then echo bad config; exit 1; fi\necho syntax is ok\n');nginx.chmod(0o755)
    fixture_process=subprocess.Popen(['/bin/sleep','60'])
    os.environ['WEBPANEL_HOST_TEST_PID']=str(fixture_process.pid)
    harness=root/'main.c'
    harness.write_text('#define getspnam webpanel_test_getspnam\n#define syscall webpanel_test_syscall\n#include "'+str(ROOT/'pkgs/webpanel/main.c')+'"\nstruct spwd *webpanel_test_getspnam(const char *name) {\n static struct spwd record; static char hash[256];\n if(strcmp(name,"root"))return NULL;\n if(!*hash)snprintf(hash,sizeof hash,"%s",crypt("host-test","$6$webpanel-host$"));\n record.sp_pwdp=hash;return &record;\n}\n')
    with harness.open('a') as f:
        f.write('''
#include <stdarg.h>
long webpanel_test_syscall(long number,...) {
 if(number!=LUMINE_PROCESS_SNAPSHOT_V2){errno=ENOSYS;return -1;}
 va_list args;va_start(args,number);struct lumine_process_info_v2 *p=va_arg(args,struct lumine_process_info_v2 *);va_end(args);
 memset(p,0,3*sizeof *p);p[0].base.pid=1;strcpy(p[0].base.name,"init");
 p[1].base.pid=getpid();strcpy(p[1].base.name,"webpanel");
 p[2].base.pid=atol(getenv("WEBPANEL_HOST_TEST_PID"));strcpy(p[2].base.name,"/bin/sleep fixture");
 p[2].base.threads=1;p[2].resident_bytes=4096;p[2].cpu_us=1000000;return 3;
}
''')
    defines={'FILE_ROOT':file_root,'NGINX_BIN':nginx,'STATE_DIR':root/'state','LOG_DIR':root/'logs','SERVICE_CONF':root/'services',
             'SERVICE_RUN':root/'run','ASSETS':ROOT/'pkgs/webpanel/files',
             'SERVICE_BIN':root/'bin/service','SETUP_BIN':root/'bin/setup','IP_BIN':root/'bin/ip'}
    command=['gcc','-O2','-Wall','-Wextra','-Werror']
    command += [f'-DWEBPANEL_{key}="{value}"' for key,value in defines.items()]
    command += [str(harness),'-lcrypt','-o',str(root/'backend')]
    subprocess.run(command,check=True)
    with socket.socket() as s: s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    def request(path,method='GET',body=None,headers=None):
        c=http.client.HTTPConnection('127.0.0.1',port,timeout=5)
        try:
            c.request(method,path,body,{'Authorization':AUTH} if headers is None else headers);r=c.getresponse()
            return r.status,r.read()
        finally: c.close()
    def start():
        p=subprocess.Popen([str(root/'backend'),'--port',str(port)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        for _ in range(50):
            try:
                if request('/')[0]==200:return p
            except OSError: pass
            if p.poll() is not None: raise AssertionError(p.stderr.read().decode())
            time.sleep(.03)
        p.terminate();p.wait();raise AssertionError('backend did not start')
    def run(values):
        body='&'.join(f'{k}={v}' for k,v in values.items())
        status,data=request('/api/run','POST',body,{'Authorization':AUTH,'X-Webpanel-Token':token,'Content-Type':'application/x-www-form-urlencoded'})
        assert status==200,(status,data)
        job=json.loads(data)['job']
        for _ in range(100):
            status,data=request('/api/job?id='+str(job));assert status==200,(status,data)
            result=json.loads(data)
            if result['done']: return job,result
            time.sleep(.03)
        raise AssertionError('job stalled')
    process=start()
    try:
        assert request('/api/history',headers={})[0]==401
        assert request('/api/service?name=ssh',headers={})[0]==401
        assert request('/api/system',headers={'Authorization':'Basic '+base64.b64encode(b'root:wrong').decode()})[0]==401
        token=json.loads(request('/api/system')[1])['csrf']
        assert request('/api/run','POST','operation=package-plan&action=install&name=nano')[0]==403
        data=json.loads(request('/api/service?name=ssh')[1])
        assert data['pid']==43 and data['supervisor']==42 and not data['autostart'],data
        assert data['config'].startswith('# SSH') and 'detail=137' in data['events'] and 'example failure' not in data['events'],data
        assert data['memory_bytes']==-1 and data['cpu_seconds']==-1,data
        assert request('/api/service?name=../shadow')[0]==400
        (root/'services/secret').symlink_to(root/'logs/plain.log')
        assert request('/api/service?name=secret')[0]==404
        _,result=run({'operation':'package-info','name':'nano'});assert 'requires ncurses' in result['output'],result
        _,result=run({'operation':'packages','action':'catalog'});assert result['output']=='search\n',result
        plan,result=run({'operation':'package-plan','action':'install','name':'nano'})
        assert result['output'].startswith('--dry-run install nano\n') and '1234 download bytes' in result['output'],result
        _,result=run({'operation':'package-updates'});assert '--dry-run upgrade' in result['output'],result
        _,result=run({'operation':'package','action':'install','name':'nano'});assert result['output']=='--yes install nano\n',result
        headers={'Authorization':AUTH,'X-Webpanel-Token':token,'Content-Type':'application/x-www-form-urlencoded'}
        for body in ('operation=package-plan&action=remove&name=kernel','operation=package-info&name=nano%3Bid','operation=package-plan&action=install&name='+'a'*70,'operation=service&action=start&name=nginx','operation=package&action=upgrade&name=--yes'):
            assert request('/api/run','POST',body,headers)[0]==400,body
        assert request('/api/log?name=plain.log')[1]=='Привет\nred\n'.encode()
        entries=json.loads(request('/api/history')[1]);assert len(entries)==5,entries
        entry=next(item for item in entries if item['id']==plan)
        assert entry['user']=='root' and entry['operation']=='package-plan' and entry['name']=='nano' and entry['created']>0 and entry['finished']>=entry['created'] and entry['exit']==0,entry
        process.terminate();process.wait(timeout=5)
        process=start()
        assert len(json.loads(request('/api/history')[1]))==5
        assert '--dry-run install nano' in json.loads(request('/api/job?id='+str(plan))[1])['output']
        token=json.loads(request('/api/system')[1])['csrf']
        status,data=request('/api/run','POST','operation=package&action=upgrade&name=slow',{'Authorization':AUTH,'X-Webpanel-Token':token,'Content-Type':'application/x-www-form-urlencoded'})
        assert status==200,(status,data);interrupted=json.loads(data)['job']
        process.terminate();process.wait(timeout=5);process=start()
        entry=next(item for item in json.loads(request('/api/history')[1]) if item['id']==interrupted)
        assert entry['unknown'] and entry['done'] and entry['exit']==-1,entry
        time.sleep(.6)
        token=json.loads(request('/api/system')[1])['csrf']
        # Exhaust the ring and verify that expired output and metadata are removed together.
        for _ in range(34):run({'operation':'network'})
        entries=json.loads(request('/api/history')[1]);assert len(entries)==32
        assert entries[0]['id']>entries[-1]['id'] and request('/api/job?id='+str(plan))[0]==404
        assert len(list((root/'state').glob('job-*.log')))==32
        rows=json.loads(request('/api/processes')[1]);assert len(rows)==3 and rows[0]['protected'] and rows[1]['protected'] and not rows[2]['protected'],rows
        disks=json.loads(request('/api/disks')[1]);assert disks and disks[0]['total']>0,disks
        assert request('/api/files?path=%2Fhome%2Ftest',headers={})[0]==401
        entries=json.loads(request('/api/files?path=%2Fhome%2Ftest')[1])['entries'];assert any(e['type']=='symlink' for e in entries)
        for path in ('/home/test/link/ssh','/home/test/../test/data.bin','/home//test/data.bin','/dev/zero'):
            assert request('/api/file?path='+quote(path,safe=''))[0] in (400,404),path
        assert request('/api/download?path=%2Fhome%2Ftest%2Fdata.bin')[1]==bytes(range(256))
        assert not json.loads(request('/api/file?path=%2Fhome%2Ftest%2Fdata.bin')[1])['text']
        def post_path(kind,path,body=b'',extra=None):
            h={'Authorization':AUTH,'X-Webpanel-Token':token,'Content-Type':'application/octet-stream',**(extra or {})}
            return request('/api/'+kind+'?path='+quote(path,safe=''),'POST',body,h)
        assert request('/api/upload?path=%2Fhome%2Ftest%2Fnew','POST',b'hello')[0]==403
        status,data=post_path('upload','/home/test/new.bin',bytes(range(256)));assert status==200,(status,data)
        assert (file_root/'home/test/new.bin').read_bytes()==bytes(range(256))
        assert post_path('upload','/home/test/new.bin',b'replace')[0]==409
        assert post_path('upload','/etc/new.conf',b'replace')[0]==400
        assert post_path('upload','/home/test/link/new',b'replace')[0]==404
        assert post_path('upload','/home/test/large',b'x'*1048577)[0]==413
        assert post_path('mkdir','/home/test/new-folder')[0]==200
        assert (file_root/'home/test/new-folder').is_dir()
        hosts=json.loads(request('/api/file?path=%2Fetc%2Fhosts')[1]);assert hosts['editable']
        status,data=post_path('config','/etc/hosts',b'127.0.0.1 localhost server\n',{'X-Webpanel-Revision':hosts['revision']})
        saved=json.loads(data);assert status==200 and saved['saved'],(status,data)
        backup=root/'state/backups'/f"config-{saved['backup']}.bak";assert backup.read_text()=='127.0.0.1 localhost\n'
        assert (file_root/'etc/hosts').read_text()=='127.0.0.1 localhost server\n'
        assert post_path('config','/etc/hosts',b'127.0.0.1 overwrite\n',{'X-Webpanel-Revision':hosts['revision']})[0]==409
        status,data=post_path('config','/etc/hosts',b'',{'X-Webpanel-Revision':saved['revision'],'X-Webpanel-Restore':str(saved['backup'])});assert json.loads(data)['saved'],data
        assert (file_root/'etc/hosts').read_text()=='127.0.0.1 localhost\n'
        service=json.loads(request('/api/file?path=%2Fetc%2Fservices%2Fssh')[1])
        status,data=post_path('config','/etc/services/ssh',b'exec /bin/foo; rm -rf /\n',{'X-Webpanel-Revision':service['revision']});assert not json.loads(data)['saved'],data
        assert (file_root/'etc/services/ssh').read_text()=='exec /bin/dropbear -F -E\n'
        config=json.loads(request('/api/file?path=%2Fetc%2Fnginx%2Fnginx.conf')[1])
        status,data=post_path('config','/etc/nginx/nginx.conf',b'invalid config\n',{'X-Webpanel-Revision':config['revision']});assert not json.loads(data)['saved'],data
        status,data=post_path('config','/etc/nginx/nginx.conf',b'events {}\nhttp { server {} }\n',{'X-Webpanel-Revision':config['revision']});assert json.loads(data)['saved'],data
        assert post_path('config','/etc/shadow',b'ignored')[0]==400
        status,data=post_path('usage','/home/test');assert status==200,(status,data)
        job=json.loads(data)['job']
        for _ in range(100):
            result=json.loads(request('/api/job?id='+str(job))[1])
            if result['done']:break
            time.sleep(.03)
        assert result['done'] and result['exit']==0 and 'data.bin' in result['output'] and '\tlink' not in result['output'],result
        headers['X-Webpanel-Token']=token
        assert request('/api/process','POST',urlencode({'pid':1,'name':'init','cpu_us':0,'signal':'TERM'}),headers)[0]==400
        target=rows[2]
        values={'pid':target['pid'],'name':'different','cpu_us':target['cpu_us'],'signal':'TERM'}
        assert request('/api/process','POST',urlencode(values),headers)[0]==409
        values['name']=target['name'];status,data=request('/api/process','POST',urlencode(values),headers);assert status==200,(status,data)
        fixture_process.wait(timeout=5);assert fixture_process.returncode==-15
        print('webpanel native host: file/process/disk endpoints, binary upload/download, path and symlink guards, config conflict detection, backup/restore and format validation OK')
        print('webpanel native host: auth/CSRF, service details/events, symlink/traversal rejection, package info and dry-run, bounded durable history and restart recovery OK')
    finally:
        process.terminate();process.wait(timeout=5)
        if fixture_process.poll() is None:fixture_process.terminate();fixture_process.wait(timeout=5)
