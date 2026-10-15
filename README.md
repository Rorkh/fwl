# Talking to a Wayland server with no libraries

How to open a window from raw machine code (FASM / `syscall`, no libc, no `libwayland`)
by speaking the Wayland wire protocol directly over a Unix domain socket.

Target assumed throughout: **Linux x86_64**, little-endian, static ELF (this repo's setup).

> Scope note: "no dependencies" means no *userspace* libraries. You still depend on
> (a) the kernel via raw syscalls and (b) the compositor's advertised protocol.
> There is no way to reach a Wayland surface without the compositor's socket.

---

## 0. Mental model

Wayland is **not** a rendering API. It is a small RPC protocol over a `SOCK_STREAM`
Unix socket. You send *requests* (client -> server) and receive *events*
(server -> client). Every object is addressed by a 32-bit **object ID**. You, the
client, allocate IDs for objects you create; the server allocates IDs for objects it
creates. Both sides marshal arguments into a binary wire format you implement yourself.

Rendering is decoupled: you hand the compositor a `wl_buffer` that is just a region of
**shared memory** you mmap'd and drew into. The compositor composites it.

Core objects you will use for one window:

```
wl_display            (always id 1)     connection to the server
  wl_registry         (you create)      enumerates server globals
    wl_compositor     (you bind)        makes surfaces
      wl_surface      (you create)      a rectangle the compositor draws
    wl_shm            (you bind)        shared-memory buffer factory
      wl_shm_pool     (you create)
        wl_buffer    (you create)       pixels
    xdg_wm_base       (you bind)        desktop window shell
      xdg_surface     (you create)      role layer on a wl_surface
        xdg_toplevel  (you create)      the actual top-level window
  wl_callback         (you create)      one-shot sync / frame pacing
```

`wl_shell` is dead; use **xdg-shell** (from the `wayland-protocols` package).

---

## 1. Connect to the compositor

The socket path is `<XDG_RUNTIME_DIR>/<WAYLAND_DISPLAY>`.

- `WAYLAND_DISPLAY` defaults to `wayland-0` if unset.
- `XDG_RUNTIME_DIR` is typically `/run/user/<uid>` (check `id -u`).

At process entry (`_start`) you have no libc, so read the environment from the initial
stack: `[rsp]` = argc, then `argv[argc+1]` = NULL, then `envp[0..]` = "KEY=VALUE" strings.
Walk that to find `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY`, or just hardcode the path
during bring-up.

Unix socket setup (raw syscalls):

```asm
; struct sockaddr_un { u16 sun_family; char sun_path[108]; }
; socket(AF_UNIX=1, SOCK_STREAM=1, 0)          ; eax = 41
; connect(fd, &addr, 2 + strlen(path) + 1)     ; eax = 42
```

Notes:
- `sin_family` field is named `sun_family`; set it to `AF_UNIX` (= `1`).
- Pass the *actual* length of `sun_path` (including the trailing NUL), not `sizeof`.
- Use a blocking socket. `poll`/`epoll` when you build a real loop.

---

## 2. The wire format (implement this exactly)

All integers are **native endianness** (little-endian on x86_64). Every message begins
with an **8-byte header**:

```
offset 0 : uint32  object_id     ; the object the request/event targets
offset 4 : uint16  opcode        ; low half of the 2nd word
offset 6 : uint16  size          ; high half of the 2nd word; INCLUDES the 8-byte header
```

Equivalent on little-endian: second dword `W = (size << 16) | opcode`, stored as
`[opcode_lo, opcode_hi, size_lo, size_hi]`.

After the header come the arguments, packed in declaration order. Alignment:

| arg type            | encoding on the wire                                                        |
|---------------------|-----------------------------------------------------------------------------|
| `int` / `uint` / `fixed` | 4 bytes                                                                 |
| `object`            | 4 bytes (object ID)                                                          |
| `new_id`            | 4 bytes (an ID **you** pick; reserve it before sending)                      |
| `new_id` + `s`+`u` (dynamic interface, e.g. `wl_registry.bind`) | the interface `string`, then `uint` version, then the `new_id` |
| `string`            | `uint` length (**including** the NUL), bytes **including** the NUL, pad to 4-byte align |
| `array`             | `uint` length, bytes, pad to 4-byte align (no NUL)                           |
| `fd` (`h`)          | **0 bytes on the wire** - the descriptor travels only as ancillary data (see §3) |

Rules that trip people up:
- `size` must be a multiple of 4. Pad strings/arrays so the whole message stays aligned.
- A null string is length 0 and still occupies the `uint` + NUL/pad.
- Object ID `0` means "null object" (valid for nullable `object` args).
- `wl_surface.commit` (and other commits) is what *applies* pending state. Requests
  before a commit are staged and invisible until the commit.

---

## 3. Passing file descriptors (critical for buffers)

`wl_shm.create_pool` takes an `fd`. FDs are **not** sent in the byte stream at all; they
travel out-of-band as `SCM_RIGHTS` ancillary data attached to `sendmsg`. An `fd` argument
therefore contributes **no bytes** to the message body - only the other arguments are
marshalled. The order of descriptors in the ancillary data matches the order of `fd`
arguments on the wire, so send the message carrying an fd on its own so the mapping is
unambiguous.

Sending:

```
sendmsg(sockfd, msghdr, 0)                     ; eax = 46
  msghdr {
    void  *msg_name;        = NULL
    u32    msg_namelen;     = 0
    iovec *msg_iov;         -> the marshalled message bytes
    u64    msg_iovlen;      = 1
    void  *msg_control;     -> cmsghdr + fd
    u64    msg_controllen;
    int    msg_flags;
  }
  cmsghdr {
    u64 cmsg_len;                  = CMSG_LEN(4)
    int cmsg_level;                = SOL_SOCKET (1)
    int cmsg_type;                 = SCM_RIGHTS (1)
    int fds[];                     = the fd
  }
```

Receiving: pass a control buffer large enough for a `cmsghdr` + `sizeof(int) * N`, then
walk `Cmsg` records to collect fds. Use `recvmsg` (eax = 47) so you don't drop fds the
server sends back.

---

## 4. Bootstrap: registry -> bind

1. `wl_display` is **object ID 1**, always.
2. Create a registry:
   `wl_display.get_registry(new_id=2)` — opcode **1**, one `new_id` arg.
3. **Roundtrip** to learn globals: send `wl_display.sync(new_id=3)` — opcode **0** — then
   read until you get `wl_callback.done` (see §6). (Or read globals until you see the
   globals you need.)
4. The server sends `wl_registry.global(name:uint, interface:string, version:uint)`
   — event opcode **0** — for each global. Match `interface` strings:
   `"wl_compositor"`, `"wl_shm"`, `"xdg_wm_base"` (and later `"wl_seat"`, `"wl_output"`).
5. Bind each you need:
   `wl_registry.bind(name:uint, interface:string, version:uint, new_id)` — opcode **0**.
   Pick a version you understand (e.g. `wl_compositor` v4, `wl_shm` v1,
   `xdg_wm_base` v1+). The server caps you at its advertised `version`.

`wl_registry.global_remove(name)` is event opcode **1** (a global vanished; ignore for now).

---

## 5. Create the window

Object you create and their opcodes (authoritative source: see §8):

**wl_compositor**
```
create_surface(new_id wl_surface)      opcode 0
create_region(new_id wl_region)        opcode 1
```

**wl_surface** (requests)
```
0 destroy              1 attach(buffer, x, y)       2 damage(x,y,w,h)
3 frame(new_id wl_callback)                         4 set_opaque_region(region)
5 set_input_region(region)                          6 commit
7 set_buffer_transform(int)                         8 set_buffer_scale(int)
9 damage_buffer(x,y,w,h)   (v4+)
```
Surface events: `enter`, `leave`, `preferred_buffer_scale`, `preferred_buffer_transform`.

**xdg_wm_base** (requests: `0 destroy`, `1 create_positioner`, `2 get_xdg_surface`, `3 pong`)
```
get_xdg_surface(new_id xdg_surface, wl_surface surface)   opcode 2
pong(uint serial)                                         opcode 3
```
Events: `ping(uint serial)` = opcode 0. **You must reply with `pong(serial)`** or the
compositor may mark you unresponsive and kill the connection.

**xdg_surface** (requests)
```
0 destroy   1 get_toplevel(new_id xdg_toplevel)   2 get_popup(...)
3 set_window_geometry(x,y,w,h)   4 ack_configure(uint serial)
```
Events: `configure(uint serial)` = 0, `close()` = 1.

**xdg_toplevel** (requests)
```
0 destroy            1 set_parent(parent)          2 set_title(string)
3 set_app_id(string)  4 show_window_menu(...)       5 move(seat, serial)
6 resize(...)         7 set_max_size(w,h)          8 set_min_size(w,h)
9 set_maximized()    10 unset_maximized()         11 set_fullscreen(output)
12 unset_fullscreen() 13 set_minimized()
```
Events: `configure(w:int,h:int,states:array)`, `close()`, `configure_bounds(...)`,
`wm_capabilities(...)`. `configure` carries the size you must adopt - see *Sizing*
under the mapping sequence below.

**Canonical top-level mapping sequence**
1. `wl_compositor.create_surface` -> `surface`
2. `xdg_wm_base.get_xdg_surface(new_id, surface)` -> `xdg_surface`
3. `xdg_surface.get_toplevel(new_id)` -> `toplevel`
4. `xdg_toplevel.set_title("...")`, `set_app_id("...")`
5. `wl_surface.commit()` **with no buffer attached** -> server sends `xdg_surface.configure`
6. Wait for `xdg_surface.configure(serial)`
7. `xdg_surface.ack_configure(serial)` (opcode 4) — mandatory
8. attach a buffer (§6), `wl_surface.commit()` -> window becomes mapped/visible

### Sizing: honor `xdg_toplevel.configure` (tiling WMs)

The compositor, not you, owns the window size. Every `xdg_toplevel.configure`
(`iia` = `width:int, height:int, states:array`) carries a size you should adopt:

- **`width > 0 && height > 0`**: the compositor wants exactly that size. This is
  what tiling WMs (sway, dwl/MangoWM, ...), kiosk shells, `set_maximized` and
  fullscreen send. Resize your `wl_buffer` to match (rebuild the pool + buffer at
  the new width/height and attach that). If you ignore it, the surface keeps
  whatever size *you* picked and the tile is left mostly empty.
- **`width == 0 && height == 0`**: the compositor has no opinion - keep the size
  you chose. This is what floating compositors send.

Notes:
- `xdg_toplevel.configure` is separate from `xdg_surface.configure`; you still
  `ack_configure(serial)` the latter (opcode 4) for every one. `xdg_toplevel.configure`
  can arrive many times per session (another tile opens/closes, drag-resize,
  maximize), so handle it on every event, not just at startup.
- Apply a resize just **before the next `attach`+`commit`**, while the previous
  buffer has already been released (see the `wl_buffer.release` note in §6).
- The old pool/buffer/mapping are safe to free immediately after switching:
  `wl_buffer.destroy` (opcode 0) and `wl_shm_pool.destroy` (opcode 1), then
  `munmap`/`close`. The compositor defers destroying a buffer still in use, and it
  holds its own dup of the fd (`SCM_RIGHTS`, §3) and its own mapping, so the pages
  stay alive until it releases the buffer.

---

## 6. Pixels: shared memory via `wl_shm`

Create the backing memory without libc using `memfd_create`:

```asm
; memfd_create(name_ptr, MFD_CLOEXEC=1)     ; eax = 319  -> fd
; ftruncate(fd, size)                        ; eax = 77
; mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED=1, fd, 0)   ; eax = 9
```

Then share it with the compositor:
1. `wl_shm.create_pool(new_id wl_shm_pool, fd, size:int)` — opcode **0** (carry the fd
   via `SCM_RIGHTS`, §3).
2. `wl_shm_pool.create_buffer(new_id wl_buffer, offset, width, height, stride, format)`
   — opcode **0**. `stride = width * 4`, `format` is one of `wl_shm.format`:
   `ARGB8888 = 0`, `XRGB8888 = 1` (little-endian XRGB = bytes B,G,R,X).
3. Draw into the mmap'd memory (e.g. write `0xFFRRGGBB` dwords).
4. `wl_surface.attach(buffer, 0, 0)` — opcode **1** (buffer 0 = detach).
5. `wl_surface.damage(0,0,w,h)` — opcode **2**.
6. `wl_surface.commit()` — opcode **6**.

Lifetime:
- The compositor sends `wl_buffer.release` (event opcode 0) when it's done reading.
  Don't redraw that buffer until then (or allocate 2+ buffers and cycle).
- For smooth animation, request a `wl_surface.frame(new_id wl_callback)` (opcode 3) and
  redraw only after it fires `wl_callback.done`. This is the vsync signal.
- To resize, rebuild the pool + buffer at the new `width`/`height` and attach that
  (the compositor drives this through `xdg_toplevel.configure`, see §5).

---

## 7. Event loop and message parsing

Receive side:

1. `recvmsg` into a buffer, collecting any `SCM_RIGHTS` fds.
2. Parse messages from the buffer: read the 8-byte header
   (`object_id`, `opcode`, `size`), then `size - 8` argument bytes.
3. Multiple messages can arrive in one `recv`; a message can also be split across reads.
   **Buffer and reassemble** — never assume one read == one message.
4. Dispatch by `(object_id, opcode)`.

Send side: you can `send`/`sendmsg` each request immediately (simplest). `wl_display.flush`
is a libwayland concept, not a wire message — with raw sockets, sending *is* flushing.
Batch multiple messages into one `sendmsg` later if you care about syscall count.

ID bookkeeping: keep a monotonic allocator for IDs you create (start at 2; `1` is the
display). You may never reuse an ID the server hasn't freed. The easy safe policy is:
**never reuse IDs**. `wl_display.delete_id` (event opcode **1**) tells you a server-side ID
is gone; if you never reuse client IDs, you can ignore it. Do not allocate IDs ≥
`0xff000000` (libwayland reserves that range for the server).

Handle error events: `wl_display.error(object_id, code:uint, message:string)` — event
opcode **0**. On this you are about to be disconnected; log and exit.

---

## 8. How to get exact opcodes/signatures (do this, don't trust tables)

The tables above are the well-known core, but interfaces evolve. Generate ground truth
from the protocol XML with `wayland-scanner`:

```bash
# Core protocol
wayland-scanner client-header /usr/share/wayland/wayland.xml  wl.h
wayland-scanner private-code  /usr/share/wayland/wayland.xml  wl.c

# xdg-shell (Debian/Ubuntu: wayland-protocols package)
wayland-scanner client-header \
  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml  xdg.h
wayland-scanner private-code  \
  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml  xdg.c
```

- The generated **header** contains `#define WL_DISPLAY_GET_REGISTRY 1`,
  `#define XDG_SURFACE_ACK_CONFIGURE 4`, etc. -> those integers *are* the opcodes.
- The generated **C** file lists each interface's messages as
  `{ "create_surface", "n", types }` **in opcode order**, with signature strings:
  `i` int, `u` uint, `f` fixed, `s` string, `o` object, `n` new_id, `a` array, `h` fd.
  The Nth entry is opcode N-... (0-based index = opcode).
- On some distros the package is `wayland-devel`/`libwayland-dev`; XML also ships in the
  `wayland` and `wayland-protocols` source trees.

To strip the strings your client sends (interface names in `bind`) you can hardcode
them; the generated header's `wl_interface` structs show the exact bytes.

---

## 9. Debugging

- `WAYLAND_DEBUG=1 ./some_libwayland_client` — libwayland logs every marshalled
  request/event to stderr. Run a known-good client and copy its byte sequence.
- `wayland-info` — lists all globals, versions, and formats the compositor advertises.
- `strace -f -e trace=socket,connect,sendmsg,recvmsg,mmap,memfd_create ./yourclient` —
  verify your syscalls and see raw byte lengths (watch for the `SCM_RIGHTS` fd on pool
  creation).
- Dump and hexdecode your own sends; compare header `size` against your buffer length.
- Cheapest bring-up: point your client at a **nested compositor** you control
  (`weston --backend=headless`, `cage`, `sway`, `labwc`) so you don't have to debug
  against a full desktop session, and set `WAYLAND_DISPLAY` to its socket.

---

## 10. References

Primary specification
- Wayland docs index: https://wayland.freedesktop.org/docs/html/
- Wire format & protocol (the important chapter): https://wayland.freedesktop.org/docs/html/ch04.html
- The Wayland Book (free, authoritative, has a wire-format chapter):
  https://wayland-book.com

Protocol definitions (the ground truth you marshal against)
- Core `wayland.xml`: https://gitlab.freedesktop.org/wayland/wayland/-/blob/main/protocol/wayland.xml
- xdg-shell: https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/main/stable/xdg-shell/xdg-shell.xml
- wayland-protocols repo (seat, output, decoration, etc.): https://gitlab.freedesktop.org/wayland/wayland-protocols

Reference clients without libwayland (closest to what you are building)
- `hello-wayland` (C, raw protocol, single window): https://github.com/emersion/hello-wayland
- `wlclient`/minimal examples from the wlroots author's blog: https://emersion.fr/blog/
- `wayland-scanner` output (any `.c` from the two XML files above) as a marshalling oracle

Servers you can test against
- wlroots / sway: https://gitlab.freedesktop.org/wlroots/wlroots
- weston: https://gitlab.freedesktop.org/wayland/weston

Kernel interfaces you need (no libc)
- `memfd_create(2)` — https://man7.org/linux/man-pages/man2/memfd_create.2.html
- `socket(2)` / `connect(2)` (AF_UNIX) — https://man7.org/linux/man-pages/man7/unix.7.html
- `sendmsg(2)` / `recvmsg(2)` + `SCM_RIGHTS` — https://man7.org/linux/man-pages/man3/cmsg.3.html
- `mmap(2)`, `ftruncate(2)`, `poll(2)`/`epoll(7)`

x86_64 syscall numbers used here (RAX = number):
`read 0, write 1, close 3, poll 7, mmap 9, munmap 11, socket 41, connect 42, sendmsg 46,
recvmsg 47, ftruncate 77, epoll_create1 291, epoll_ctl 233, epoll_wait 232,
memfd_create 319`.

---

## 11. Bring-up checklist (do it in this order)

1. Parse env, connect to `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`.
2. `get_registry`, `sync`, read until `wl_callback.done`. Print the globals you see.
3. `bind` `wl_compositor`, `wl_shm`, `xdg_wm_base`.
4. `create_surface`, `get_xdg_surface`, `get_toplevel`, set title/app_id.
5. `commit` (no buffer), wait for `xdg_surface.configure`, `ack_configure`.
6. `memfd_create` + `ftruncate` + `mmap`; `wl_shm.create_pool` (with SCM_RIGHTS fd);
   `wl_shm_pool.create_buffer`.
7. Fill pixels, `attach` + `damage` + `commit`. Window should appear.
8. Handle `xdg_toplevel.configure(w,h)`: when `w,h > 0`, rebuild the shm buffer to
   that size before the next commit (tiling WMs dictate the size; `0,0` = no
   opinion, keep your size).
9. Add `wl_surface.frame` pacing, handle `wl_buffer.release`,
   `xdg_wm_base.ping` -> `pong`, and `xdg_surface.close`.
10. Add a second buffer and only redraw a released buffer (avoid tearing/stalls).

## 12. Pitfalls recap

- Forgetting `SCM_RIGHTS` on `create_pool` -> protocol error / no buffer.
- Forgetting `ack_configure` or `pong` -> compositor considers you dead.
- Not padding strings/arrays to 4 bytes -> `size` mismatch, desync, disconnect.
- Reusing object IDs the server still owns -> hard protocol error.
- Assuming one `recv` equals one message -> corruption under load.
- Non-native endianness assumptions.
- Attaching a buffer and drawing after `commit` without waiting for `release`.
- Assuming the size you picked is the window size -> on a tiling WM the surface
  stays small and the tile looks mostly empty; adopt `xdg_toplevel.configure(w,h)`
  (`0,0` means the compositor has no opinion).
