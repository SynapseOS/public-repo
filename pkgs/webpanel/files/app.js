'use strict';
const $ = id => document.getElementById(id);
const tabs = [ 'services', 'packages', 'logs', 'network', 'processes', 'disks', 'files' ];
const protectedPackages = new Set([
	'kernel', 'init', 'sh', 'sbase', 'ubase', 'setup', 'service', 'nginx', 'webpanel', 'dropbear'
]);
let authorization = '', csrf = '', serverId = '', busy = false, timer = null, session = 0,
    loginPending = false;
let selectedTab = 'services', packageView = '', pendingRead = null, metricsPending = false,
    logRevision = 0;
const controllers = new Set( );
const loaded = new Set( );
const pickerPages = {
	services : 0,
	packages : 0,
	processes : 0
};
const pickerRows = {
	services : [],
	packages : [],
	processes : []
};
let processRows = [], processTimer = null, processPending = false, processRevision = 0,
    filesRevision = 0, fileRevision = 0, diskRevision = 0;
let directoryData = null, openFile = null, backupId = null;

let serviceRevision = 0, historyRevision = 0, historyReadRevision = 0, historyItems = [],
    historyText = '';
let listText = { services : '', packages : '' }, logText = '', logFile = '', logUpdated = '',
    logTimer = null, logLive = false, logPending = 0;
const operationLabels = {
	services : 'Список служб',
	service : 'Управление службой',
	packages : 'Список пакетов',
	'package-info' : 'Описание пакета',
	'package-plan' : 'План изменения пакета',
	'package-updates' : 'Проверка обновлений',
	package : 'Управление пакетом',
	network : 'Сетевые интерфейсы',
	process : 'Завершение процесса',
	'disk-usage' : 'Размеры каталогов',
	file : 'Работа с файлами',
	config : 'Конфигурация'
};

function notice(text) { $('notice').textContent = text; }

function remember(key, value) {
	try {
		if (value === null)
			sessionStorage.removeItem(key);
		else
			sessionStorage.setItem(key, value);
	} catch {
	}
}

function recalled(key) {
	try {
		return sessionStorage.getItem(key);
	} catch {
		return null;
	}
}

function current(id) { return id === session && !!authorization; }

function controls( ) {
	document.querySelectorAll('#dashboard button').forEach(b => {
		b.disabled = (busy && !b.hasAttribute('data-tab') && !b.hasAttribute('data-read')) ||
		             b.dataset.unavailable === 'true';
	});
	$('dashboard').setAttribute('aria-busy', String(busy));
	$('login-form').querySelector('button').disabled = loginPending;
}

function endSession(message) {
	session++;
	authorization = '';
	csrf = '';
	serverId = '';
	busy = false;
	loginPending = false;
	pendingRead = null;
	metricsPending = false;
	clearInterval(timer);
	timer = null;
	clearTimeout(logTimer);
	logTimer = null;
	logLive = false;
	logPending = 0;
	serviceRevision++;
	historyRevision++;
	historyReadRevision++;
	historyItems = [];
	historyText = '';
	listText = { services : '', packages : '' };
	logText = '';
	logFile = '';
	logUpdated = '';
	['service', 'package'].forEach(
	    kind => $(kind + '-name').replaceChildren(new Option('Выберите из списка', '')));
	for (const kind of Object.keys(pickerRows)) {
		pickerRows[kind] = [];
		pickerPages[kind] = 0;
		renderPicker(kind);
	}
	$('logs-live').textContent = 'Включить автообновление';
	$('logs-live').setAttribute('aria-pressed', 'false');
	logRevision++;
	loaded.clear( );
	resetManagement( );
	controllers.forEach(c => c.abort( ));
	controllers.clear( );
	$('dashboard').hidden = true;
	$('logout').hidden = true;
	$('login').hidden = false;
	controls( );
	notice(message);
	$('password').focus( );
}

async function request(path, options = { }) {
	const id = session, controller = new AbortController( );
	controllers.add(controller);
	const timeout = setTimeout(( ) => controller.abort( ), 15000);
	try {
		const r = await fetch(path, {
			...options,
			headers : { Authorization : authorization, ...options.headers },
			signal : controller.signal,
			cache : 'no-store'
		});
		// Read the body while the timeout and session cancellation still apply.
		const bytes = await r.arrayBuffer( ), body = new TextDecoder( ).decode(bytes);
		if (!current(id))
			throw new Error('Сеанс завершён.');
		if (!r.ok) {
			if (r.status === 401) {
				endSession('Сеанс завершён. Войдите снова с текущим паролем root.');
				throw new Error('Сеанс завершён.');
			}
			const messages = {
				403 : 'Токен доступа устарел. Выйдите и войдите снова.',
				409 : 'На сервере уже выполняется операция. Дождитесь её завершения.',
				404 : 'Запрошенные данные больше недоступны.'
			};
			const error = new Error(r.status === 409
			                            ? (body.trim( ) || messages[409])
			                            : messages[r.status] || body.trim( ) || `HTTP ${r.status}`);
			error.status = r.status;
			throw error;
		}
		return {
			json : async ( ) => JSON.parse(body),
			text : async ( ) => body,
			bytes : async ( ) => bytes
		};
	} catch (e) {
		if (e.name === 'AbortError')
			throw new Error('Сервер не ответил вовремя. Проверьте соединение.');
		if (e instanceof TypeError)
			throw new Error('Нет связи с сервером. Проверьте соединение.');
		throw e;
	} finally {
		clearTimeout(timeout);
		controllers.delete(controller);
	}
}

async function metrics( ) {
	if (metricsPending || !authorization)
		return;
	const id = session;
	metricsPending = true;
	try {
		const data = await (await request('/api/system')).json( );
		// Store a backend fingerprint, never the CSRF token, to detect restarts on resume.
		const digest = await crypto.subtle.digest('SHA-256', new TextEncoder( ).encode(data.csrf));
		if (!current(id))
			return;
		csrf = data.csrf;
		serverId =
		    Array.from(new Uint8Array(digest), b => b.toString(16).padStart(2, '0')).join('');
		$('cpu').textContent =
		    data.cpu_core_percent < 0 ? 'Недоступно' : data.cpu_core_percent.toFixed(1) + '%';
		$('hostname').textContent = data.hostname;
		$('release').textContent = data.release;
		const hours = Math.floor(data.uptime / 3600);
		$('uptime').textContent = `${hours} ч ${Math.floor(data.uptime % 3600 / 60)} мин`;
		const mib = x => (x / 1048576).toFixed(0);
		$('memory').textContent = `${mib(data.memory_used)} / ${mib(data.memory_total)} МиБ`;
	} finally {
		if (id === session)
			metricsPending = false;
	}
}

