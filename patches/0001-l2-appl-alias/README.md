# 0001 — L2 connect to an aliased APPLICATION runs a truncated alias

| | |
|---|---|
| **Affects** | Upstream LinBPQ (verified in 6.0.25.41) and linbpq-flexnet v2.3.0 |
| **File** | `L2Code.c`, `L2SABM()` — one line |
| **FlexNet-specific** | No. Any AX.25 (L2) connect to an aliased APPLICATION callsign is affected |
| **Status** | Compiles cleanly; not yet verified on a live node. Will be part of the next linbpq-flexnet release once it has been |
| **Reported in** | [Issue #1](https://github.com/onionuser79/linbpq-flexnet/issues/1) |

## Symptom

An APPLICATION whose command is an alias that makes an outward Telnet
connection:

```
APPLICATION 3,DX,ATTACH 2 127.0.0.1 63000 S,DXCL,DXCLUS,255
```

works when a local user types `C DXCL` or `DX`, and also over NET/ROM.
A user arriving with an AX.25 connect to `DXCL`, either directly or
through FlexNet, gets the link up and then:

| Telnet port setting | What the user sees |
|---|---|
| `SECURETELNET=1` | `Error - Telnet Outward Connect needs SYSOP Status` |
| `SECURETELNET=0` | `Error - Invalid Command` |

## Cause

When an L2 SABM is accepted for an APPLICATION callsign that has an
alias, `L2SABM()` queues a command on the new session as if the user
had typed it:

```c
memcpy(Msg->L2DATA, ALIASPTR, 12);
Msg->L2DATA[12] = 13;
```

`ALIASPTR` points at the **alias text**, so the session receives only
its first 12 characters, `ATTACH 2 127`, and runs it with the
connecting user's (non-sysop) rights:

- `ATTACHCMD()` copies the session's `Secure_Session` (0) to the Telnet
  stream and sends it `C 127`.
- With `SECURETELNET=1` the Telnet driver refuses an outward connect
  from a non-secure session.
- With `SECURETELNET=0` it parses host `127` with no port, so the port
  is 0, and the command is rejected as invalid.

The NET/ROM path (`L4Code.c`, connect request to an application) does
this correctly. It queues the APPL **command name** (`DX`). The command
handler's `APPLCMD()` then copies the whole alias and runs it with
`Secure_Session` set for that one command, a rule upstream applies on
purpose ("Set secure session for application alias in case telnet
outward connect"). The patch makes the L2 path do the same.

A local `C DXCL` works because the CONNECT-to-application branch in
`Cmd.c` uses the whole alias, and a sysop's console session is already
secure.

## The change

```diff
-			memcpy(Msg->L2DATA, ALIASPTR, 12);
+			memcpy(Msg->L2DATA, LINK->ApplName, 12);
```

`LINK->ApplName` is set to the matched application's `APPLCMD` when the
frame is accepted for an application callsign (`L2FORUS`). It is the
same space-padded 12-character name the NET/ROM path queues.

**Behaviour change to be aware of:** an L2 user connecting to an aliased
application now runs that alias with temporary sysop status, as NET/ROM
users and local users typing the APPL command already do. This is the
rule LinBPQ already applies on those paths. Before applying, check that
none of your application aliases does something you would not let any
connecting user do.

## Files

| Patch | Apply to | Line endings |
|---|---|---|
| `l2-appl-alias-upstream-6.0.25.41.patch` | Stock LinBPQ 6.0.25.41 source tree | CRLF, as upstream's `L2Code.c` |
| `l2-appl-alias-linbpq-flexnet-v2.3.0.patch` | linbpq-flexnet v2.3.0 `L2Code.c` | LF |

Download the files rather than copying them from a web page: the
upstream patch has to keep its CRLF line endings to apply.

## Applying

Stock LinBPQ:

```bash
cd ~/linbpq-build
patch -p1 --dry-run < ~/linbpq-flexnet/patches/0001-l2-appl-alias/l2-appl-alias-upstream-6.0.25.41.patch
patch -p1           < ~/linbpq-flexnet/patches/0001-l2-appl-alias/l2-appl-alias-upstream-6.0.25.41.patch
make
```

linbpq-flexnet v2.3.0, in the build tree after the overlay step of the
README's installation:

```bash
cd ~/linbpq-build
patch -p1 --dry-run < ~/linbpq-flexnet/patches/0001-l2-appl-alias/l2-appl-alias-linbpq-flexnet-v2.3.0.patch
patch -p1           < ~/linbpq-flexnet/patches/0001-l2-appl-alias/l2-appl-alias-linbpq-flexnet-v2.3.0.patch
make
```

Copying the overlay again replaces `L2Code.c` and undoes the patch.
Re-apply it after every overlay copy until it is part of a release.

## Verifying

With the original `ATTACH ... 127.0.0.1 <port>` alias and
`SECURETELNET=1`, an AX.25 connect to the application callsign from
another node should reach the application. NET/ROM connects and local
`C` should behave as before.

## Workaround without patching

Use the `CMDPORT` form, which fits in 12 characters and does not need
sysop status:

```
; Telnet port configuration: add the application's port to CMDPORT
CMDPORT 63000

APPLICATION 3,DX,C 2 HOST 0 S,DXCL,DXCLUS,255
```

`HOST n` indexes the `CMDPORT` list from **zero**. The Telnet driver
does not check `SECURETELNET` for `HOST` connects, and Telnet ports
allow gateway connects, so a non-sysop session gets through. The `HOST`
form sends the user's callsign to the application on connect.
