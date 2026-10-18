#!/usr/bin/env node
// Host-side browser regression test. Requires Playwright and its Chromium browser.
'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const http=require('node:http');
const {chromium}=require('playwright');
const assets=path.join(__dirname,'files');
const server=http.createServer((req,res)=>{
 const name=req.url.split('?')[0]==='/'?'index.html':req.url.slice(1).split(/[?#]/)[0];
 if(!['index.html','app.js','style.css'].includes(name)){res.writeHead(404).end();return;}
 res.setHeader('Content-Security-Policy',"default-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
 res.setHeader('Content-Type',name.endsWith('.js')?'text/javascript':name.endsWith('.css')?'text/css':'text/html');
 res.end(fs.readFileSync(path.join(assets,name)));
});
(async()=>{
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 const browser=await chromium.launch({headless:true});
 try{
  const page=await browser.newPage();
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  let sequence=0,posts=[],jobs=new Map(),hold=false,disconnect=0,unauthorized=false;
  let expandedCatalog=false;
  let logReads=[],delayLog=null,serviceVersion=0,acceptConfirm=true;
  const logContents={'a.log':'latest a content','z.log':'latest z content'};
  const reply=(route,data,status=200)=>route.fulfill({status,contentType:'application/json',body:JSON.stringify(data)});
  await page.route('**/api/**',async route=>{
   const req=route.request(),url=new URL(req.url());
   if(unauthorized)return route.fulfill({status:401,body:'Authentication required'});
   assert.match(req.headers().authorization,/^Basic /);
   if(url.pathname==='/api/system')return reply(route,{csrf:'test-token',hostname:'test-server',release:'test',uptime:3660,cpu_core_percent:12,memory_used:1048576,memory_total:2097152});
   if(url.pathname==='/api/run'){
    assert.equal(req.headers()['x-webpanel-token'],'test-token');
    const values=Object.fromEntries(new URLSearchParams(req.postData()));posts.push(values);
    const job=++sequence;jobs.set(job,{values,created:Math.floor(Date.now()/1000)});return reply(route,{job});
   }
   if(url.pathname==='/api/service')return reply(route,{state:'running',pid:120,supervisor:110,desired:true,autostart:true,retry:0,heartbeat_age:1,memory_bytes:1048576,cpu_seconds:2.5,config:'exec /bin/dropbear -F -E',events:'[supervisor 110] exited pid=119 detail=137',config_truncated:false});
   if(url.pathname==='/api/history')return reply(route,[...jobs.entries()].reverse().map(([id,job])=>({id,user:'root',created:job.created,finished:hold?0:job.created,done:!hold,exit:hold?-1:0,unknown:false,operation:job.values.operation,action:job.values.action||'',name:job.values.name||''})));
   if(url.pathname==='/api/job'){
    if(disconnect){disconnect--;return route.abort('failed');}
    const job=jobs.get(Number(url.searchParams.get('id')));assert(job);
    const {values}=job;let output='command output';
    if(values.operation==='services')output=`ssh running pid=${++serviceVersion}\nnginx stopped (no supervisor)\n`;
    if(values.operation==='packages')output=values.action==='installed'?'setup 1 local\nwebpanel 1 local\n':'nano 1 [main]\nsetup 1 [main]\n';
    if(values.operation==='packages'&&expandedCatalog)output=Array.from({length:45},(_,i)=>`package-${String(i).padStart(2,'0')} 1 [main] Test package`).join('\n');
    if(values.operation==='package-info')output='nano 8.7 [main] Text editor\n  requires ncurses >= 6.5';
    if(values.operation==='package-plan'||values.operation==='package-updates')output='Plan: 1 package changes, 2 file operations, 1234 download bytes';
    if(values.operation==='network')output='vtnet0 10.0.2.15';
    return reply(route,{done:!hold,exit:hold?-1:0,output});
   }
   if(url.pathname==='/api/logs')return reply(route,['z.log','a.log']);
   if(url.pathname==='/api/log'){
    const name=url.searchParams.get('name');logReads.push(name);
    if(name==='a.log'&&delayLog)await delayLog;
    return route.fulfill({body:logContents[name]});
   }
   throw new Error(`Unexpected API ${url}`);
  });
  page.on('dialog',d=>acceptConfirm?d.accept():d.dismiss());
  const base=`http://127.0.0.1:${server.address().port}`;
  const login=async()=>{await page.fill('#password','пароль');await page.click('#login-form button');await page.waitForFunction(()=>!document.getElementById('dashboard').hidden);};
  const idle=()=>page.waitForFunction(()=>document.getElementById('dashboard').getAttribute('aria-busy')==='false');
  const textIs=(id,text)=>page.waitForFunction(({id,text})=>document.getElementById(id).textContent.includes(text),{id,text});
  await page.goto(base);await login();await idle();
  assert.equal(await page.locator('#password').inputValue(),'');
  assert.deepEqual(await page.locator('#service-names option').evaluateAll(nodes=>nodes.map(n=>n.value)),['nginx','ssh']);
  await page.click('[data-tab="packages"]');await idle();await textIs('packages-output','nano');
  await page.click('#packages-installed');await idle();await textIs('packages-output','webpanel');
  await page.selectOption('#package-name','setup');await page.selectOption('#package-action','remove');
  const count=posts.length;await page.click('#package-form button');assert.equal(posts.length,count);await textIs('notice','защищён');
  await page.click('[data-tab="services"]');await page.click('#services-rows button[data-value="ssh"]');assert.equal(await page.inputValue('#service-name'),'ssh');
  const previous=serviceVersion;await page.click('#service-form button');await textIs('task-output','успешно');await idle();
  assert(serviceVersion>previous);assert.equal(posts.at(-1).operation,'services');
  await page.click('[data-tab="logs"]');await textIs('logs-output','latest a');
  let release;delayLog=new Promise(resolve=>{release=resolve;});
  await page.click('#logs-refresh');await page.waitForFunction(()=>document.getElementById('log-name').value==='a.log');
  await page.selectOption('#log-name','z.log');await textIs('logs-output','latest z');
  release();delayLog=null;await page.waitForTimeout(100);assert.equal(await page.locator('#logs-output').textContent(),'latest z content');
  await page.click('[data-tab="network"]');await idle();await textIs('network-output','vtnet');
  await page.click('[data-tab="services"]');await idle();
  hold=true;await page.click('#service-form button');await page.waitForFunction(()=>!!sessionStorage.getItem('webpanel-job'));
  await page.click('[data-tab="logs"]');await textIs('logs-output','latest z');
  assert.equal(await page.locator('#logs-refresh').isDisabled(),false);
  assert.equal(await page.locator('#service-form button').isDisabled(),true);
  const beforeReload=posts.length;await page.reload();await login();
  assert.equal(posts.length,beforeReload);assert.equal(await page.locator('#logs').isVisible(),true);
  disconnect=3;await textIs('notice','повторяем проверку');hold=false;await textIs('task-output','успешно');await idle();
  assert.equal(await page.evaluate(()=>sessionStorage.getItem('webpanel-job')),null);
  await page.click('[data-tab="services"]');await idle();await page.selectOption('#service-name','ssh');hold=true;await page.click('#service-form button');
  await page.waitForFunction(()=>!!sessionStorage.getItem('webpanel-job'));
  await page.click('#logout');await page.waitForTimeout(900);
  assert.equal(await page.locator('#dashboard').isVisible(),false);
  await textIs('notice','Вы вышли');hold=false;await login();await idle();
  unauthorized=true;await page.evaluate(()=>document.getElementById('services-refresh').click());await page.waitForFunction(()=>!document.getElementById('login').hidden);
  await textIs('notice','Войдите снова');assert.equal(await page.evaluate(()=>authorization),'');
  unauthorized=false;
  await page.evaluate(()=>{sessionStorage.setItem('webpanel-tab','services');history.replaceState(null,'','#services');sessionStorage.setItem('webpanel-job',JSON.stringify({job:1,values:{operation:'service',name:'ssh',action:'start'},target:'task-output',serverId:'previous-backend'}));});
  const beforeRestart=posts.length;await login();await idle();
  await textIs('task-output','перезапущен');
  assert.equal(posts.length,beforeRestart+1);assert.equal(posts.at(-1).operation,'services');
  await page.focus('[data-tab="services"]');await page.keyboard.press('ArrowRight');await idle();
  assert.equal(await page.locator('#packages').isVisible(),true);
  assert.equal(posts.at(-1).action,'installed');
  await page.click('#packages-list');await idle();
  await page.click('#packages-rows button[data-value="nano"]');assert.equal(await page.inputValue('#package-name'),'nano');
  await page.selectOption('#package-action','install');
  await page.click('#package-info');await textIs('package-details','requires ncurses');await idle();
  const beforeInstall=posts.length;
  await page.click('#package-form button');await textIs('task-output','успешно');await idle();
  assert.deepEqual(posts.slice(beforeInstall).map(v=>v.operation),['package-plan','package','packages']);
  assert.equal(posts[beforeInstall+1].name,'nano');
  acceptConfirm=false;const beforeCancel=posts.length;
  await page.click('#package-form button');await idle();await page.waitForTimeout(100);
  assert.deepEqual(posts.slice(beforeCancel).map(v=>v.operation),['package-plan']);acceptConfirm=true;
  const beforeRace=posts.length;hold=true;
  await page.click('#package-form button');
  await page.waitForFunction(()=>JSON.parse(sessionStorage.getItem('webpanel-job')||'null')?.values.operation==='package-plan');
  await page.selectOption('#package-name','setup');await page.evaluate(()=>loaded.delete('network'));
  await page.click('[data-tab="network"]');hold=false;await idle();
  assert.deepEqual(posts.slice(beforeRace).map(v=>v.operation),['package-plan','package','network']);
  assert.equal(posts[beforeRace+1].name,'nano');
  await page.click('[data-tab="packages"]');await idle();
  await page.click('#packages-installed');await idle();
  await page.fill('#packages-filter','webpanel');
  assert.equal(await page.locator('#packages-output').textContent(),'webpanel 1 local');
  await page.click('[data-tab="services"]');await idle();await page.selectOption('#service-name','ssh');await page.click('#service-inspect');
  await textIs('service-details','120');await textIs('service-config','dropbear');await textIs('service-events','137');
  await page.fill('#services-filter','nginx');assert.match(await page.locator('#services-output').textContent(),/^nginx/);
  assert(!(await page.locator('#services-output').textContent()).includes('ssh'));
  await page.click('[data-tab="logs"]');
  logContents['z.log']='2026-10-12 14:30 [error] failure\n2026-10-12 14:31 [warning] retry\n2026-10-12 14:32 [info] ready';
  await page.click('#logs-refresh');await textIs('logs-output','failure');
  await page.selectOption('#logs-level','error');assert.equal(await page.locator('#logs-output').textContent(),'2026-10-12 14:30 [error] failure');
  await page.fill('#logs-filter','absent');await textIs('logs-output','Нет строк');
  await page.fill('#logs-filter','');await page.selectOption('#logs-level','');await page.fill('#logs-time','14:31');
  assert.equal(await page.locator('#logs-output').textContent(),'2026-10-12 14:31 [warning] retry');
  await page.fill('#logs-time','');await page.click('#logs-live');
  logContents['z.log']+='\n2026-10-12 14:33 [info] live update';await textIs('logs-output','live update');
  await page.click('#logs-live');const readsAtPause=logReads.length;await page.waitForTimeout(2200);assert.equal(logReads.length,readsAtPause);
  const downloadEvent=page.waitForEvent('download');await page.click('#logs-download');assert.equal((await downloadEvent).suggestedFilename(),'z.log');
  await page.click('#history-refresh');await textIs('history-output','root');
  assert((await page.locator('#history-job option').count())>1);
  const historyDownload=page.waitForEvent('download');await page.click('#history-download');assert.match((await historyDownload).suggestedFilename(),/^operation-\d+\.log$/);
  expandedCatalog=true;await page.click('[data-tab="packages"]');await page.fill('#packages-filter','');await page.click('#packages-list');await idle();
  assert.equal(await page.locator('#packages-rows tr').count(),20);
  await page.click('#packages-next');assert.match(await page.textContent('#packages-page'),/21–40 из 45/);
  await page.click('#packages-rows button[data-value="package-23"]');assert.equal(await page.inputValue('#package-name'),'package-23');
  await page.fill('#packages-filter','package-44');assert.equal(await page.locator('#packages-rows tr').count(),1);
  assert.equal(await page.inputValue('#package-name'),'package-23');assert.match(await page.textContent('#packages-selection'),/скрыто текущим фильтром/);
  await page.focus('#packages-rows button');await page.keyboard.press('Enter');assert.equal(await page.inputValue('#package-name'),'package-44');
  await page.fill('#packages-filter','no-such-package');assert.equal(await page.locator('#packages-rows tr').count(),0);assert.match(await page.textContent('#packages-page'),/Нет совпадений/);
  await page.fill('#packages-filter','');assert.equal(await page.inputValue('#package-name'),'package-44');
  await page.setViewportSize({width:390,height:844});assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
  assert.deepEqual(errors,[]);
  console.log('webpanel UI: clickable selectors, selection preservation, pagination, keyboard selection, protected actions, automatic refresh, log selection/races, tab navigation during jobs, reload recovery, network retry, logout cancellation and expired authentication, backend restart keyboard navigation, package plan/confirmation/cancellation, service inspection, list/log filters, live log pause and downloads OK');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;}).finally(()=>server.close());