function output(target, text) {
	const el = $(target), atEnd = el.scrollTop + el.clientHeight >= el.scrollHeight - 24;
	el.textContent = text;
	if (atEnd)
		el.scrollTop = el.scrollHeight;
}

function suggestions(target, text) {
	const names = [
		...new Set(text.split('\n')
		               .map(line => line.trim( ).split(/\s+/)[0])
		               .filter(name => /^[a-zA-Z0-9_][a-zA-Z0-9_-]{0,62}$/.test(name)))
	].sort( );
	$(target).replaceChildren(...names.map(name => {
		const o = document.createElement('option');
		o.value = name;
		return o;
	}));
}

function saveJob(job, values, target) {
	remember('webpanel-job', JSON.stringify({ job, values, target, serverId }));
}

function savedJob( ) {
	try {
		const data = JSON.parse(recalled('webpanel-job'));
		if (data && data.serverId !== serverId) {
			output(
			    'task-output',
			    'Backend панели перезапущен. Предыдущий результат недоступен; проверьте состояние службы или пакета перед повтором.');
			remember('webpanel-job', null);
			return null;
		}
		if (data && Number.isInteger(data.job) && data.job > 0 &&
		    [
			    'task-output', 'services-output', 'packages-output', 'network-output',
			    'package-details', 'package-plan', 'disk-usage'
		    ].includes(data.target) &&
		    Object.keys(operationLabels).includes(data.values?.operation))
			return data;
	} catch {
	}
	remember('webpanel-job', null);
	return null;
}

const pause = ms => new Promise(resolve => setTimeout(resolve, ms));

async function run(values, target = 'task-output', resume = null, drain = true, submission = null) {
	if (busy || !authorization)
		return false;
	const id = session;
	busy = true;
	controls( );
	notice('');
	let success = false, job = resume, result = null, extra = { };
	output(target, resume ? 'Восстанавливаем состояние операции…' : 'Выполняется…');
	try {
		if (!job) {
			// A POST is never retried: a lost response may still mean the command started.
			const route =
			    submission?.path || (values.operation === 'process' ? '/api/process'
			                         : values.operation === 'disk-usage'
			                             ? '/api/usage?path=' + encodeURIComponent(values.path)
			                             : '/api/run');
			const data =
			    await (await request(route, {
				    method : 'POST',
				    headers : {
					    'Content-Type' : submission?.type || 'application/x-www-form-urlencoded',
					    'X-Webpanel-Token' : csrf,
					    ...submission?.headers
				    },
				    body : submission?.body ?? new URLSearchParams(values)
			    })).json( );
			extra = data;
			if (!current(id))
				return false;
			job = data.job;
			saveJob(job, values, target);
			void refreshHistory( );
		}
		while (current(id)) {
			let data;
			try {
				data = await (await request(`/api/job?id=${job}`)).json( );
			} catch (e) {
				if (!current(id))
					return false;
				if (e.status) {
					remember('webpanel-job', null);
					throw e;
				}
				notice(`${e.message} Операция может продолжаться; повторяем проверку.`);
				await pause(2000);
				continue;
			}
			if (!current(id))
				return false;
			notice('');
			if (target === 'services-output' || target === 'packages-output') {
				const kind = target === 'services-output' ? 'services' : 'packages';
				listText[kind] = data.output || '';
				renderList(kind, data.done);
			} else
				output(target, data.output ||
				                   (data.done ? 'Команда завершена без вывода.' : 'Выполняется…'));
			if (data.done) {
				remember('webpanel-job', null);
				success = data.exit === 0;
				result = data;
				if (!success)
					notice(`Операция завершилась с кодом ${
						data.exit}. Подробности — в выводе команды.`);
				else if (target === 'task-output')
					output(target, `Операция успешно завершена.\n${data.output || ''}`);
				if (success && values.operation === 'services') {
					loaded.add('services');
					suggestions('service-names', data.output || '');
				}
				if (success && values.operation === 'packages') {
					loaded.add('packages');
					suggestions('package-names', data.output || '');
				}
				if (success && values.operation === 'network')
					loaded.add('network');
				break;
			}
			await pause(750);
		}
	} catch (e) {
		if (current(id))
			notice(
			    job || e.status
			        ? e.message
			        : `${e.message} Если запрос дошёл до сервера, команда могла запуститься. Проверьте состояние перед повтором.`);
	} finally {
		if (current(id)) {
			busy = false;
			controls( );
			if (success && [ 'service', 'package' ].includes(values.operation)) {
				const tab = values.operation === 'service' ? 'services' : 'packages';
				loaded.delete(tab);
				if (selectedTab === tab)
					pendingRead = tab;
				if (values.operation === 'service' && $('service-name').value === values.name)
					void inspectService( );
			}
			void refreshHistory( );
			if (drain)
				flushRead( );
		}
	}
	return success ? {...extra, ...result } : null;
}

function flushRead( ) {
	const next = pendingRead;
	pendingRead = null;
	if (next && next === selectedTab)
		refreshTab(next);
}

function refreshTab(tab) {
	if (!authorization)
		return;
	if ([ 'processes', 'disks', 'files' ].includes(tab)) {
		void refreshManagement(tab);
		return;
	}
	if (tab === 'logs') {
		const id = session;
		void loadLogs( ).catch(e => {
			if (current(id))
				notice(e.message);
		});
		return;
	}
	if (busy) {
		pendingRead = tab;
		return;
	}
	if (tab === 'services')
		void run({ operation : 'services' }, 'services-output');
	if (tab === 'packages')
		void run({ operation : 'packages', action : packageView || 'catalog' }, 'packages-output');
	if (tab === 'network')
		void run({ operation : 'network' }, 'network-output');
}

function selectTab(tab, load = true) {
	if (!tabs.includes(tab))
		tab = 'services';
	selectedTab = tab;
	remember('webpanel-tab', tab);
	document.querySelectorAll('[data-tab]')
	    .forEach(b => b.setAttribute('aria-current', b.dataset.tab === tab ? 'page' : 'false'));
	document.querySelectorAll('.tab').forEach(s => s.hidden = s.id !== tab);
	if (load && (!loaded.has(tab) || tab === 'logs'))
		refreshTab(tab);
	scheduleLogUpdates( );
	scheduleProcesses( );
}

