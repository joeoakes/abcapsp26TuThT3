Implementation Documentation 

Environment Overview 

**Devices** 

* 
**Laptop (Windows + WSL Ubuntu)** 


* Runs MongoDB 


* Runs Professor's HTTP server (`maze_http_mongo`) 


* Runs Nginx reverse proxy with mTLS 




* 
**Raspberry Pi 4 (RetroPie)** 


* Runs SDL2 Maze game 


* Sends JSON telemetry events 





**Network** 

* Laptop IP on LAN: `192.168.1.190` 


* Pi IP on LAN: `192.168.1.236` 



---

Phase 1: HTTP → MongoDB Server 

Setup (Laptop WSL Terminal) 

1. 
**Clone course repository fork** 


```bash
cd
git clone https://github.com/markj2104/abcapsp26LT.git

```





2. 
**Enter HTTP server folder** 


```bash
cd abcapsp26LT/http

```





3. 
**Install required libraries** 


```bash
sudo apt install -y build-essential pkg-config libmicrohttpd-dev libmongoc-dev libbson-dev

```





4. 
**Compile professor's HTTP → MongoDB server** 


```bash
gcc -O2 -Wall -Wextra -std=c11 maze_http_mongo.c -o maze_http_mongo \
$(pkg-config --cflags --libs libmicrohttpd libmongoc-1.0)

```





5. 
**Run HTTP server (leave running)** 


```bash
./maze_http_mongo

```





* Server listens on: `http://127.0.0.1:8080/move` 




6. 
**Verify server locally (Open a second WSL terminal)** 


```bash
curl -s -X POST http://127.0.0.1:8080/move \
-H "Content-Type: application/json" \
-d '{"event_type":"backend_test"}'

```





**Expected output:**
```json
{"ok":true}

```





7. 
**Verify MongoDB insertion** 


* MongoDB Compass Database: `maze` Collection: `moves` 


* JSON documents appear after POST requests. 





---

Phase 2: Maze JSON Telemetry Client (Pi SSH Terminal) 

1. 
**Install libcurl for HTTP client** 


```bash
sudo apt update
sudo apt install -y libcurl4-openssl-dev

```





2. 
**SDL2 Maze code modification** 


* Added: `#include <curl/curl.h>` 


* Added `send_json_http()` function 


* Replaced `puts(json);` with `send_json_http(json);` 




3. 
**Compile Maze with curl + SDL2** 


```bash
cd ~/abcapsp26LT/maze
gcc maze_sdl2.c -o maze_sdl2 $(sdl2-config --cflags --libs) -lcurl

```





4. 
**Run Maze** 


```bash
./maze_sdl2

```





5. 
**Verify Pi can send HTTP logs to laptop** 


```bash
curl -s -X POST http://192.168.1.190:8080/move \
-H "Content-Type: application/json" \
-d '{"event_type":"pi_test"}'

```





**Expected:**
```json
{"ok":true}

```






---

Phase 3: Adding mTLS Security 

Part A - Certificate Authority and Certificates (Laptop WSL) 

```bash
mkdir ~/mtls
cd ~/mtls

```



1. 
**Create Certificate Authority** 


```bash
openssl genrsa -out ca.key 4096
openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 \
-subj "/CN=Mark-MTLS-CA" -out ca.crt

```





2. 
**Create Server Certificate** 


```bash
openssl genrsa -out server.key 4096
openssl req -new -key server.key -subj "/CN=mark-laptop" -out server.csr
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
-out server.crt -days 825 -sha256

```





3. 
**Create Client Certificate (Pi)** 


```bash
openssl genrsa -out pi.key 4096
openssl req -new -key pi.key -subj "/CN=retropie-pi4" -out pi.csr
openssl x509 -req -in pi.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
-out pi.crt -days 825 -sha256

```






Part B - Nginx mTLS Reverse Proxy (Laptop WSL) 

1. 
**Install nginx** 


```bash
sudo apt install -y nginx

```





2. 
**Configure mTLS site** 


