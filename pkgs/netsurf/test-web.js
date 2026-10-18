// test-web.js: проверка сетевых интерфейсов и хранения данных в гостевом NetSurf
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Duktape ES5 и HTTP
// Подсистема: проверки браузера
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
(function( ) {
"use strict";
var passed = [], failed = [], order = [];
function check(name, condition) { (condition ? passed : failed).push(name); }
function rejected(name, promise) {
	return promise.then(function( ) { check(name, false); }, function( ) { check(name, true); });
}
function xhr(method, url, body, timeout) {
	return new Promise(function(resolve, reject) {
		var request = new XMLHttpRequest( ), states = [];
		request.onreadystatechange = function( ) { states.push(request.readyState); };
		request.open(method, url);
		request.timeout = timeout || 0;
		request.onload = function( ) { resolve({ request : request, states : states }); };
		request.onerror = function( ) { reject(new Error("XHR error")); };
		request.ontimeout = function( ) { reject(new Error("XHR timeout")); };
		if (body) {
			request.setRequestHeader("Content-Type", "application/json");
		}
		request.send(body || null);
	});
}
setTimeout(function( ) { order.push("timer"); }, 0);
Promise.resolve(42).then(function(value) {
	order.push("microtask");
	check("promise-value", value === 42);
});
order.push("sync");
var done =
    Promise.resolve( )
        .then(function( ) { return new Promise(function(resolve) { setTimeout(resolve, 20); }); })
        .then(function( ) {
	        check("microtask-order=" + order.join(","), order.join(",") === "sync,microtask,timer");
	        return Promise.resolve({
		        then : function(resolve) {
		            resolve(42);
		            resolve(99);
		        }
	        });
        })
        .then(function(value) {
	        check("thenable-once", value === 42);
	        return Promise.reject("expected")["catch"](function(
	            error) { check("promise-reject", error === "expected"); });
        })
        .then(function( ) {
	        return Promise.all([ Promise.resolve(1), 2 ])
	            .then(function(values) { check("promise-all", values.join(",") === "1,2"); });
        })
        .then(function( ) {
	        localStorage.setItem("synapse-test", "тест42");
	        check("storage-get", localStorage.getItem("synapse-test") === "тест42");
	        localStorage["synapse-property"] = "value";
	        check("storage-property", localStorage.getItem("synapse-property") === "value");
	        delete localStorage["synapse-property"];
	        localStorage.removeItem("synapse-test");
	        check("storage-remove", localStorage.getItem("synapse-test") === null);
	        localStorage.setItem("synapse-persist", "42");
	        return fetch("/api")
	            .then(function(response) {
		            check("fetch-status-headers",
		                  response.ok && response.status === 200 &&
		                      response.headers.get("content-type").indexOf("application/json") ===
		                          0);
		            return response.json( );
	            })
	            .then(function(data) { check("fetch-json", data.value === 42); });
        })
        .then(function( ) {
	        return fetch("/not-found").then(function(response) {
		        check("fetch-http-error-resolves", response.status === 404 && !response.ok);
	        });
        })
        .then(function( ) {
	        return fetch("/echo", {
		               method : "POST",
		               headers : { "Content-Type" : "application/json" },
		               body : "{\"n\":42}"
	               })
	            .then(function(response) { return response.json( ); })
	            .then(function(data) { check("fetch-post", data.body === "{\"n\":42}"); });
        })
        .then(function( ) {
	        return xhr("GET", "/api").then(function(result) {
		        check("xhr-get", result.request.status === 200 &&
		                             JSON.parse(result.request.responseText).value === 42);
		        check("xhr-states", result.states.join(",") === "1,2,3,4");
	        });
        })
        .then(function( ) {
	        return xhr("POST", "/echo", "xhr-body").then(function(result) {
		        check("xhr-post", result.request.status === 201 &&
		                              JSON.parse(result.request.responseText).body === "xhr-body");
	        });
        })
        .then(function( ) { return rejected("xhr-timeout", xhr("GET", "/slow", null, 30)); })
        .then(function( ) {
	        return new Promise(function(resolve) {
		        var request = new XMLHttpRequest( );
		        request.open("GET", "/slow");
		        request.onabort = function( ) {
			        check("xhr-abort", request.status === 0);
			        resolve( );
		        };
		        request.send( );
		        request.abort( );
	        });
        })
        .then(function( ) {
	        var controller = new AbortController( );
	        var promise = fetch("/slow", { signal : controller.signal });
	        controller.abort( );
	        return rejected("fetch-abort", promise);
        })
        .then(function( ) {
	        return fetch("http://10.0.2.2:8877/cors")
	            .then(function(response) { return response.json( ); })
	            .then(function(data) { check("cors-allowed", data.value === 42); });
        })
        .then(function( ) { return rejected("cors-blocked", fetch("http://10.0.2.2:8877/api")); })
        .then(function( ) {
	        return rejected("fetch-same-origin-mode",
	                        fetch("http://10.0.2.2:8877/cors", { mode : "same-origin" }));
        })
        .then(function( ) {
	        return new Promise(function(resolve) {
		        window.__synapseHTTP("http://10.0.2.2:8877/api", "GET", "",
		                             "", function(error, status, headers, body) {
			                             check("native-cors-no-leak", !!error && status === 0 &&
			                                                              headers === "" &&
			                                                              body === "");
			                             resolve( );
		                             }, 0, 1000, "cors");
	        });
        })
        .then(function( ) {
	        return rejected("forbidden-header",
	                        fetch("/api", { headers : { "Cookie" : "injected=1" } }));
        })
        .then(
            function( ) { return rejected("tls-untrusted", fetch("https://10.0.2.2:8878/cors")); });
done = done.then(function( ) {
	fetch("/slow");
	return new Promise(function(resolve) { setTimeout(resolve, 40); });
});
done["catch"](function(error) { failed.push("unexpected: " + String(error)); }).then(function( ) {
	window.location.href = "/web-result?passed=" + encodeURIComponent(JSON.stringify(passed)) +
	                       "&failed=" + encodeURIComponent(JSON.stringify(failed)) + "&pending=1";
});
}( ));