$('login-form').addEventListener('submit', async e => {
	e.preventDefault( );
	if (loginPending)
		return;
	if (location.protocol !== 'https:' && !['127.0.0.1', 'localhost'].includes(location.hostname)) {
		notice('Для входа откройте панель через HTTPS.');
		return;
	}
	const id = ++session;
	loginPending = true;
	controls( );
	notice('Проверяем пароль…');
	const bytes = new TextEncoder( ).encode('root:' + $('password').value);
	authorization = 'Basic ' + btoa(Array.from(bytes, b => String.fromCharCode(b)).join(''));
	$('password').value = '';
	try {
		await metrics( );
		if (!current(id))
			return;
		$('login').hidden = true;
		$('dashboard').hidden = false;
		$('logout').hidden = false;
		notice('');
		// Clear data from the previous login; only non-secret preferences are stored.
		['services-output', 'packages-output', 'network-output', 'logs-output'].forEach(
		    target => output(target, ''));
		output('task-output', 'Операций пока нет.');
		['service-details', 'service-config', 'service-events', 'package-details', 'package-plan',
		 'history-output', 'logs-status']
		    .forEach(target => output(target, ''));
		$('history-job').replaceChildren( );
		$('service-names').replaceChildren( );
		$('package-names').replaceChildren( );
		$('files-path').value = recalled('webpanel-directory') || '/etc';
		packageView = recalled('webpanel-package-view') === 'installed' ? 'installed' : '';
		selectTab(recalled('webpanel-tab') || 'services', false);
		timer = setInterval(( ) => {
			if (!document.hidden)
				metrics( ).catch(e => {
					if (current(id))
						notice(e.message);
				});
		}, 10000);
		const saved = savedJob( );
		if (saved) {
			pendingRead = selectedTab;
			void run(saved.values, saved.target, saved.job);
		} else
			refreshTab(selectedTab);
		void refreshHistory( );
	} catch (e) {
		if (id === session)
			endSession(e.message);
	} finally {
		if (id === session) {
			loginPending = false;
			controls( );
		}
	}
});
$('logout').onclick = ( ) => {
	if (!fileDirty( ) || confirm('Выйти и потерять несохранённые изменения конфигурации?'))
		endSession('Вы вышли из панели. Активная команда может продолжаться на сервере.');
};
window.addEventListener('beforeunload', e => {
	if (busy || fileDirty( )) {
		e.preventDefault( );
		e.returnValue = '';
	}
});
window.addEventListener('online', ( ) => {
	const id = session;
	if (authorization)
		metrics( ).catch(e => {
			if (current(id))
				notice(e.message);
		});
});

function syncTab( ) {
	selectTab(location.hash.slice(1) || recalled('webpanel-tab') || 'services', !!authorization);
}