```bash
sudo tee /etc/nginx/sites-available/maze_mtls <<'EOF'
server {
    listen 8443 ssl;
    server_name _;

    ssl_certificate /home/markj2104/mtls/server.crt;
    ssl_certificate_key /home/markj2104/mtls/server.key;
    ssl_client_certificate /home/markj2104/mtls/ca.crt;
    ssl_verify_client on;
    ssl_protocols TLSv1.2 TLSv1.3;

    location /move {
        proxy_pass http://127.0.0.1:8080/move;
        proxy_set_header Host $host;
    }
}
EOF

```





3. 
**Enable site** 


```bash
sudo rm -f /etc/nginx/sites-enabled/default
sudo ln -s /etc/nginx/sites-available/maze_mtls /etc/nginx/sites-enabled/
sudo nginx -t
sudo systemctl restart nginx

```






Part C - Expose mTLS Port to LAN (Windows PowerShell Admin) 

1. 
**Find WSL IP:** 


```powershell
wsl hostname -I

```





* Result Example: `172.23.22.117` 




2. 
**Create portproxy:** 


```powershell
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=8443 connectaddress=172.23.22.117 connectport=8443

```





3. 
**Firewall Rule:** 


```powershell
New-NetFirewallRule -DisplayName "Maze mTLS 8443" -Direction Inbound -Protocol TCP -LocalPort 8443 -Action Allow -Profile Any

```






Part D - Copy Certificates to Pi 

* 
**On Pi:** 


```bash
sudo mkdir -p /etc/mtls
sudo chown pi:pi /etc/mtls

```





* 
**On Laptop WSL:** 


```bash
scp ~/mtls/ca.crt ~/mtls/pi.crt ~/mtls/pi.key pi@192.168.1.236:/etc/mtls/

```





* 
**On Pi:** 


```bash
chmod 600 /etc/mtls/pi.key

```





* Verify: `ls -l /etc/mtls` 





Part E - Verify mTLS Channel (Pi Terminal) 

**Test secure POST:** 

```bash
curl --cacert /etc/mtls/ca.crt \
--cert /etc/mtls/pi.crt \
--key /etc/mtls/pi.key \
--resolve mark-laptop:8443:192.168.1.190 \
https://mark-laptop:8443/move \
-H "Content-Type: application/json" \
-d '{"event_type":"mtls_test"}'

```



**Expected:**

```json
{"ok":true}

```



**This confirmed:** 

* TLS encryption active 


* Client cert required 


* Server identity verified 


* Proxy forwarding to HTTP server working 



---

Phase 4: Updating Maze Code to Use mTLS 

**Added function in `maze_sdl2.c**` 

```c
static void send_json_mtls(const char *json) {
    CURL *curl = curl_easy_init();
    if (!curl) return;

    struct curl_slist *headers = NULL;
    struct curl_slist *resolve = NULL;

    headers = curl_slist_append(headers, "Content-Type: application/json");
    resolve = curl_slist_append(resolve, "mark-laptop:8443:192.168.1.190");

    curl_easy_setopt(curl, CURLOPT_URL, "https://mark-laptop:8443/move");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);

    curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/mtls/ca.crt");
    curl_easy_setopt(curl, CURLOPT_SSLCERT, "/etc/mtls/pi.crt");
    curl_easy_setopt(curl, CURLOPT_SSLKEY, "/etc/mtls/pi.key");

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
    
    curl_easy_perform(curl);
    
    curl_slist_free_all(resolve);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

```



**Modifications:**

* Replaced: `puts(json);` 


* With: `send_json_mtls(json);` 



**Recompile Maze on Pi:** 

```bash
gcc maze_sdl2.c -o maze_sdl2 $(sdl2-config --cflags --libs) -lcurl

```



**Run Maze:** 

```bash
./maze_sdl2

```



**Result:** 

* Each player move sends encrypted mTLS telemetry 


* Nginx authenticates client cert 


* Professor's HTTP server receives JSON 


* MongoDB stores event 



---

Phase 5: GitHub Submission 

**On Pi:** 

```bash
cd ~/abcapsp26LT/maze
git add maze_sdl2.c
git commit -m "Add mTLS-secured JSON logging to Maze client"
git push

```



Security Properties Achieved 

* TLS encryption in transit 


* Server identity verified by CA 


* Client identity verified by Nginx 


* Only authorized Pi can submit logs 


* Backend MongoDB never exposed directly 


* CA private key never copied off laptop