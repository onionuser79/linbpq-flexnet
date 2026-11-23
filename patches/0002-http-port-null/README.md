# 0002 — Web request without a port number crashes the node

| | |
|---|---|
| **Affects** | Upstream LinBPQ (verified in 6.0.26.1) and linbpq-flexnet up to v2.11.4 |
| **File** | `HTTPcode.c`, `InnerProcessHTTPMessage()` — two lines |
| **FlexNet-specific** | No. Any LinBPQ node with `HTTPPORT` set is affected |
| **Status** | Included in linbpq-flexnet **v2.11.5**. The patch here is for stock LinBPQ |

## Symptom

The node process dies. The debug log ends with:

```
SIGSEGV Received
... libc.so.6(atoi+0x18)  atoi.c:28 -- __GI_atoi
... linbpq  HTTPcode.c:3810 -- InnerProcessHTTPMessage
... linbpq  HTTPcode.c:4732 -- ProcessHTTPMessage
```

It is triggered by a single unauthenticated request to the web port:

```
GET /Node/PortStats HTTP/1.0
GET /Node/PortBeacons HTTP/1.0
```

The node's own pages always add `?<port>`, so normal use never hits it;
crawlers and scanners that strip query strings do.

## Cause

Both handlers take the port number with

```c
char * PortChar = strtok_s(NULL, "&", &Context);
int PortNo = atoi(PortChar);
```

With no `?` in the URL there is no token, `PortChar` is `NULL`, and
`atoi(NULL)` faults. The HTTP handler runs on its own thread but a
SIGSEGV ends the whole process.

## Fix

```c
int PortNo = PortChar ? atoi(PortChar) : 0;
```

Port 0 takes the existing "unknown port" paths: `PortStats` answers
`Invalid Port`; `PortBeacons` renders the same page as `?0`.

## Apply (stock LinBPQ 6.0.26.1)

```bash
cd ~/linbpq-build
patch -p1 < http-port-null-upstream-6.0.26.1.patch
make
```

The patch keeps upstream's CRLF line endings.

## Check

```bash
curl -s -o /dev/null -w '%{http_code}\n' http://NODE:HTTPPORT/Node/PortStats
```

Before: no reply, the node is gone. After: a reply (`Invalid Port` page
or the sign-on redirect), and the node keeps running.