window.addEventListener('hashchange', syncTab);
document.querySelectorAll('[data-tab]').forEach(b => {
	b.onclick = ( ) => {
		selectTab(b.dataset.tab);
		history.replaceState(null, '', '#' + b.dataset.tab);
	};
	b.addEventListener('keydown', e => {
		if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(e.key))
			return;
		e.preventDefault( );
		const index = tabs.indexOf(b.dataset.tab),
		      next = e.key === 'Home' ? 0
		             : e.key === 'End'
		                 ? tabs.length - 1
		                 : (index + (e.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length;
		const button = document.querySelector(`[data-tab="${tabs[next]}"]`);
		button.focus( );
		button.click( );
	});
});
syncTab( );
$('services-refresh').onclick = ( ) => refreshTab('services');
const serviceActions = {
	start : 'Запустить',
	stop : 'Остановить',
	restart : 'Перезапустить',
	enable : 'Включить автозапуск',
	disable : 'Отключить автозапуск'
};
const packageActions = {
	install : 'Установить',
	upgrade : 'Обновить',
	remove : 'Удалить'
};

function actionValues(kind) {
	const input = $(kind + '-name');
	input.value = input.value.trim( );
	if (!$(kind + '-form').reportValidity( ))
		return null;
	const name = input.value, action = $(kind + '-action').value;
	if (kind === 'service' && [ 'webpanel', 'nginx' ].includes(name)) {
		notice('Службами webpanel и nginx управляйте через SSH, чтобы сохранить доступ к панели.');
		input.focus( );
		return null;
	}
	if (kind === 'package' && action === 'remove' && protectedPackages.has(name)) {
		notice(`Пакет ${name} защищён от удаления через панель.`);
		input.focus( );
		return null;
	}
	return { operation : kind, name, action };
}

async function submitAction(kind) {
	if (busy)
		return;
	const values = actionValues(kind);
	if (!values)
		return;
	const id = session,
	      label = (kind === 'service' ? serviceActions : packageActions)[values.action];
	if (kind === 'package') {
		const plan =
		    await run({...values, operation : 'package-plan' }, 'package-plan', null, false);
		if (!plan || !current(id)) {
			if (current(id))
				flushRead( );
			return;
		}
		// Apply the exact selection that was planned, even if form fields changed meanwhile.
		if (confirm(`${label}: пакет «${values.name}»?\n\n${
				plan.output.slice(-4000)}\n\nПолный план показан в разделе пакетов.`))
			void run(values);
		else
			flushRead( );
	} else if (confirm(`${label}: служба «${values.name}»?`))
		void run(values);
}

['service', 'package'].forEach(kind => {
	$(kind + '-form').onsubmit = e => {
		e.preventDefault( );
		submitAction(kind);
	};
	$(kind + '-name')
	    .addEventListener('change',
	                      ( ) => renderPicker(kind === 'service' ? 'services' : 'packages'));
});

function listPackages(view) {
	packageView = view;
	remember('webpanel-package-view', view);
	refreshTab('packages');
}

$('packages-list').onclick = ( ) => listPackages('');
$('packages-installed').onclick = ( ) => listPackages('installed');
$('packages-update').onclick = ( ) => {
	if (confirm('Обновить каталог пакетов?'))
		void run({ operation : 'package', action : 'update' });
};
$('network-refresh').onclick = ( ) => refreshTab('network');

function selectedPicker(kind) {
	return $(kind === 'processes'  ? 'process-choice'
	         : kind === 'services' ? 'service-name'
	                               : 'package-name')
	    .value;
}

function renderPicker(kind) {
	const rows = pickerRows[kind], size = 20;
	pickerPages[kind] = Math.max(0, Math.min(pickerPages[kind], Math.ceil(rows.length / size) - 1));
	const first = pickerPages[kind] * size, selected = selectedPicker(kind);
	const body = $(kind + '-rows');
	body.replaceChildren( );
	for (const item of rows.slice(first, first + size)) {
		const row = document.createElement('tr'), name = document.createElement('td'),
		      info = document.createElement('td'), button = document.createElement('button');
		button.type = 'button';
		button.textContent = item.label;
		button.dataset.read = '';
		button.dataset.value = item.value;
		button.setAttribute('aria-pressed', String(item.value === selected));
		button.setAttribute('aria-label', 'Выбрать ' + item.label);
		button.onclick = ( ) => choosePicker(kind, item.value);
		name.append(button);
		info.textContent = item.summary;
		row.append(name, info);
		body.append(row);
	}
	$(kind + '-table').hidden = !rows.length;
	$(kind + '-page').textContent =
	    rows.length ? `${first + 1}–${Math.min(first + size, rows.length)} из ${rows.length}`
				    : 'Нет совпадений';
	for (const [suffix, unavailable] of [[ 'previous', first === 0 ],
	                                     [ 'next', first + size >= rows.length ]]) {
		const button = $(kind + '-' + suffix);
		button.dataset.unavailable = String(unavailable);
		button.disabled = unavailable;
	}
	const item = rows.find(item => item.value === selected);
	$(kind + '-selection').textContent =
	    selected ? `Выбрано: ${item?.label || selected}${
			           item ? ' · ' + item.summary : ' · скрыто текущим фильтром'}`
			     : 'Выберите строку из списка.';
}

function choosePicker(kind, value) {
	const input = $(kind === 'processes'  ? 'process-choice'
	                : kind === 'services' ? 'service-name'
	                                      : 'package-name');
	input.value = value;
	if (input.value !== value)
		return;
	input.dispatchEvent(new Event('change', { bubbles : true }));
	renderPicker(kind);
	notice('');
}

for (const kind of Object.keys(pickerPages)) {
	$(kind + '-previous').onclick = ( ) => {
		pickerPages[kind]--;
		renderPicker(kind);
	};
	$(kind + '-next').onclick = ( ) => {
		pickerPages[kind]++;
		renderPicker(kind);
	};
}

function renderList(kind, done = true) {
	const query = $(kind + '-filter').value.trim( ).toLocaleLowerCase( );
	const state = kind === 'services' ? $('services-state').value : '';
	const all = listText[kind]
	                .split('\n')
	                .map(line => line.trim( ))
	                .filter(line => /^[a-zA-Z0-9_][a-zA-Z0-9_-]{0,62}\s/.test(line));
	const lines = all.filter(line => line.toLocaleLowerCase( ).includes(query) &&
	                                 (!state || line.split(/\s+/)[1] === state));
	output(kind + '-output',
	       lines.join('\n') || (done ? 'Ничего не найдено.' : 'Загрузка списка…'));
	if (done) {
		const input = $(kind === 'services' ? 'service-name' : 'package-name'),
		      selected = input.value;
		input.replaceChildren(new Option('Выберите из списка', ''), ...all.map(line => {
			const parts = line.split(/\s+/);
			return new Option(parts[0] + ' — ' + parts.slice(1).join(' '), parts[0]);
		}));
		if (all.some(line => line.split(/\s+/)[0] === selected))
			input.value = selected;
	}
	const states = {
		running : 'Запущена',
		stopped : 'Остановлена',
		backoff : 'Ожидает перезапуска'
	};
	pickerRows[kind] = lines.map(line => {
		const parts = line.split(/\s+/), name = parts.shift( );
		return {
			value : name,
			label : name,
			summary : kind === 'services' ? `${states[parts[0]] || parts[0]}${
					                                ['nginx', 'webpanel'].includes(name)
					                                    ? ' · управление через SSH'
					                                    : ''} · ${parts.slice(1).join(' ')}`
						                  : parts.join(' ').replace('(installed)', '(установлен)')
		};
	});
	renderPicker(kind);
}

['services', 'packages'].forEach(kind => $(kind + '-filter').oninput = ( ) => {
	pickerPages[kind] = 0;
	renderList(kind);
});
$('services-state').onchange = ( ) => {
	pickerPages.services = 0;
	renderList('services');
};

async function inspectService( ) {
	const name = $('service-name').value.trim( );
	if (!/^[a-zA-Z0-9_][a-zA-Z0-9_-]{0,62}$/.test(name)) {
		$('service-name').reportValidity( );
		return;
	}
	const id = session, revision = ++serviceRevision;
	output('service-details', 'Загрузка сведений о службе…');
	try {
		const data = await (await request('/api/service?name=' + encodeURIComponent(name))).json( );
		if (!current(id) || revision !== serviceRevision ||
		    $('service-name').value.trim( ) !== name)
			return;
		const state = {
			running : 'Запущена',
			stopped : 'Остановлена',
			backoff : 'Ожидает перезапуска'
		}[data.state] ||
		              data.state;
		const exits =
		    [...(data.events || '').matchAll(/exited pid=(\d+) detail=(\d+)(?: time=(\d+))?/g) ];
		const last = exits.at(-1), code = last ? Number(last[2]) : null;
		const signals = { 6 : 'SIGABRT', 9 : 'SIGKILL', 11 : 'SIGSEGV', 15 : 'SIGTERM' };
		const reason = code === null ? 'Нет записей о завершении'
		               : code === 0  ? 'нормальное завершение'
		               : code >= 128
		                   ? `сигнал ${code - 128}${
				                 signals[code - 128] ? ' (' + signals[code - 128] + ')' : ''}`
				           : 'ошибка процесса';
		output('service-details', [
			`Служба: ${name} — ${state}`,
			`PID: ${data.pid || '—'}; супервизор: ${data.supervisor || '—'}`,
			`Автозапуск: ${data.autostart ? 'включён' : 'выключен'}; целевое состояние: ${
				data.desired ? 'запущена' : 'остановлена'}`,
			`Задержка перезапуска: ${data.retry} с`,
			`Последнее завершение: ${reason}${code === null ? '' : '; код ' + code}${
				last?.[3] ? ', ' + new Date(Number(last[3]) * 1000).toLocaleString('ru-RU') : ''}`,
			`Запусков в доступном хвосте журнала: ${
				(data.events || '')
					.split('\n')
					.filter(line => line.includes('] started pid='))
					.length}`,
			`Память процесса: ${
				data.memory_bytes < 0 ? 'недоступна'
									  : (data.memory_bytes / 1048576).toFixed(1) + ' МиБ'}`,
			`Процессорное время: ${
				data.cpu_seconds < 0 ? 'недоступно' : data.cpu_seconds.toFixed(2) + ' с'}`,
			data.heartbeat_age < 0 ? 'Нет данных супервизора.'
								   : `Последний ответ супервизора: ${data.heartbeat_age} с назад`,
			'События ниже содержат запросы перезапуска и коды завершения процессов.'
		].join('\n'));
		output('service-config',
		       data.config === null
		           ? 'Конфигурация недоступна.'
		           : data.config + (data.config_truncated ? '\n[Показаны первые 4 КиБ]' : ''));
		output('service-events', data.events || 'Событий супервизора пока нет.');
	} catch (e) {
		if (current(id) && revision === serviceRevision) {
			output('service-details', e.message);
			output('service-config', 'Конфигурация недоступна.');
			output('service-events', 'События недоступны.');
		}
	}
}

$('service-inspect').onclick = ( ) => void inspectService( );
$('service-name').addEventListener('change', ( ) => void inspectService( ));
$('service-log').onclick = async ( ) => {
	const name = $('service-name').value.trim( );
	if (!/^[a-zA-Z0-9_][a-zA-Z0-9_-]{0,62}$/.test(name)) {
		$('service-name').reportValidity( );
		return;
	}
	const id = session;
	selectTab('logs', false);
	history.replaceState(null, '', '#logs');
	try {
		await loadLogs( );
		if (!current(id))
			return;
		const filename = name + '.log';
		if (!Array.from($('log-name').options).some(o => o.value === filename)) {
			notice(`Журнал ${filename} пока отсутствует.`);
			return;
		}
		$('log-name').value = filename;
		logText = '';
		logFile = filename;
		void readLog( );
	} catch (e) {
		if (current(id))
			notice(e.message);
	}
};
$('package-info').onclick = ( ) => {
	const name = $('package-name').value.trim( );
	if (!/^[a-zA-Z0-9_][a-zA-Z0-9_-]{0,62}$/.test(name)) {
		$('package-name').reportValidity( );
		return;
	}
	void run({ operation : 'package-info', name }, 'package-details');
};
$('package-preview').onclick = ( ) => {
	const values = actionValues('package');
	if (values)
		void run({...values, operation : 'package-plan' }, 'package-plan');
};
$('packages-updates').onclick = ( ) => void run({ operation : 'package-updates' }, 'package-plan');

function renderLog( ) {
	const query = $('logs-filter').value.toLocaleLowerCase( ), stamp = $('logs-time').value,
	      level = $('logs-level').value;
	const patterns = {
		error : /\b(error|err|fatal|critical|crit|panic|failed|failure)\b|ошибк|авари/i,
		warning : /\b(warn|warning)\b|предупрежд/i,
		info : /\b(info|notice|debug)\b|информац/i
	};
	const source = logText.split('\n').filter(line => line.length);
	const lines =
	    source.filter(line => line.toLocaleLowerCase( ).includes(query) && line.includes(stamp) &&
	                          (!level || patterns[level].test(line)));
	output('logs-output', lines.join('\n') || (logText   ? 'Нет строк, соответствующих фильтрам.'
	                                           : logFile ? 'Файл журнала пуст.'
	                                                     : 'Журналы пока отсутствуют.'));
	$('logs-status').textContent =
	    `${logLive ? 'Автообновление включено' : 'Автообновление остановлено'}${
			logUpdated ? '; обновлено ' + logUpdated
			           : ''}. Строк: ${lines.length} / ${source.length}.`;
}

async function readLog(revision = ++logRevision) {
	const id = session, name = $('log-name').value;
	if (!name) {
		logFile = '';
		logText = '';
		renderLog( );
		return;
	}
	logPending++;
	try {
		const text = await (await request('/api/log?name=' + encodeURIComponent(name))).text( );
		if (current(id) && revision === logRevision && $('log-name').value === name) {
			logText = text;
			logFile = name;
			logUpdated = new Date( ).toLocaleTimeString('ru-RU');
			renderLog( );
			remember('webpanel-log', name);
		}
	} catch (e) {
		if (current(id) && revision === logRevision)
			$('logs-status').textContent = e.message;
	} finally {
		if (current(id))
			logPending--;
	}
}

async function loadLogs( ) {
	const id = session, revision = ++logRevision,
	      previous = $('log-name').value || recalled('webpanel-log');
	const names = await (await request('/api/logs')).json( );
	if (!current(id) || revision !== logRevision)
		return;
	names.sort( );
	$('log-name').replaceChildren(...names.map(name => {
		const o = document.createElement('option');
		o.value = name;
		o.textContent = name;
		return o;
	}));
	if (names.includes(previous))
		$('log-name').value = previous;
	if (logFile !== $('log-name').value) {
		logText = '';
		logUpdated = '';
	}
	loaded.add('logs');
	await readLog(revision);
}

function scheduleLogUpdates( ) {
	clearTimeout(logTimer);
	logTimer = null;
	if (!logLive || !authorization || selectedTab !== 'logs')
		return;
	logTimer = setTimeout(async ( ) => {
		const id = session;
		if (!document.hidden && !logPending)
			await readLog( );
		if (current(id))
			scheduleLogUpdates( );
	}, 2000);
}

$('logs-live').onclick = ( ) => {
	logLive = !logLive;
	$('logs-live').setAttribute('aria-pressed', String(logLive));
	$('logs-live').textContent =
	    logLive ? 'Приостановить автообновление' : 'Включить автообновление';
	renderLog( );
	scheduleLogUpdates( );
};
['logs-filter', 'logs-time'].forEach(id => $(id).oninput = renderLog);
$('logs-level').onchange = renderLog;
$('log-name').onchange = ( ) => {
	logText = '';
	logUpdated = '';
	logFile = $('log-name').value;
	notice('');
	output('logs-output', 'Загрузка…');
	void readLog( );
};
$('logs-refresh').onclick = ( ) => refreshTab('logs');

function download(name, text) {
	const url = URL.createObjectURL(new Blob([ text ], { type : 'text/plain;charset=utf-8' }));
	const a = document.createElement('a');
	a.href = url;
	a.download = name;
	document.body.append(a);
	a.click( );
	a.remove( );
	setTimeout(( ) => URL.revokeObjectURL(url), 1000);
}

$('logs-download').onclick = ( ) => {
	if (logFile && logUpdated)
		download(logFile, logText);
	else
		notice('Сначала дождитесь загрузки журнала.');
};

function jobTitle(item) {
	const status = item.unknown      ? 'результат неизвестен'
	               : !item.done      ? 'выполняется'
	               : item.exit === 0 ? 'успех'
	                                 : `код ${item.exit}`;
	return `#${item.id} · ${new Date(item.created * 1000).toLocaleString('ru-RU')} · ${
		item.user} · ${operationLabels[item.operation] || item.operation} ${item.action || ''} ${
		item.name || ''} · ${status}`;
}

async function refreshHistory( ) {
	if (!authorization)
		return;
	const id = session, revision = ++historyRevision, selected = $('history-job').value;
	try {
		const items = await (await request('/api/history')).json( );
		if (!current(id) || revision !== historyRevision)
			return;
		historyItems = items;
		$('history-job').replaceChildren(...items.map(item => {
			const o = document.createElement('option');
			o.value = String(item.id);
			o.textContent = jobTitle(item);
			return o;
		}));
		if (items.some(item => String(item.id) === selected))
			$('history-job').value = selected;
		if (!items.length) {
			historyReadRevision++;
			historyText = '';
			output('history-output', 'Операций пока нет.');
			return;
		}
		await readHistory( );
	} catch (e) {
		if (current(id) && revision === historyRevision)
			output('history-output', e.message);
	}
}

async function readHistory( ) {
	const id = session, revision = ++historyReadRevision, value = $('history-job').value;
	const item = historyItems.find(item => String(item.id) === value);
	if (!item)
		return;
	historyText = '';
	output('history-output', 'Загрузка вывода операции…');
	try {
		const result = await (await request('/api/job?id=' + item.id)).json( );
		if (!current(id) || revision !== historyReadRevision || $('history-job').value !== value)
			return;
		historyText = `${jobTitle(item)}\n${
			item.finished
				? 'Завершена: ' + new Date(item.finished * 1000).toLocaleString('ru-RU') + '\n'
				: ''}${
			item.unknown ? 'Backend перезапущен; окончательный результат команды неизвестен.\n'
					     : ''}\n${result.output || 'Вывод отсутствует.'}`;
		output('history-output', historyText);
	} catch (e) {
		if (current(id) && revision === historyReadRevision)
			output('history-output', e.message);
	}
}

$('history-refresh').onclick = ( ) => void refreshHistory( );
$('history-job').onchange = ( ) => void readHistory( );
$('history-download').onclick = ( ) => {
	if (historyText)
		download('operation-' + $('history-job').value + '.log', historyText);
	else
		notice('Сначала выберите операцию с доступным выводом.');
};

function fileDirty( ) {
	return !!openFile?.editable && $('file-editor').value !== openFile.content;
}

function resetManagement( ) {
	clearTimeout(processTimer);
	processTimer = null;
	processPending = false;
	processRevision++;
	filesRevision++;
	fileRevision++;
	diskRevision++;
	processRows = [];
	directoryData = null;
	openFile = null;
	backupId = null;
	['processes-output', 'process-details', 'disks-output', 'disk-usage', 'file-output',
	 'file-current', 'files-status', 'file-status']
	    .forEach(id => output(id, ''));
	$('process-choice').replaceChildren( );
	$('file-entry').replaceChildren( );
	$('file-editor').value = '';
	$('file-editor').disabled = false;
	$('file-editor-label').hidden = true;
	$('file-save').hidden = true;
	$('file-restore').hidden = true;
}

async function refreshManagement(tab) {
	const id = session;
	try {
		if (tab === 'processes')
			await loadProcesses( );
		if (tab === 'disks')
			await loadDisks( );
		if (tab === 'files')
			await loadDirectory( );
	} catch (e) {
		if (current(id))
			notice(e.message);
	}
}

function bytesLabel(n) {
	const units = [ 'Б', 'КиБ', 'МиБ', 'ГиБ', 'ТиБ' ];
	let i = 0;
	while (n >= 1024 && i < units.length - 1) {
		n /= 1024;
		i++;
	}
	return n.toFixed(i ? 1 : 0) + ' ' + units[i];
}

function renderProcesses( ) {
	const query = $('processes-filter').value.trim( ).toLocaleLowerCase( ),
	      sort = $('processes-sort').value;
	const rows = processRows.filter(p => `${p.pid} ${p.name}`.toLocaleLowerCase( ).includes(query));
	rows.sort((a, b) => sort === 'name'     ? a.name.localeCompare(b.name)
	                    : sort === 'memory' ? b.memory_bytes - a.memory_bytes
	                    : sort === 'cpu'    ? b.cpu_percent - a.cpu_percent
	                                        : a.pid - b.pid);
	const selected = $('process-choice').value;
	$('process-choice').replaceChildren(...rows.map(p => {
		const o = document.createElement('option');
		o.value = String(p.pid);
		o.textContent = `${p.pid} — ${p.name}${p.protected ? ' (защищён)' : ''}`;
		return o;
	}));
	$('process-choice').value = rows.some(p => String(p.pid) === selected) ? selected : '';
	pickerRows.processes =
	    rows.map(p => ({
		             value : String(p.pid),
		             label : `${p.pid} — ${p.name}`,
				     summary : `CPU ${p.cpu_percent.toFixed(1)}% · ${bytesLabel(p.memory_bytes)}${
						 p.protected ? ' · защищён' : ''}`
				 }));
	renderPicker('processes');
	output('processes-output',
	       rows.length ? 'PID       CPU       Память     Потоки  Имя\n' +
	                         rows.map(p => `${String(p.pid).padEnd(10)}${
			                              p.cpu_percent.toFixed(1).padEnd(
			                                  8)}% ${bytesLabel(p.memory_bytes).padEnd(11)}${
					                      String(p.threads).padEnd(8)}${p.name}`)
						         .join('\n')
					   : 'Процессы не найдены.');
	renderProcessDetail( );
}

function selectedProcess( ) {
	return processRows.find(p => String(p.pid) === $('process-choice').value);
}

function renderProcessDetail( ) {
	const p = selectedProcess( );
	output('process-details',
	       p ? `PID: ${p.pid}\nИмя: ${p.name}\nСостояние: ${
				   { 0 : 'Выполняется', 1 : 'Готов к выполнению', 2 : 'Завершён' }[p.state] ||
				   p.state}\nПотоков: ${p.threads}\nПамять: ${bytesLabel(p.memory_bytes)}\nCPU: ${
				   p.cpu_percent.toFixed(
					   1)}%\nПроцессорное время: ${(Number(p.cpu_us) / 1000000).toFixed(2)} с\n${
				   p.protected ? 'Системный процесс защищён от завершения через панель.' : ''}`
			 : 'Выберите процесс.');
}

async function loadProcesses( ) {
	if (processPending)
		return;
	const id = session, revision = ++processRevision;
	processPending = true;
	try {
		const rows = await (await request('/api/processes')).json( );
		if (current(id) && revision === processRevision) {
			processRows = rows;
			loaded.add('processes');
			renderProcesses( );
		}
	} finally {
		if (current(id))
			processPending = false;
	}
}

function scheduleProcesses( ) {
	clearTimeout(processTimer);
	processTimer = null;
	if (!authorization || selectedTab !== 'processes')
		return;
	processTimer = setTimeout(async ( ) => {
		const id = session;
		if (!document.hidden)
			await refreshManagement('processes');
		if (current(id))
			scheduleProcesses( );
	}, 5000);
}

$('processes-refresh').onclick = ( ) => void refreshManagement('processes');
$('processes-filter').oninput = ( ) => {
	pickerPages.processes = 0;
	renderProcesses( );
};
$('processes-sort').onchange = renderProcesses;
$('process-choice').onchange = ( ) => {
	renderProcessDetail( );
	renderPicker('processes');
};

async function signalProcess(signal) {
	const p = selectedProcess( );
	if (busy)
		return;
	if (!p) {
		notice('Выберите процесс в списке.');
		return;
	}
	if (p.protected) {
		notice('Этот процесс защищён от завершения через панель.');
		return;
	}
	if (confirm(`Отправить ${signal} процессу ${p.pid} «${p.name}»?`)) {
		const id = session;
		await run({
			operation : 'process',
			pid : String(p.pid),
			signal,
			name : p.name,
			cpu_us : p.cpu_us
		});
		if (current(id))
			void refreshManagement('processes');
	}
}

$('process-term').onclick = ( ) => void signalProcess('TERM');
$('process-kill').onclick = ( ) => void signalProcess('KILL');

async function loadDisks( ) {
	const id = session, revision = ++diskRevision,
	      data = await (await request('/api/disks')).json( );
	if (!current(id) || revision !== diskRevision)
		return;
	loaded.add('disks');
	output('disks-output',
	       data.length
	           ? data.map(d => `${d.path} · файловая система ${d.device}\nОбъём: ${
				              bytesLabel(d.total)}; занято: ${
					          bytesLabel(Math.max(
					              0, d.total - d.free))}; доступно: ${bytesLabel(d.available)}\n${
							  d.files ? 'Файловых узлов: ' + d.files + '; свободно: ' + d.files_free
							          : 'Сведения о файловых узлах недоступны.'}`)
					 .join('\n\n')
			   : 'Файловая система не предоставила сведения о свободном месте.');
}

$('disks-refresh').onclick = ( ) => void refreshManagement('disks');
$('disk-usage-form').onsubmit = async e => {
	e.preventDefault( );
	const path = $('disk-path').value.trim( );
	const id = session;
	const result = await run({ operation : 'disk-usage', path }, 'disk-usage');
	if (!result || !current(id))
		return;
	const rows = [], messages = [];
	for (const line of result.output.split('\n')) {
		const match = line.match(/^(\d+)\t(.+)$/);
		if (match)
			rows.push({ bytes : Number(match[1]), name : match[2] });
		else if (line)
			messages.push(line);
	}
	rows.sort((a, b) => b.bytes - a.bytes);
	output('disk-usage', messages.join('\n') + '\n\n' +
	                         rows.map(r => `${bytesLabel(r.bytes)}\t${r.name}`).join('\n'));
};

function childPath(directory, name) { return (directory === '/' ? '' : directory) + '/' + name; }

async function loadDirectory(path = $('files-path').value.trim( )) {
	const id = session, revision = ++filesRevision;
	const data = await (await request('/api/files?path=' + encodeURIComponent(path))).json( );
	if (!current(id) || revision !== filesRevision)
		return;
	directoryData = data;
	$('files-path').value = data.path;
	remember('webpanel-directory', data.path);
	loaded.add('files');
	$('files-filter').value = '';
	renderDirectory( );
}

function renderDirectory( ) {
	if (!directoryData)
		return;
	const data = directoryData, query = $('files-filter').value.trim( ).toLocaleLowerCase( ),
	      selected = $('file-entry').value;
	const entries = data.entries.filter(entry => entry.name.toLocaleLowerCase( ).includes(query));
	entries.sort((a, b) => (a.type === 'directory' ? 0 : 1) - (b.type === 'directory' ? 0 : 1) ||
	                       a.name.localeCompare(b.name));
	$('file-entry').replaceChildren(...entries.map(entry => {
		const o = document.createElement('option');
		o.value = entry.name;
		o.textContent = `${
			entry.type === 'directory' ? '[каталог]'
			: entry.type === 'symlink' ? '[ссылка]'
			: entry.type === 'special' ? '[спецфайл]'
			                           : bytesLabel(entry.size)} ${entry.name}`;
		return o;
	}));
	$('file-entry').value = entries.some(e => e.name === selected) ? selected : '';
	$('files-status').textContent = `${data.path}: ${entries.length} из ${
		data.entries.length} элементов.${data.truncated ? ' Показаны первые 512 элементов.' : ''}`;
}

$('files-filter').oninput = renderDirectory;
$('files-shortcuts').onclick = e => {
	const path = e.target.dataset.directory;
	if (path) {
		const id = session;
		void loadDirectory(path).catch(e => {
			if (current(id))
				notice(e.message);
		});
	}
};
$('file-entry').onkeydown = e => {
	if (e.key === 'Enter') {
		e.preventDefault( );
		void openEntry( ).catch(e => notice(e.message));
	}
};
$('files-form').onsubmit = e => {
	e.preventDefault( );
	void refreshManagement('files');
};
$('files-refresh').onclick = ( ) => void refreshManagement('files');
$('files-up').onclick = ( ) => {
	const path = directoryData?.path || '/etc',
	      parent = path.slice(0, path.lastIndexOf('/')) || '/';
	void loadDirectory(parent).catch(e => notice(e.message));
};

function selectedEntry( ) {
	return directoryData?.entries.find(e => e.name === $('file-entry').value);
}

async function openEntry( ) {
	if ($('file-editor').disabled) {
		notice('Дождитесь завершения сохранения конфигурации.');
		return;
	}
	const entry = selectedEntry( );
	if (!entry) {
		notice('Выберите файл или каталог в списке.');
		return;
	}
	const path = childPath(directoryData.path, entry.name);
	if (entry.type === 'directory') {
		await loadDirectory(path);
		return;
	}
	if (entry.type !== 'file') {
		notice('Символические ссылки и специальные файлы не открываются.');
		return;
	}
	if (fileDirty( ) && !confirm('Открыть другой файл и потерять несохранённые правки?'))
		return;
	const id = session, revision = ++fileRevision;
	const file = await (await request('/api/file?path=' + encodeURIComponent(path))).json( );
	if (!current(id) || revision !== fileRevision)
		return;
	openFile = file;
	backupId = null;
	$('file-current').textContent = `${file.path} · ${bytesLabel(file.size)}`;
	output('file-output', file.text
	                          ? file.content + (file.truncated ? '\n[Показаны первые 64 КиБ]' : '')
	                          : 'Двоичный файл. Скачайте его для просмотра.');
	$('file-editor-label').hidden = !file.editable;
	$('file-save').hidden = !file.editable;
	$('file-restore').hidden = true;
	$('file-editor').value = file.editable ? file.content : '';
	$('file-status').textContent =
	    file.editable
	        ? 'Поддерживаемый конфиг: перед сохранением будет проверен формат и создана резервная копия.'
	        : 'Просмотр без редактирования.';
}

$('file-open').onclick = ( ) => void openEntry( ).catch(e => notice(e.message));
$('file-entry').ondblclick = ( ) => void openEntry( ).catch(e => notice(e.message));

function downloadBytes(name, bytes) {
	const url = URL.createObjectURL(new Blob([ bytes ]));
	const a = document.createElement('a');
	a.href = url;
	a.download = name;
	document.body.append(a);
	a.click( );
	a.remove( );
	setTimeout(( ) => URL.revokeObjectURL(url), 1000);
}

$('file-download').onclick = async ( ) => {
	const entry = selectedEntry( );
	if (!entry || entry.type !== 'file') {
		notice('Выберите обычный файл.');
		return;
	}
	const id = session, path = childPath(directoryData.path, entry.name);
	try {
		const bytes =
		    await (await request('/api/download?path=' + encodeURIComponent(path))).bytes( );
		if (current(id))
			downloadBytes(entry.name, bytes);
	} catch (e) {
		if (current(id))
			notice(e.message);
	}
};
$('file-editor').oninput = ( ) => {
	$('file-status').textContent =
	    fileDirty( ) ? 'Есть несохранённые изменения.' : 'Изменений нет.';
};

async function saveConfig(restore = false) {
	if (!openFile?.editable || busy)
		return;
	const file = openFile, text = $('file-editor').value, id = session;
	if (restore && !backupId)
		return;
	if (!confirm(`${restore ? 'Восстановить предыдущую версию' : 'Проверить и сохранить'} ${
			file.path}?`))
		return;
	const body = restore ? '' : text;
	if (new TextEncoder( ).encode(body).length > 65536) {
		notice('Конфигурация больше 64 КиБ.');
		return;
	}
	$('file-editor').disabled = true;
	try {
		const result = await run(
		    { operation : 'config', path : file.path, action : restore ? 'restore' : 'save' },
		    'task-output', null, true, {
			    path : '/api/config?path=' + encodeURIComponent(file.path),
			    type : 'text/plain;charset=utf-8',
			    headers : {
				    'X-Webpanel-Revision' : file.revision,
				    ...(restore ? { 'X-Webpanel-Restore' : String(backupId) } : { })
			    },
			    body
		    });
		if (!current(id) || !result)
			return;
		if (restore) {
			const restored =
			    await (await request('/api/file?path=' + encodeURIComponent(file.path))).json( );
			if (!current(id))
				return;
			openFile = restored;
			$('file-editor').value = restored.content;
		} else {
			openFile = {...file, revision : result.revision, content : text };
		}
		backupId = result.backup;
		$('file-restore').hidden = !backupId;
		output('file-output', openFile.content);
		$('file-status').textContent =
		    `Сохранено. Резервная копия #${backupId}. Служба автоматически не перезапускается.`;
	} finally {
		if (current(id))
			$('file-editor').disabled = false;
	}
}

$('file-save').onclick = ( ) => void saveConfig( ).catch(e => notice(e.message));
$('file-restore').onclick = ( ) => void saveConfig(true).catch(e => notice(e.message));
$('file-upload').onchange =
    ( ) => { $('file-upload-name').value = $('file-upload').files[0]?.name || ''; };

function newFilePath(name) {
	if (!directoryData) {
		notice('Сначала откройте каталог.');
		return null;
	}
	if (!name || name === '.' || name === '..' || /[\/\\\x00-\x1f\x7f]/.test(name)) {
		notice('Укажите имя без разделителей каталогов и управляющих символов.');
		return null;
	}
	return childPath(directoryData.path, name);
}

$('file-upload-form').onsubmit = async e => {
	e.preventDefault( );
	if (busy)
		return;
	const file = $('file-upload').files[0], path = newFilePath($('file-upload-name').value);
	if (!file || !path)
		return;
	if (file.size > 1048576) {
		notice('Максимальный размер файла — 1 МиБ.');
		return;
	}
	if (!confirm(`Загрузить ${file.name} в ${path}?`))
		return;
	const id = session,
	      result = await run({ operation : 'file', action : 'upload', path }, 'task-output', null,
	                         true, {
		                         path : '/api/upload?path=' + encodeURIComponent(path),
		                         type : 'application/octet-stream',
		                         body : file
	                         });
	if (current(id) && result)
		void refreshManagement('files');
};
$('file-mkdir-form').onsubmit = async e => {
	e.preventDefault( );
	if (busy)
		return;
	const path = newFilePath($('file-mkdir-name').value);
	if (!path || !confirm(`Создать каталог ${path}?`))
		return;
	const id = session, result = await run({ operation : 'file', action : 'mkdir', path },
	                                       'task-output', null, true, {
		                                       path : '/api/mkdir?path=' + encodeURIComponent(path),
		                                       type : 'application/octet-stream',
		                                       body : ''
	                                       });
	if (current(id) && result)
		void refreshManagement('files');
};
