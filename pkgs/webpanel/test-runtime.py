#!/usr/bin/env python3
"""Explicit guest test: HTTPS proxy, authentication, CSRF and bounded commands."""
import base64,http.client,json,os,ssl,subprocess,sys,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tests'))
from guest_runtime import Guest
port=int(os.environ.get('NGINX_TLS_TEST_PORT','8443'))
password=os.environ.get('WEBPANEL_TEST_PASSWORD','cloud-test-pass')
g=Guest(); panel=None;nginx=None
try:
 g.run('/bin/mkdir',['-p','/usr/share/webpanel','/var/lib/webpanel','/var/log','/var/lib/nginx','/run'])
 g.write('/tmp/webpanel',(ROOT/'pkgs/.work/webpanel/payload/bin/webpanel').read_bytes())
 g.write('/tmp/nginx',(ROOT/'pkgs/.work/nginx/payload/bin/nginx').read_bytes())
 g.run('/bin/chmod',['755','/tmp/webpanel','/tmp/nginx'])
 for name in ['index.html','app.js','style.css']:g.write('/usr/share/webpanel/'+name,(ROOT/'pkgs/webpanel/files'/name).read_bytes())
 with tempfile.TemporaryDirectory(prefix='panel-tls-') as tmp:
  tmp=Path(tmp);cert=tmp/'cert.pem';key=tmp/'key.pem'
  subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','2','-subj','/CN=localhost','-addext','subjectAltName=IP:127.0.0.1','-keyout',str(key),'-out',str(cert)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
  g.write('/tmp/panel-cert.pem',cert.read_bytes());g.write('/tmp/panel-key.pem',key.read_bytes());g.run('/bin/chmod',['600','/tmp/panel-key.pem'])
  config='''user root; master_process off; daemon off; pid /run/panel-nginx-test.pid;
error_log /var/log/panel-nginx-test.log info;
events { use poll; worker_connections 128; accept_mutex off; }
http { access_log off; sendfile off; server { listen 8443 ssl;
ssl_certificate /tmp/panel-cert.pem; ssl_certificate_key /tmp/panel-key.pem;
location / { proxy_pass http://127.0.0.1:8088; } } }
'''
  g.write('/tmp/panel-nginx.conf',config.encode());g.run('/tmp/nginx',['-t','-c','/tmp/panel-nginx.conf'])
  panel=g.start('/tmp/webpanel');nginx=g.start('/tmp/nginx',['-c','/tmp/panel-nginx.conf'])
  context=ssl.create_default_context(cafile=str(cert))
  auth='Basic '+base64.b64encode(('root:'+password).encode()).decode()
  def request(path,method='GET',body=None,headers=None):
   c=http.client.HTTPSConnection('127.0.0.1',port,context=context,timeout=15)
   try:c.request(method,path,body=body,headers=headers or {});r=c.getresponse();return r.status,r.read()
   finally:c.close()
  for i in range(40):
   try:
    status,body=request('/')
    if status==200:break
   except OSError:pass
   time.sleep(.2)
  assert status==200 and 'Панель управления'.encode() in body,(status,body)
  assert request('/api/system')[0]==401
  assert request('/api/system',headers={'Authorization':'Basic '+base64.b64encode(b'root:invalid-password').decode()})[0]==401
  status,body=request('/api/system',headers={'Authorization':auth});assert status==200,(status,body)
  info=json.loads(body);assert info['uptime']>=0 and info['memory_total']>0 and len(info['csrf'])==64,info
  headers={'Authorization':auth,'Content-Type':'application/x-www-form-urlencoded'}
  assert request('/api/run','POST','operation=services',headers)[0]==403
  headers['X-Webpanel-Token']=info['csrf']
  assert request('/api/run','POST','operation=service&action=start&name=ssh%3Bid',headers)[0]==400
  assert request('/api/run','POST','operation=package&action=remove&name=kernel',headers)[0]==400
  assert request('/api/log?name=../../etc/shadow',headers={'Authorization':auth})[0]==400
  assert request('/../../etc/shadow')[0] in (400,404)
  def run(body):
   status,data=request('/api/run','POST',body,headers);assert status==200,(status,data)
   job=json.loads(data)['job']
   for i in range(80):
    status,data=request('/api/job?id='+str(job),headers={'Authorization':auth});assert status==200,(status,data)
    result=json.loads(data)
    if result['done']:assert result['exit']==0,result;return result['output']
    time.sleep(.2)
   raise AssertionError('panel command stalled')
  assert 'ssh' in run('operation=services')
  assert 'setup' in run('operation=packages&action=installed')
  assert 'vtnet' in run('operation=network')
  g.write('/var/log/webpanel-test.log','Привет\n\x1b[31mred\x1b[0m\n'.encode())
  status,data=request('/api/log?name=webpanel-test.log',headers={'Authorization':auth});assert status==200 and data=='Привет\nred\n'.encode(),(status,data)
  assert 'webpanel-test.log' in json.loads(request('/api/logs',headers={'Authorization':auth})[1])
  print('webpanel guest: verified HTTPS, login rejection/acceptance, system metrics, CSRF, injection/traversal rejection, protected packages, service/package/network listings, UTF-8 logs and ANSI filtering OK')
finally:
 for pid in [nginx,panel]:
  if pid:
   try:g.run('/bin/kill',['-TERM',str(pid)]);g.result(pid)
   except Exception:pass
 g.close()
