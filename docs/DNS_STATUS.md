# dns_status — 查看 DNS 解析记录

无需 `--with-debug`，通过 HTTP 接口查看：

1. **http/location `resolver` 缓存**（请求时动态解析会写入）
2. **`upstream` + `server ... resolve`** 的 zone peer / template
3. **本 worker 最近的动态解析环形缓冲**（最多 128 条）

## 编译

默认开启。显式开关：

```bash
./configure --with-http_dns_status_module   # 默认即 YES
# 或
./configure --without-http_dns_status_module
make && make install
```

## 配置示例（精简）

```nginx
http {
    resolver 8.8.8.8 valid=30s ipv6=off;

    upstream backend {
        zone backend 64k;
        resolver 8.8.8.8;
        server api.example.com resolve;
    }

    server {
        listen 8080;

        location /dns_status {
            dns_status;
            # 建议限制访问
            # allow 127.0.0.1;
            # deny all;
        }

        location /dyn {
            # 触发“请求时动态解析”
            set $backend_host api.example.com;
            proxy_pass http://$backend_host:80;
        }

        location / {
            proxy_pass http://backend;
        }
    }
}
```

## 完整示例：8080 反代百度 HTTPS + dns_status

可直接写入 `conf/nginx.conf`（前缀按实际安装路径，如 `/usr/local/nginx`）：

```nginx
worker_processes  1;

error_log  logs/error.log  info;
pid        logs/nginx.pid;

events {
    worker_connections  1024;
}

http {
    include       mime.types;
    default_type  application/octet-stream;

    sendfile        on;
    keepalive_timeout  65;

    # 动态解析 / resolve 都需要
    resolver 8.8.8.8 114.114.114.114 valid=30s ipv6=off;
    resolver_timeout 5s;

    # 上游：HTTPS + 运行时 DNS（server ... resolve）
    upstream baidu_upstream {
        zone baidu_upstream 64k;
        resolver 8.8.8.8 114.114.114.114 valid=30s ipv6=off;
        resolver_timeout 5s;

        # 百度常用主机名；443 + resolve
        server www.baidu.com:443 resolve;
    }

    server {
        listen       8080;
        server_name  localhost;

        # DNS / resolve 查看
        location /dns_status {
            dns_status;
            allow 127.0.0.1;
            allow ::1;
            deny all;
        }

        # 请求时动态解析（可选；访问后可在 recent resolve 段看到）
        location /dyn {
            set $backend www.baidu.com;
            proxy_pass https://$backend:443;
            proxy_ssl_server_name on;
            proxy_ssl_name www.baidu.com;
            proxy_set_header Host www.baidu.com;
        }

        # 反代到百度 HTTPS
        location / {
            proxy_pass https://baidu_upstream;

            proxy_http_version 1.1;
            proxy_set_header Host www.baidu.com;
            proxy_set_header Connection "";

            # 上游是 HTTPS 时建议打开
            proxy_ssl_server_name on;
            proxy_ssl_name www.baidu.com;
            proxy_ssl_protocols TLSv1.2 TLSv1.3;

            proxy_connect_timeout 10s;
            proxy_send_timeout 60s;
            proxy_read_timeout 60s;
        }
    }
}
```

| 项 | 说明 |
|---|---|
| `zone` + `resolve` | 运行时解析 `www.baidu.com`，结果在 `/dns_status` 的 upstream 段 |
| `proxy_pass https://...` | 对上游走 HTTPS |
| `proxy_ssl_server_name` / `proxy_ssl_name` | SNI，访问百度 HTTPS 一般需要 |
| `Host www.baidu.com` | 上游按该 Host 回响应 |
| `/dyn` | 变量 `proxy_pass`，触发 location resolver 动态解析 |

```bash
cd /usr/local/nginx/sbin
./nginx -t
./nginx                 # 若已在跑：./nginx -s reload

# 测代理（确认 8080 是本 nginx，不是 Portainer 等）
curl -I http://127.0.0.1:8080/

# 触发动态解析后再看 DNS
curl -s http://127.0.0.1:8080/dyn >/dev/null
curl -s http://127.0.0.1:8080/dns_status
```

## 查看命令

```bash
# 查看当前 worker 的 DNS / resolve 状态
curl -s http://127.0.0.1:8080/dns_status

# 先打一笔动态解析，再查看
curl -s http://127.0.0.1:8080/dyn >/dev/null
curl -s http://127.0.0.1:8080/dns_status
```

输出分段：

| 段 | 含义 |
|---|---|
| `http/server/location resolver cache` | `resolver` DNS 服务器 + `name_cache` |
| `upstream server ... resolve` | 带 `zone` 的 upstream 解析结果 peer |
| `recent resolve events` | 本 worker 最近解析事件（动态 proxy_pass + zone resolve） |

> 多 worker 时每个 worker 缓存与动态环形缓冲独立；`upstream zone` peer 为共享内存，各 worker 看到的 peer 列表一致。
