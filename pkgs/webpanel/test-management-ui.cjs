#!/usr/bin/env node
// Browser integration checks against the isolated simulated demo API.
'use strict';
const assert=require('node:assert/strict');
const {spawn}=require('node:child_process');
const path=require('node:path');
const {chromium}=require('playwright');
(async()=>{
 const demo=spawn('python3',[path.join(__dirname,'demo.py'),'--port','0'],{stdio:['ignore','pipe','pipe']});
 let browser;
 try{
  // The server prints its assigned port after binding.
  const origin=await new Promise((resolve,reject)=>{
   let output='';const timer=setTimeout(()=>reject(Error('Demo startup timed out')),10000);
   demo.stdout.on('data',data=>{output+=data;const match=output.match(/http:\/\/127\.0\.0\.1:(\d+)/);if(match){clearTimeout(timer);resolve(match[0]);}});
   demo.once('exit',code=>{clearTimeout(timer);reject(Error('Demo exited: '+code));});
  });
  browser=await chromium.launch({headless:true});
  const page=await browser.newPage();const errors=[];
  page.on('pageerror',error=>errors.push(error.message));page.on('dialog',dialog=>dialog.accept());
  await page.goto(origin);await page.fill('#password','demo');await page.click('#login-form button');
  await page.waitForFunction(()=>!document.querySelector('#dashboard').hidden&&document.querySelector('#services-output').textContent.includes('ssh'));
  const idle=()=>page.waitForFunction(()=>!document.querySelector('#file-save').disabled&&!document.querySelector('#process-term').disabled);
  const tab=async name=>{await idle();await page.click(`[data-tab="${name}"]`);};
  const directory=async name=>{await page.fill('#files-path',name);await page.click('#files-form button');await page.waitForFunction(name=>document.querySelector('#files-status').textContent.startsWith(name+':'),name);};
  const open=async name=>{await page.selectOption('#file-entry',name);await page.click('#file-open');await page.waitForFunction(name=>document.querySelector('#file-current').textContent.includes('/'+name+' ·'),name);};
  await tab('processes');await page.waitForFunction(()=>document.querySelector('#process-choice').options.length===5);
  await page.selectOption('#processes-sort','pid');assert.equal(await page.locator('#process-choice option').first().getAttribute('value'),'1');
  await page.click('#processes-rows button[data-value="1"]');await page.click('#process-term');assert.match(await page.textContent('#notice'),/защищён/);
  await page.fill('#processes-filter','worker');assert.equal(await page.locator('#process-choice option').count(),1);
  await page.click('#processes-rows button[data-value="174"]');await page.click('#process-term');await page.waitForFunction(()=>document.querySelector('#processes-output').textContent.includes('не найдены'));await idle();
  await tab('disks');await page.waitForFunction(()=>document.querySelector('#disks-output').textContent.includes('доступно'));
  await page.fill('#disk-path','/home');await page.click('#disk-usage-form button');await page.waitForFunction(()=>document.querySelector('#disk-usage').textContent.includes('Итого:'));await idle();
  await tab('files');await page.waitForFunction(()=>document.querySelector('#file-entry').options.length>0);await open('hosts');
  const original=await page.inputValue('#file-editor');const edited=original+'10.0.2.20 test-server\n';
  await page.fill('#file-editor',edited);assert.match(await page.textContent('#file-status'),/несохранённые/);
  await page.click('#file-save');await page.waitForFunction(()=>document.querySelector('#file-status').textContent.startsWith('Сохранено.'));await idle();
  assert.equal(await page.inputValue('#file-editor'),edited);await page.click('#file-restore');
  await page.waitForFunction(original=>document.querySelector('#file-editor').value===original,original);await idle();
  await page.fill('#file-editor','invalid-address badhost\n');await page.click('#file-save');
  await page.waitForFunction(()=>document.querySelector('#task-output').textContent.includes('Неверный формат'));await idle();
  assert.equal(await page.inputValue('#file-editor'),'invalid-address badhost\n');assert.match(await page.textContent('#file-status'),/несохранённые/);
  const auth={Authorization:'Basic '+Buffer.from('root:demo').toString('base64')};
  const system=await (await page.request.get(origin+'/api/system',{headers:auth})).json();
  const getFile=async file=>await (await page.request.get(origin+'/api/file?path='+encodeURIComponent(file),{headers:auth})).json();
  let file=await getFile('/etc/hosts');assert.equal(file.content,original);
  const external=await page.request.post(origin+'/api/config?path=/etc/hosts',{headers:{...auth,'X-Webpanel-Token':system.csrf,'X-Webpanel-Revision':file.revision},data:original+'10.0.2.30 external\n'});assert.equal(external.status(),200);
  await page.fill('#file-editor',edited);await page.click('#file-save');await page.waitForFunction(()=>document.querySelector('#notice').textContent.includes('изменился'));await idle();
  assert.equal(await page.inputValue('#file-editor'),edited);
  await directory('/home/demo');await open('readme.txt');assert.equal(await page.locator('#file-editor-label').isVisible(),false);
  const binary=Buffer.from(Array.from({length:256},(_,i)=>i));
  await page.setInputFiles('#file-upload',{name:'binary.dat',mimeType:'application/octet-stream',buffer:binary});
  await page.click('#file-upload-form button');await page.waitForFunction(()=>Array.from(document.querySelector('#file-entry').options).some(o=>o.value==='binary.dat'));await idle();
  await page.fill('#files-filter','binary');assert.equal(await page.locator('#file-entry option').count(),1);
  await open('binary.dat');assert.match(await page.textContent('#file-output'),/Двоичный/);
  const downloaded=page.waitForEvent('download');await page.click('#file-download');const download=await downloaded;
  const stream=await download.createReadStream();const chunks=[];for await(const chunk of stream)chunks.push(chunk);assert.deepEqual(Buffer.concat(chunks),binary);
  await page.click('#file-upload-form button');await page.waitForFunction(()=>document.querySelector('#notice').textContent.includes('уже существует'));await idle();
  await page.fill('#files-filter','');
  await page.fill('#file-mkdir-name','new-directory');await page.click('#file-mkdir-form button');
  await page.waitForFunction(()=>Array.from(document.querySelector('#file-entry').options).some(o=>o.value==='new-directory'));await idle();
  await page.selectOption('#file-entry','new-directory');await page.click('#file-open');await page.waitForFunction(()=>document.querySelector('#files-status').textContent.startsWith('/home/demo/new-directory:'));
  await page.click('#files-up');await page.waitForFunction(()=>document.querySelector('#files-status').textContent.startsWith('/home/demo:'));
  await page.setViewportSize({width:390,height:844});
  for(const name of ['processes','disks','files']){await tab(name);assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true,'mobile overflow '+name);}
  assert.deepEqual(errors,[]);console.log('webpanel management browser: processes, disk scan, config save/restore/validation/conflict, binary upload/download, mkdir and mobile layout OK');
 }finally{if(browser)await browser.close();demo.kill('SIGTERM');}
})().catch(error=>{console.error(error);process.exitCode=1;});
