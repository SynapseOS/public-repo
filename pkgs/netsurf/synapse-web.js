// synapse-web.js: Promise, fetch, XMLHttpRequest и localStorage для NetSurf
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Duktape ES5 и привязки NetSurf
// Подсистема: веб-интерфейсы браузера
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
(function(global) {
"use strict";
var jobs = [], scheduled = false, draining = false;

// Исполнить задания после возврата внешнего вызова JavaScript
function drain( ) {
	if (draining) {
		return;
	}
	draining = true;
	scheduled = false;
	var count = 0;
	while (jobs.length && count++ < 4096) {
		var job = jobs.shift( );
		try {
			job( );
		} catch (error) {
			(function(failure) { global.setTimeout(function( ) { throw failure; }, 0); }(error));
		}
	}
	draining = false;
	if (jobs.length && !scheduled) {
		scheduled = true;
		global.setTimeout(drain, 0);
	}
}

// Сохранить порядок обработчиков Promise, не выполняя их внутри then
function enqueue(job) {
	if (typeof job !== "function") {
		throw new TypeError("Callback must be a function");
	}
	jobs.push(job);
	if (!scheduled) {
		scheduled = true;
		global.setTimeout(drain, 0);
	}
}
Object.defineProperty(global, "__synapseDrainJobs", { value : drain });
global.queueMicrotask = enqueue;

// Завершить Promise ровно один раз с учётом чужих thenable
function settle(promise, state, value) {
	if (promise._state) {
		return;
	}
	if (state === 1 && value === promise) {
		settle(promise, 2, new TypeError("Promise self resolution"));
		return;
	}
	if (state === 1 && value !== null &&
	    (typeof value === "object" || typeof value === "function")) {
		var then;
		try {
			then = value.then;
		} catch (error) {
			settle(promise, 2, error);
			return;
		}
		if (typeof then === "function") {
			var done = false;
			try {
				then.call(value,
				          function(result) {
					          if (!done) {
						          done = true;
						          settle(promise, 1, result);
					          }
				          },
				          function(failure) {
					          if (!done) {
						          done = true;
						          settle(promise, 2, failure);
					          }
				          });
			} catch (error) {
				if (!done) {
					settle(promise, 2, error);
				}
			}
			return;
		}
	}
	promise._state = state;
	promise._value = value;
	var handlers = promise._handlers;
	promise._handlers = [];
	for (var i = 0; i < handlers.length; i++) {
		handle(promise, handlers[i]);
	}
}

// Продолжить цепочку через очередь заданий
function handle(promise, handler) {
	if (!promise._state) {
		promise._handlers.push(handler);
		return;
	}
	enqueue(function( ) {
		var callback = promise._state === 1 ? handler.fulfilled : handler.rejected;
		if (typeof callback !== "function") {
			(promise._state === 1 ? handler.resolve : handler.reject)(promise._value);
			return;
		}
		try {
			handler.resolve(callback(promise._value));
		} catch (error) {
			handler.reject(error);
		}
	});
}

// Создать Promise с защищённым от повторного завершения исполнителем
function Promise(executor) {
	if (!(this instanceof Promise) || typeof executor !== "function") {
		throw new TypeError("Promise requires an executor");
	}
	this._state = 0;
	this._handlers = [];
	var self = this, done = false;
	try {
		executor(
		    function(value) {
			    if (!done) {
				    done = true;
				    settle(self, 1, value);
			    }
		    },
		    function(error) {
			    if (!done) {
				    done = true;
				    settle(self, 2, error);
			    }
		    });
	} catch (error) {
		if (!done) {
			done = true;
			settle(self, 2, error);
		}
	}
}
Promise.prototype.then = function(fulfilled, rejected) {
	var self = this;
	return new Promise(function(resolve, reject) {
		handle(self,
		       { fulfilled : fulfilled, rejected : rejected, resolve : resolve, reject : reject });
	});
};
Promise.prototype["catch"] = function(rejected) {
	return this.then(null, rejected);
};
Promise.prototype["finally"] = function(callback) {
	if (typeof callback !== "function") {
		return this.then(callback, callback);
	}
	return this.then(
	    function(
	        value) { return Promise.resolve(callback( )).then(function( ) { return value; }); },
	    function(
	        error) { return Promise.resolve(callback( )).then(function( ) { throw error; }); });
};
Promise.resolve = function(value) {
	return value instanceof Promise ? value : new Promise(function(resolve) { resolve(value); });
};
Promise.reject = function(error) {
	return new Promise(function(resolve, reject) { reject(error); });
};
Promise.all = function(input) {
	return new Promise(function(resolve, reject) {
		var values = Array.from(input), remaining = values.length;
		if (!remaining) {
			resolve(values);
			return;
		}
		values.forEach(function(value, index) {
			Promise.resolve(value).then(function(result) {
				values[index] = result;
				if (--remaining === 0) {
					resolve(values);
				}
			}, reject);
		});
	});
};
Promise.race = function(input) {
	return new Promise(function(resolve, reject) {
		Array.from(input).forEach(function(
		    value) { Promise.resolve(value).then(resolve, reject); });
	});
};
Promise.allSettled = function(input) {
	return Promise.all(Array.from(input).map(function(value) {
		return Promise.resolve(value).then(
		    function(result) { return { status : "fulfilled", value : result }; },
		    function(error) { return { status : "rejected", reason : error }; });
	}));
};
global.Promise = Promise;

// Нормализовать заголовок до передачи проверяющей стороне C
function headerName(name) {
	name = String(name).toLowerCase( );
	if (!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(name)) {
		throw new TypeError("Invalid header name");
	}
	return name;
}
function headerValue(value) {
	value = String(value).replace(/^[\t ]+|[\t ]+$/g, "");
	if (/[\r\n\x00]/.test(value)) {
		throw new TypeError("Invalid header value");
	}
	return value;
}
function Headers(input) {
	this._values = Object.create(null);
	var self = this;
	if (input instanceof Headers) {
		input.forEach(function(v, k) { self.append(k, v); });
	} else if (Array.isArray(input)) {
		input.forEach(function(p) { self.append(p[0], p[1]); });
	} else if (input) {
		Object.keys(input).forEach(function(k) { self.append(k, input[k]); });
	}
}
Headers.prototype.append = function(name, value) {
	name = headerName(name);
	value = headerValue(value);
	this._values[name] = this.has(name) ? this._values[name] + ", " + value : value;
};
Headers.prototype.set = function(name, value) {
	this._values[headerName(name)] = headerValue(value);
};
Headers.prototype.get = function(name) {
	name = headerName(name);
	return this.has(name) ? this._values[name] : null;
};
Headers.prototype.has = function(name) {
	return Object.prototype.hasOwnProperty.call(this._values, headerName(name));
};
Headers.prototype["delete"] = function(name) {
	delete this._values[headerName(name)];
};
Headers.prototype.forEach = function(callback, owner) {
	var self = this;
	Object.keys(this._values)
	    .forEach(function(name) { callback.call(owner, self._values[name], name, self); });
};
function headerPairs(headers) {
	var pairs = [];
	headers.forEach(function(value, name) { pairs.push([ name, value ]); });
	return pairs;
}
function parseHeaders(text) {
	var headers = new Headers( );
	String(text).split(/\r?\n/).forEach(function(line) {
		var split = line.indexOf(":");
		if (split > 0) {
			headers.append(line.slice(0, split), line.slice(split + 1));
		}
	});
	return headers;
}

// Ограничить тело поддержанными текстовыми представлениями
function textBody(body) {
	if (body === null || body === undefined) {
		return "";
	}
	if (typeof body === "string") {
		return body;
	}
	throw new TypeError("Only string request bodies are currently supported");
}
function Response(body, options) {
	options = options || { };
	this._body = body === undefined || body === null ? "" : String(body);
	this.status = options.status === undefined ? 200 : Number(options.status);
	this.statusText = options.statusText || "";
	this.headers = new Headers(options.headers);
	this.url = options.url || "";
	this.ok = this.status >= 200 && this.status < 300;
	this.type = "basic";
	this.redirected = false;
	this.bodyUsed = false;
}
Response.prototype.text = function( ) {
	if (this.bodyUsed) {
		return Promise.reject(new TypeError("Response body already consumed"));
	}
	this.bodyUsed = true;
	return Promise.resolve(this._body);
};
Response.prototype.json = function( ) {
	return this.text( ).then(function(text) { return JSON.parse(text); });
};
Response.prototype.clone = function( ) {
	if (this.bodyUsed) {
		throw new TypeError("Response body already consumed");
	}
	return new Response(this._body, this);
};
function Request(input, options) {
	options = options || { };
	var previous = input instanceof Request ? input : { };
	this.url = input instanceof Request ? input.url : String(input);
	this.method = String(options.method || previous.method || "GET").toUpperCase( );
	this.headers = new Headers(options.headers || previous.headers);
	this.credentials = options.credentials || previous.credentials || "same-origin";
	this.mode = options.mode || previous.mode || "cors";
	this.signal = options.signal || previous.signal;
	this._body = textBody(options.body === undefined ? previous._body : options.body);
	if (this._body && (this.method === "GET" || this.method === "HEAD")) {
		throw new TypeError("GET and HEAD cannot have a body");
	}
}
Request.prototype.clone = function( ) {
	return new Request(this);
};
global.Headers = Headers;
global.Request = Request;
global.Response = Response;

// Отменять асинхронный запрос через общий сигнал
function AbortSignal( ) {
	this.aborted = false;
	this.reason = undefined;
	this._listeners = [];
}
AbortSignal.prototype.addEventListener = function(name, callback) {
	if (name === "abort" && this._listeners.indexOf(callback) < 0) {
		this._listeners.push(callback);
	}
};
AbortSignal.prototype.removeEventListener = function(name, callback) {
	var index = this._listeners.indexOf(callback);
	if (name === "abort" && index >= 0) {
		this._listeners.splice(index, 1);
	}
};
AbortSignal.prototype.throwIfAborted = function( ) {
	if (this.aborted) {
		throw this.reason;
	}
};
function AbortController( ) { this.signal = new AbortSignal( ); }
AbortController.prototype.abort = function(reason) {
	var signal = this.signal;
	if (signal.aborted) {
		return;
	}
	signal.aborted = true;
	signal.reason = reason || new Error("AbortError");
	signal._listeners.slice( ).forEach(function(
	    callback) { callback.call(signal, { type : "abort", target : signal }); });
	if (typeof signal.onabort === "function") {
		signal.onabort({ type : "abort", target : signal });
	}
};
global.AbortController = AbortController;
global.AbortSignal = AbortSignal;

// Выполнять fetch асинхронно через curl, ошибки сети отклоняют Promise
global.fetch = function(input, options) {
	return new Promise(function(resolve, reject) {
		var request = new Request(input, options), finished = false, id = 0;
		if (request.mode === "no-cors") {
			reject(new TypeError("no-cors mode is not supported"));
			return;
		}
		function abort( ) {
			if (finished) {
				return;
			}
			finished = true;
			if (id) {
				global.__synapseAbort(id);
			}
			var error = new Error("The request was aborted");
			error.name = "AbortError";
			reject(error);
		}
		if (request.signal && request.signal.aborted) {
			abort( );
			return;
		}
		id = global.__synapseHTTP(
		    request.url, request.method, request._body,
		    headerPairs(request.headers).map(function(p) { return p.join(": "); }).join("\r\n"),
		    function(error, status, headers, body, url) {
			    if (finished) {
				    return;
			    }
			    finished = true;
			    if (request.signal) {
				    request.signal.removeEventListener("abort", abort);
			    }
			    if (error) {
				    reject(new TypeError(error));
				    return;
			    }
			    if (status >= 300 && status < 400) {
				    reject(new TypeError("Fetch redirects are not yet supported"));
				    return;
			    }
			    resolve(new Response(
			        body, { status : status, headers : parseHeaders(headers), url : url }));
		    },
		    request.credentials === "omit"      ? 0
		    : request.credentials === "include" ? 2
		                                        : 1,
		    60000, request.mode);
		if (request.signal) {
			request.signal.addEventListener("abort", abort);
		}
	});
};

// Доставить события запросов без зависимости от реализации DOM EventTarget
function dispatch(target, name) {
	var event = {
		type : name,
		target : target,
		currentTarget : target,
		loaded : target.responseText.length,
		lengthComputable : false
	};
	var callback = target["on" + name];
	if (typeof callback === "function") {
		callback.call(target, event);
	}
	var listeners = (target._listeners[name] || []).slice( );
	listeners.forEach(function(listener) {
		if (typeof listener === "function") {
			listener.call(target, event);
		} else if (listener && listener.handleEvent) {
			listener.handleEvent(event);
		}
	});
}
function XMLHttpRequest( ) {
	this.readyState = 0;
	this.status = 0;
	this.statusText = "";
	this.responseText = "";
	this.response = null;
	this.responseURL = "";
	this.responseType = "";
	this.timeout = 0;
	this.withCredentials = false;
	this._headers = new Headers( );
	this._listeners = Object.create(null);
	this._id = 0;
	this._sent = false;
	this._generation = 0;
}
XMLHttpRequest.prototype.addEventListener = function(name, callback) {
	var list = this._listeners[name] || (this._listeners[name] = []);
	if (list.indexOf(callback) < 0) {
		list.push(callback);
	}
};
XMLHttpRequest.prototype.removeEventListener = function(name, callback) {
	var list = this._listeners[name] || [], index = list.indexOf(callback);
	if (index >= 0) {
		list.splice(index, 1);
	}
};
XMLHttpRequest.prototype.open = function(method, url, asynchronous) {
	if (asynchronous === false) {
		throw new Error("Synchronous XMLHttpRequest is not supported");
	}
	if (this._id) {
		global.__synapseAbort(this._id);
	}
	this._generation++;
	this._id = 0;
	this._sent = false;
	this._method = String(method).toUpperCase( );
	this._url = String(url);
	this._headers = new Headers( );
	this._responseHeaders = new Headers( );
	this.status = 0;
	this.response = null;
	this.responseText = "";
	this.readyState = 1;
	dispatch(this, "readystatechange");
};
XMLHttpRequest.prototype.setRequestHeader = function(name, value) {
	if (this.readyState !== 1 || this._sent) {
		throw new Error("InvalidStateError");
	}
	this._headers.append(name, value);
};
XMLHttpRequest.prototype.getResponseHeader = function(name) {
	return this.readyState < 2 ? null : this._responseHeaders.get(name);
};
XMLHttpRequest.prototype.getAllResponseHeaders = function( ) {
	if (this.readyState < 2) {
		return "";
	}
	return headerPairs(this._responseHeaders)
	    .map(function(pair) { return pair.join(": ") + "\r\n"; })
	    .join("");
};
XMLHttpRequest.prototype.abort = function( ) {
	var active = this._sent;
	if (this._id) {
		global.__synapseAbort(this._id);
	}
	this._generation++;
	this._id = 0;
	this._sent = false;
	this.status = 0;
	this.response = null;
	this.responseText = "";
	this.readyState = 0;
	if (active) {
		dispatch(this, "abort");
		dispatch(this, "loadend");
	}
};
XMLHttpRequest.prototype.send = function(body) {
	if (this.readyState !== 1 || this._sent) {
		throw new Error("InvalidStateError");
	}
	if (this.responseType !== "" && this.responseType !== "text" && this.responseType !== "json") {
		throw new TypeError("Unsupported responseType");
	}
	var self = this, generation = this._generation;
	this._sent = true;
	try {
		this._id = global.__synapseHTTP(
		    this._url, this._method, textBody(body),
		    headerPairs(this._headers).map(function(p) { return p.join(": "); }).join("\r\n"),
		    function(error, status, headers, response, url) {
			    if (self._generation !== generation) {
				    return;
			    }
			    self._id = 0;
			    self._sent = false;
			    if (error) {
				    self.status = 0;
				    self.readyState = 4;
				    dispatch(self, "readystatechange");
				    dispatch(self, /timed|timeout/i.test(error) ? "timeout" : "error");
				    dispatch(self, "loadend");
				    return;
			    }
			    self.status = status;
			    self.responseURL = url;
			    self._responseHeaders = parseHeaders(headers);
			    self.readyState = 2;
			    dispatch(self, "readystatechange");
			    if (self._generation !== generation) {
				    return;
			    }
			    self.responseText = response;
			    self.response = response;
			    if (self.responseType === "json") {
				    try {
					    self.response = JSON.parse(response);
				    } catch (ignored) {
					    self.response = null;
				    }
			    }
			    self.readyState = 3;
			    dispatch(self, "readystatechange");
			    if (self._generation !== generation) {
				    return;
			    }
			    dispatch(self, "progress");
			    self.readyState = 4;
			    dispatch(self, "readystatechange");
			    if (self._generation !== generation) {
				    return;
			    }
			    dispatch(self, "load");
			    dispatch(self, "loadend");
		    },
		    this.withCredentials ? 2 : 1, Math.max(0, Math.min(60000, Number(this.timeout) || 0)),
		    "cors");
		dispatch(this, "loadstart");
	} catch (error) {
		this._sent = false;
		throw error;
	}
};
["UNSENT", "OPENED", "HEADERS_RECEIVED", "LOADING", "DONE"].forEach(function(name, index) {
	XMLHttpRequest[name] = index;
	XMLHttpRequest.prototype[name] = index;
});
global.XMLHttpRequest = XMLHttpRequest;

// Читать актуальные данные сайта перед каждой операцией хранилища
function readStorage( ) {
	var parsed = JSON.parse(global.__synapseStorageRead( )), result = Object.create(null);
	Object.keys(parsed).forEach(function(key) { result[key] = String(parsed[key]); });
	return result;
}
function Storage( ) {}
Storage.prototype.getItem = function(key) {
	var values = readStorage( );
	key = String(key);
	return Object.prototype.hasOwnProperty.call(values, key) ? values[key] : null;
};
Storage.prototype.setItem = function(key, value) {
	var values = readStorage( );
	values[String(key)] = String(value);
	global.__synapseStorageWrite(JSON.stringify(values));
};
Storage.prototype.removeItem = function(key) {
	var values = readStorage( );
	delete values[String(key)];
	global.__synapseStorageWrite(JSON.stringify(values));
};
Storage.prototype.clear = function( ) {
	global.__synapseStorageWrite("{}");
};
Storage.prototype.key = function(index) {
	return Object.keys(readStorage( ))[Number(index)] || null;
};
Object.defineProperty(Storage.prototype, "length",
                      { get : function( ) { return Object.keys(readStorage( )).length; } });
global.Storage = Storage;
var storage = new Storage( );
if (typeof global.Proxy === "function") {
	storage = new global.Proxy(storage, {
		get : function(target, key) {
		    if (typeof key !== "string" || key in target) {
			    return target[key];
		    }
		    var value = target.getItem(key);
		    return value === null ? undefined : value;
		},
		set : function(target, key, value) {
		    target.setItem(String(key), value);
		    return true;
		},
		deleteProperty : function(target, key) {
		    target.removeItem(String(key));
		    return true;
		},
		ownKeys : function( ) { return Object.keys(readStorage( )); },
		has : function(target,
		               key) { return key in target || target.getItem(String(key)) !== null; }
	});
}
Object.defineProperty(global, "localStorage", { value : storage });
}(this));
