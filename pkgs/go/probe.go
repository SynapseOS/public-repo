// probe.go: Проверка горутин, памяти и локального HTTP в SynapseOS
// Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
// Подсистема: Go, первое изменение: 22.10.2026, последнее изменение: 22.10.2026
// Статус доступа: общедоступный
package main

import (
	"fmt"
	"io"
	"net"
	"net/http"
	"runtime"
	"sync"
	"time"
)

// Проверить работу runtime и обмен через сетевой опросчик ядра
func main() {
	var workers sync.WaitGroup

	for i := 0; i < 8; i++ {
		workers.Add(1)
		go func() {
			defer workers.Done()
			time.Sleep(10 * time.Millisecond)
		}()
	}

	workers.Wait()
	runtime.GC()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}

	server := &http.Server{
		Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, "SYNAPSE_GO_HTTP_PASS")
		}),
		ReadHeaderTimeout: 5 * time.Second,
	}
	defer server.Close()
	go server.Serve(listener)

	client := &http.Client{Timeout: 10 * time.Second}
	response, err := client.Get("http://" + listener.Addr().String())
	if err != nil {
		panic(err)
	}
	defer response.Body.Close()

	body, err := io.ReadAll(response.Body)
	if err != nil || string(body) != "SYNAPSE_GO_HTTP_PASS" {
		panic("HTTP response mismatch")
	}

	fmt.Println("SYNAPSE_GO_RUNTIME_PASS", runtime.Version(), string(body))
}
