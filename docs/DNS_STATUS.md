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

## 配置示例

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
