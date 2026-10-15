; ---------------------------------------------------------------------------
; wayland.inc - a tiny fasm2/fasmg Wayland client "library"
;
; Speaks the Wayland wire protocol directly over a Unix socket, no libc and
; no libwayland.  It can open an xdg-shell toplevel window backed by a wl_shm
; (shared memory) buffer that you draw pixels into.
;
; Build with fasm2:   fasm2 wayland.asm wayland
;
; Public API (all are plain labels, call with the SysV-ish convention noted):
;   wl_envp          dq  - set to the process envp before calling wl_init
;   wl_init          -> connects, gets the registry, binds the globals
;   wl_create_window  rdi=width, rsi=height, rdx=title cstring
;   wl_fb            dq  - pointer to the mapped pixel buffer (XRGB8888)
;   wl_width/wl_height/wl_stride  dd - geometry of that buffer
;   wl_present        - attach + damage + commit (shows the current pixels);
;                       also requests a wl_surface.frame callback for pacing
;   wl_wait_frame     - blocks until that frame callback fires (vsync)
;   wl_wait_release   - blocks until the compositor released the buffer
;   wl_dispatch       - flush, block for events, and process them once
;   wl_quit          dq  - set to 1 when the window should close
;
; Rendering model: one buffer, single-buffered.  Draw into [wl_fb], call
; wl_present, then wl_wait_frame + wl_wait_release before drawing the next
; frame.  Loop until [wl_quit] is set (the window was closed).
; ---------------------------------------------------------------------------

; ------------------------------- syscalls ---------------------------------
SYS_write         = 1
SYS_mmap          = 9
SYS_socket        = 41
SYS_connect       = 42
SYS_sendmsg       = 46
SYS_recvmsg       = 47
SYS_ftruncate     = 77
SYS_exit          = 60
SYS_memfd_create  = 319

; ------------------------------- constants --------------------------------
AF_UNIX            = 1
SOCK_STREAM        = 1
SOL_SOCKET         = 1
SCM_RIGHTS         = 1
MAP_SHARED         = 1
MSG_NOSIGNAL       = 0x4000
PROT_RW            = 3          ; PROT_READ | PROT_WRITE
MFD_CLOEXEC        = 1

WL_DISPLAY_SYNC              = 0
WL_DISPLAY_GET_REGISTRY      = 1
WL_REGISTRY_BIND             = 0
WL_COMPOSITOR_CREATE_SURFACE = 0
WL_SURFACE_ATTACH            = 1
WL_SURFACE_DAMAGE            = 2
WL_SURFACE_FRAME             = 3
WL_SURFACE_COMMIT            = 6
WL_SHM_CREATE_POOL           = 0
WL_SHM_POOL_CREATE_BUFFER    = 0
XDG_WM_BASE_GET_XDG_SURFACE  = 2
XDG_WM_BASE_PONG             = 3
XDG_SURFACE_GET_TOPLEVEL     = 1
XDG_SURFACE_ACK_CONFIGURE    = 4
XDG_TOPLEVEL_SET_TITLE       = 2
XDG_TOPLEVEL_SET_APP_ID      = 3
WL_SHM_FORMAT_XRGB8888       = 1

WL_OUT_CAP = 8192
WL_IN_CAP  = 16384

; --------------------------- marshalling macros ---------------------------
; rbx is reserved as the outgoing cursor into wl_out for the whole program.
; Tracks (in wl_msg_start) the start of the message being built.

macro put_dw v
    mov dword [rbx], v
    add rbx, 4
end macro

macro put_w v
    mov word [rbx], v
    add rbx, 2
end macro

macro begin_msg obj, op
    mov [wl_msg_start], rbx
    put_dw obj
    put_w op
    put_w 0                 ; size placeholder
end macro

macro end_msg
    mov rax, rbx
    sub rax, [wl_msg_start]
    mov rdi, [wl_msg_start]
    mov word [rdi + 6], ax  ; patch size
end macro

; ================================== code ==================================
segment readable executable

; --- allocate a fresh client object id (never reused) --------------- eax ->
wl_alloc_id:
    mov eax, [wl_next_id]
    inc dword [wl_next_id]
    ret

; --- append a string field (uint len, bytes, NUL, pad to 4) --- rsi = cstr --
wl_put_str:
    xor ecx, ecx
.wl_put_str_count:
    cmp byte [rsi + rcx], 0
    je  .wl_put_str_gotlen
    inc ecx
    jmp .wl_put_str_count
.wl_put_str_gotlen:
    lea eax, [ecx + 1]              ; wire string length includes the NUL
    put_dw eax
    test ecx, ecx
    jz  .wl_put_str_nul
    xor edx, edx
.wl_put_str_copy:
    mov al, [rsi + rdx]
    mov [rbx + rdx], al
    inc edx
    cmp edx, ecx
    jb  .wl_put_str_copy
    add rbx, rcx
.wl_put_str_nul:
    mov byte [rbx], 0
    inc rbx
.wl_put_str_pad:
    test rbx, 3
    jz  .wl_put_str_done
    mov byte [rbx], 0
    inc rbx
    jmp .wl_put_str_pad
.wl_put_str_done:
    ret

; --- strcmp against a wire string -------- rdx=bytes, ecx=len, rsi=target --
; The wire length includes the trailing NUL, so a match needs
;   wire_len == strlen(target) + 1
; Returns eax = 1 if equal, else 0.  Preserves ecx, edx.
wl_iface_eq:
    xor edi, edi
.wl_iface_eq_loop:
    mov al, [rsi + rdi]
    test al, al
    jz  .wl_iface_eq_end
    cmp edi, ecx
    jae .wl_iface_eq_no
    cmp al, [rdx + rdi]
    jne .wl_iface_eq_no
    inc edi
    jmp .wl_iface_eq_loop
.wl_iface_eq_end:
    lea eax, [rdi + 1]              ; strlen + 1 (the NUL)
    cmp eax, ecx
    jne .wl_iface_eq_no
    mov eax, 1
    ret
.wl_iface_eq_no:
    xor eax, eax
    ret

; --- copy cstring, return rdi = pointer to the NUL ---- rdi=dst, rsi=src ---
wl_stpcpy:
    xor ecx, ecx
.wl_stpcpy_loop:
    mov al, [rsi + rcx]
    mov [rdi + rcx], al
    test al, al
    jz  .wl_stpcpy_done
    inc ecx
    jmp .wl_stpcpy_loop
.wl_stpcpy_done:
    lea rdi, [rdi + rcx]
    ret

; --- getenv from the saved envp ------------------- rsi=KEY -> rax=value|0 --
wl_getenv:
    mov r8, [wl_envp]
    test r8, r8
    jz  .wl_getenv_none
.wl_getenv_next:
    mov rdi, [r8]
    test rdi, rdi
    jz  .wl_getenv_none
    xor ecx, ecx
.wl_getenv_cmp:
    mov al, [rsi + rcx]
    test al, al
    jz  .wl_getenv_eq
    cmp al, [rdi + rcx]
    jne .wl_getenv_adv
    inc ecx
    jmp .wl_getenv_cmp
.wl_getenv_eq:
    cmp byte [rdi + rcx], '='
    jne .wl_getenv_adv
    lea rax, [rdi + rcx + 1]
    ret
.wl_getenv_adv:
    add r8, 8
    jmp .wl_getenv_next
.wl_getenv_none:
    xor eax, eax
    ret

; --- fatal: exit(1) --------------------------------------------------------
wl_die:
    mov eax, SYS_exit
    mov edi, 1
    syscall

; --- connect to $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY --------------------------
; Building the path keeps its destination cursor in r9 because wl_getenv
; clobbers rdi.
wl_connect:
    lea r9, [wl_sock_path]
    ; runtime dir
    lea rsi, [k_xdg_runtime]
    call wl_getenv
    mov rsi, rax
    test rsi, rsi
    jnz .wl_connect_rt
    lea rsi, [wl_default_runtime]
.wl_connect_rt:
    mov rdi, r9
    call wl_stpcpy                 ; rdi -> trailing NUL
    mov r9, rdi
    mov byte [r9], '/'
    inc r9
    ; display name
    lea rsi, [k_wayland_display]
    call wl_getenv
    mov rsi, rax
    test rsi, rsi
    jnz .wl_connect_disp
    lea rsi, [wl_default_display]
.wl_connect_disp:
    mov rdi, r9
    call wl_stpcpy                 ; writes terminating NUL

    ; sockaddr_un { u16 family; char path[]; }
    lea rdi, [wl_addr]
    mov word [rdi], AF_UNIX
    add rdi, 2
    lea rsi, [wl_sock_path]
    call wl_stpcpy
    mov rax, rdi
    sub rax, wl_addr
    inc rax                        ; + NUL  => 2 + strlen(path) + 1
    mov [wl_addrlen], rax

    mov eax, SYS_socket
    mov edi, AF_UNIX
    mov esi, SOCK_STREAM
    xor edx, edx
    syscall
    test rax, rax
    js  wl_die
    mov [wl_sock], eax

    mov eax, SYS_connect
    mov edi, [wl_sock]
    lea rsi, [wl_addr]
    mov rdx, [wl_addrlen]
    syscall
    test rax, rax
    js  wl_die
    ret

; --- send everything currently marshalled in wl_out ------------------------
wl_flush:
    mov rax, rbx
    sub rax, wl_out
    jz  .wl_flush_none
    mov qword [wl_iov + 8], rax

    mov qword [wl_msghdr + 0], 0          ; msg_name
    mov dword [wl_msghdr + 8], 0          ; msg_namelen
    lea rax, [wl_iov]
    mov qword [wl_msghdr + 16], rax       ; msg_iov
    mov qword [wl_msghdr + 24], 1         ; msg_iovlen
    mov qword [wl_msghdr + 32], 0         ; msg_control
    mov qword [wl_msghdr + 40], 0         ; msg_controllen
    mov dword [wl_msghdr + 48], 0         ; msg_flags

    mov eax, SYS_sendmsg
    mov edi, [wl_sock]
    lea rsi, [wl_msghdr]
    mov edx, MSG_NOSIGNAL
    syscall
    test rax, rax
    js  wl_die
.wl_flush_none:
    mov rbx, wl_out
    ret

; --- flush attaching one fd via SCM_RIGHTS -------------------------- edi=fd
wl_flush_fd:
    mov rax, rbx
    sub rax, wl_out
    mov qword [wl_iov + 8], rax

    mov qword [wl_ctl + 0], 20            ; cmsg_len = CMSG_LEN(4)
    mov dword [wl_ctl + 8], SOL_SOCKET
    mov dword [wl_ctl + 12], SCM_RIGHTS
    mov dword [wl_ctl + 16], edi          ; the fd

    mov qword [wl_msghdr + 0], 0
    mov dword [wl_msghdr + 8], 0
    lea rax, [wl_iov]
    mov qword [wl_msghdr + 16], rax
    mov qword [wl_msghdr + 24], 1
    lea rax, [wl_ctl]
    mov qword [wl_msghdr + 32], rax
    mov qword [wl_msghdr + 40], 24        ; CMSG_SPACE(4)
    mov dword [wl_msghdr + 48], 0

    mov eax, SYS_sendmsg
    mov edi, [wl_sock]
    lea rsi, [wl_msghdr]
    mov edx, MSG_NOSIGNAL
    syscall
    test rax, rax
    js  wl_die
    mov rbx, wl_out
    ret

; --- receive once into wl_in append position -------------------------------
wl_recv:
    mov rax, WL_IN_CAP
    sub rax, [wl_in_len]
    lea rsi, [wl_in]
    add rsi, [wl_in_len]
    mov qword [wl_riov + 0], rsi
    mov qword [wl_riov + 8], rax

    mov qword [wl_rmsghdr + 0], 0
    mov dword [wl_rmsghdr + 8], 0
    lea rax, [wl_riov]
    mov qword [wl_rmsghdr + 16], rax
    mov qword [wl_rmsghdr + 24], 1
    lea rax, [wl_in_ctl]
    mov qword [wl_rmsghdr + 32], rax
    mov qword [wl_rmsghdr + 40], 64
    mov dword [wl_rmsghdr + 48], 0

    mov eax, SYS_recvmsg
    mov edi, [wl_sock]
    lea rsi, [wl_rmsghdr]
    xor edx, edx
    syscall
    test rax, rax
    js  wl_die
    jz  wl_die                     ; EOF: server closed
    add [wl_in_len], rax
    ret

; --- parse every complete message currently buffered -----------------------
wl_process:
    mov qword [wl_parse_off], 0
.wl_process_loop:
    mov r8, [wl_in_len]
    sub r8, [wl_parse_off]
    cmp r8, 8
    jb  .wl_process_partial
    lea rdi, [wl_in]
    add rdi, [wl_parse_off]
    mov eax, [rdi]                 ; object id
    movzx ecx, word [rdi + 4]      ; opcode
    movzx edx, word [rdi + 6]      ; size (includes header)
    cmp edx, 8
    jb  wl_die
    cmp rdx, r8
    ja  .wl_process_partial
    mov [wl_msg_size], rdx
    add rdi, 8                     ; args
    call wl_handle_event
    mov rax, [wl_msg_size]
    add [wl_parse_off], rax
    jmp .wl_process_loop
.wl_process_partial:
    mov rcx, [wl_in_len]
    sub rcx, [wl_parse_off]
    lea rsi, [wl_in]
    add rsi, [wl_parse_off]
    lea rdi, [wl_in]
    test rcx, rcx
    jz  .wl_process_setlen
    xor edx, edx
.wl_process_move:
    mov al, [rsi + rdx]
    mov [rdi + rdx], al
    inc edx
    cmp rdx, rcx
    jb  .wl_process_move
.wl_process_setlen:
    mov [wl_in_len], rcx
    ret

; --- dispatch one received message -- eax=obj, ecx=op, rdi=args ------------
wl_handle_event:
    cmp eax, 1
    je  wl_ev_display
    cmp eax, [wl_registry_id]
    je  wl_ev_registry
    cmp eax, [wl_cb_id]
    je  wl_ev_callback
    cmp eax, [wl_frame_cb]
    je  wl_ev_frame
    cmp eax, [wl_wm_base_id]
    je  wl_ev_wm_base
    cmp eax, [wl_xdg_surf_id]
    je  wl_ev_xdg_surface
    cmp eax, [wl_toplevel_id]
    je  wl_ev_toplevel
    cmp eax, [wl_buffer_id]
    je  wl_ev_buffer
wl_ev_ret:
    ret

; wl_display.error(ous): print the message and die
wl_ev_display:
    test ecx, ecx
    jnz wl_ev_ret                  ; delete_id: ignore
    mov edx, [rdi + 8]             ; string length
    lea rsi, [rdi + 12]            ; string bytes
    push rsi
    push rdx
    mov eax, SYS_write
    mov edi, 2
    lea rsi, [wl_err_prefix]
    mov edx, wl_err_prefix_len
    syscall
    pop rdx
    pop rsi
    mov eax, SYS_write
    mov edi, 2
    syscall
    jmp wl_die

; wl_registry.global(usu)
wl_ev_registry:
    test ecx, ecx
    jnz wl_ev_ret
    mov eax, [rdi]
    mov [wl_tmp_name], eax
    mov ecx, [rdi + 4]             ; interface length
    lea rdx, [rdi + 8]             ; interface bytes
    mov r8d, ecx
    add r8d, 3
    and r8d, -4
    lea r9, [rdi + 8]
    add r9, r8
    mov eax, [r9]                  ; advertised version
    mov [wl_tmp_ver], eax

    lea rsi, [iface_wl_compositor]
    call wl_iface_eq
    test eax, eax
    jz  .wl_ev_reg_shm
    lea rsi, [iface_wl_compositor]
    mov edx, 4
    lea rcx, [wl_compositor_id]
    jmp wl_bind_global
.wl_ev_reg_shm:
    lea rsi, [iface_wl_shm]
    call wl_iface_eq
    test eax, eax
    jz  .wl_ev_reg_xdg
    lea rsi, [iface_wl_shm]
    mov edx, 1
    lea rcx, [wl_shm_id]
    jmp wl_bind_global
.wl_ev_reg_xdg:
    lea rsi, [iface_xdg_wm_base]
    call wl_iface_eq
    test eax, eax
    jz  wl_ev_ret
    lea rsi, [iface_xdg_wm_base]
    mov edx, 1
    lea rcx, [wl_wm_base_id]
    jmp wl_bind_global

; bind a global ------------------------------------------------ rsi/edx/rcx --
;   rsi = interface cstring, edx = max version we understand, rcx = role var
wl_bind_global:
    mov r12, rcx
    mov r13, rsi
    mov eax, [wl_tmp_ver]
    cmp eax, edx
    jbe .wl_bind_ver
    mov eax, edx
.wl_bind_ver:
    mov [wl_tmp_ver], eax
    call wl_alloc_id
    mov [r12], eax
    mov r14d, eax

    mov eax, [wl_registry_id]
    begin_msg eax, WL_REGISTRY_BIND
    mov eax, [wl_tmp_name]
    put_dw eax
    mov rsi, r13
    call wl_put_str
    mov eax, [wl_tmp_ver]
    put_dw eax
    mov eax, r14d
    put_dw eax
    end_msg
    ret

; wl_callback.done()
wl_ev_callback:
    mov qword [wl_sync_done], 1
    ret

; wl_callback.done() for the frame callback requested by wl_present
wl_ev_frame:
    mov qword [wl_frame_done], 1
    ret

; xdg_wm_base.ping(u) -> pong(u)
wl_ev_wm_base:
    test ecx, ecx
    jnz wl_ev_ret
    mov eax, [rdi]
    mov [wl_tmp_serial], eax
    mov eax, [wl_wm_base_id]
    begin_msg eax, XDG_WM_BASE_PONG
    mov eax, [wl_tmp_serial]
    put_dw eax
    end_msg
    ret

; xdg_surface.configure(u) -> ack_configure(u)
wl_ev_xdg_surface:
    test ecx, ecx
    jnz wl_ev_ret
    mov eax, [rdi]
    mov [wl_tmp_serial], eax
    mov qword [wl_got_configure], 1
    mov eax, [wl_xdg_surf_id]
    begin_msg eax, XDG_SURFACE_ACK_CONFIGURE
    mov eax, [wl_tmp_serial]
    put_dw eax
    end_msg
    ret

; xdg_toplevel.configure(iia) / close()
wl_ev_toplevel:
    test ecx, ecx
    jz  wl_ev_ret                  ; configure: geometry handled by compositor
    mov qword [wl_quit], 1         ; close
    ret

; wl_buffer.release()
wl_ev_buffer:
    test ecx, ecx
    jnz wl_ev_ret
    mov qword [wl_buffer_busy], 0
    ret

; --- one dispatch step: flush, block for events, process -------------------
wl_dispatch:
    call wl_flush
    call wl_recv
    call wl_process
    ret

; --- roundtrip: wl_display.sync and read until the callback fires ----------
wl_roundtrip:
    call wl_alloc_id
    mov [wl_cb_id], eax
    mov qword [wl_sync_done], 0
    mov eax, 1
    begin_msg eax, WL_DISPLAY_SYNC
    mov eax, [wl_cb_id]
    put_dw eax
    end_msg
.wl_roundtrip_spin:
    call wl_dispatch
    cmp qword [wl_sync_done], 0
    je  .wl_roundtrip_spin
    ret

; --- init: connect, get_registry, learn + bind the globals -----------------
wl_init:
    mov rbx, wl_out               ; start the marshalling cursor
    call wl_connect
    call wl_alloc_id
    mov [wl_registry_id], eax
    mov r14d, eax
    mov eax, 1
    begin_msg eax, WL_DISPLAY_GET_REGISTRY
    mov eax, r14d
    put_dw eax
    end_msg
    call wl_roundtrip
    ; all three globals must have been bound
    cmp dword [wl_compositor_id], 0
    je  wl_die
    cmp dword [wl_shm_id], 0
    je  wl_die
    cmp dword [wl_wm_base_id], 0
    je  wl_die
    ret

; --- create the toplevel window ---- rdi=w, rsi=h, rdx=title cstring -------
wl_create_window:
    mov [wl_width], edi
    mov [wl_height], esi
    mov [wl_title_ptr], rdx

    ; wl_compositor.create_surface
    call wl_alloc_id
    mov [wl_surface_id], eax
    mov r14d, eax
    mov eax, [wl_compositor_id]
    begin_msg eax, WL_COMPOSITOR_CREATE_SURFACE
    mov eax, r14d
    put_dw eax
    end_msg

    ; xdg_wm_base.get_xdg_surface(new_id, surface)
    call wl_alloc_id
    mov [wl_xdg_surf_id], eax
    mov r14d, eax
    mov eax, [wl_wm_base_id]
    begin_msg eax, XDG_WM_BASE_GET_XDG_SURFACE
    mov eax, r14d
    put_dw eax
    mov eax, [wl_surface_id]
    put_dw eax
    end_msg

    ; xdg_surface.get_toplevel
    call wl_alloc_id
    mov [wl_toplevel_id], eax
    mov r14d, eax
    mov eax, [wl_xdg_surf_id]
    begin_msg eax, XDG_SURFACE_GET_TOPLEVEL
    mov eax, r14d
    put_dw eax
    end_msg

    ; xdg_toplevel.set_title
    mov eax, [wl_toplevel_id]
    begin_msg eax, XDG_TOPLEVEL_SET_TITLE
    mov rsi, [wl_title_ptr]
    call wl_put_str
    end_msg

    ; xdg_toplevel.set_app_id
    mov eax, [wl_toplevel_id]
    begin_msg eax, XDG_TOPLEVEL_SET_APP_ID
    lea rsi, [wl_app_id]
    call wl_put_str
    end_msg

    ; wl_surface.commit (no buffer) -> triggers the first configure
    mov eax, [wl_surface_id]
    begin_msg eax, WL_SURFACE_COMMIT
    end_msg

    call wl_flush
.wl_create_window_wait:
    cmp qword [wl_got_configure], 0
    jne .wl_create_window_mapped
    call wl_dispatch
    jmp .wl_create_window_wait
.wl_create_window_mapped:
    ; the ack_configure was queued while processing the configure event
    call wl_flush
    call wl_create_shm_buffer
    ret

; --- create the shm pool + buffer and map the pixels -----------------------
wl_create_shm_buffer:
    ; size = width * 4 * height
    mov eax, [wl_width]
    shl eax, 2
    mov [wl_stride], eax
    mov ecx, eax
    imul ecx, [wl_height]
    mov [wl_shm_size], ecx

    mov eax, SYS_memfd_create
    lea rdi, [wl_memfd_name]
    mov esi, MFD_CLOEXEC
    syscall
    test rax, rax
    js  wl_die
    mov [wl_shm_fd], eax

    mov eax, SYS_ftruncate
    mov edi, [wl_shm_fd]
    mov esi, [wl_shm_size]
    syscall
    test rax, rax
    js  wl_die

    mov eax, SYS_mmap
    xor edi, edi
    mov esi, [wl_shm_size]
    mov edx, PROT_RW
    mov r10d, MAP_SHARED
    mov r8d, [wl_shm_fd]
    xor r9d, r9d
    syscall
    test rax, rax
    js  wl_die
    mov [wl_fb], rax

    ; wl_shm.create_pool(new_id, fd, size)
    ; NOTE: an 'h' (fd) argument occupies NO bytes in the message body - the
    ; descriptor travels only as SCM_RIGHTS ancillary data (see spec ch. Wire
    ; Format).  It is sent alone here so only this message carries the fd.
    call wl_alloc_id
    mov [wl_pool_id], eax
    mov r14d, eax
    mov eax, [wl_shm_id]
    begin_msg eax, WL_SHM_CREATE_POOL
    mov eax, r14d
    put_dw eax
    mov eax, [wl_shm_size]
    put_dw eax
    end_msg
    mov edi, [wl_shm_fd]
    call wl_flush_fd

    ; wl_shm_pool.create_buffer(new_id, offset, w, h, stride, format)
    call wl_alloc_id
    mov [wl_buffer_id], eax
    mov r14d, eax
    mov eax, [wl_pool_id]
    begin_msg eax, WL_SHM_POOL_CREATE_BUFFER
    mov eax, r14d
    put_dw eax
    put_dw 0
    mov eax, [wl_width]
    put_dw eax
    mov eax, [wl_height]
    put_dw eax
    mov eax, [wl_stride]
    put_dw eax
    put_dw WL_SHM_FORMAT_XRGB8888
    end_msg
    call wl_flush
    ret

; --- show the current contents of the framebuffer --------------------------
; Requests a wl_surface.frame callback (the vsync signal) for this commit, so
; wl_wait_frame can pace the next draw to the compositor's refresh.
wl_present:
    call wl_alloc_id
    mov [wl_frame_cb], eax
    mov qword [wl_frame_done], 0
    mov r14d, eax
    mov eax, [wl_surface_id]
    begin_msg eax, WL_SURFACE_FRAME
    mov eax, r14d
    put_dw eax
    end_msg

    mov eax, [wl_surface_id]
    begin_msg eax, WL_SURFACE_ATTACH
    mov eax, [wl_buffer_id]
    put_dw eax
    put_dw 0
    put_dw 0
    end_msg

    mov eax, [wl_surface_id]
    begin_msg eax, WL_SURFACE_DAMAGE
    put_dw 0
    put_dw 0
    mov eax, [wl_width]
    put_dw eax
    mov eax, [wl_height]
    put_dw eax
    end_msg

    mov eax, [wl_surface_id]
    begin_msg eax, WL_SURFACE_COMMIT
    end_msg

    mov qword [wl_buffer_busy], 1
    call wl_flush
    ret

; --- block until the compositor released the buffer (or the window closed) --
wl_wait_release:
.wl_wait_release_spin:
    cmp qword [wl_quit], 0
    jne .wl_wait_release_done
    cmp qword [wl_buffer_busy], 0
    je  .wl_wait_release_done
    call wl_dispatch
    jmp .wl_wait_release_spin
.wl_wait_release_done:
    ret

; --- block until the frame callback from the last wl_present fires ---------
; Together with wl_wait_release this paces drawing to the compositor's refresh.
wl_wait_frame:
.wl_wait_frame_spin:
    cmp qword [wl_quit], 0
    jne .wl_wait_frame_done
    cmp qword [wl_frame_done], 0
    jne .wl_wait_frame_done
    call wl_dispatch
    jmp .wl_wait_frame_spin
.wl_wait_frame_done:
    ret

; ================================== data ==================================
segment readable writeable

align 8
wl_envp          dq 0
wl_next_id       dd 2

wl_sock          dd 0
wl_addrlen       dq 0
wl_compositor_id dd 0
wl_shm_id        dd 0
wl_wm_base_id    dd 0
wl_registry_id   dd 0
wl_cb_id         dd 0
wl_frame_cb      dd 0
wl_surface_id    dd 0
wl_xdg_surf_id   dd 0
wl_toplevel_id   dd 0
wl_pool_id       dd 0
wl_buffer_id     dd 0

wl_width         dd 0
wl_height        dd 0
wl_stride        dd 0
wl_shm_fd        dd 0
wl_shm_size      dd 0
wl_fb            dq 0

wl_quit          dq 0
wl_sync_done     dq 0
wl_got_configure dq 0
wl_buffer_busy   dq 0
wl_frame_done    dq 0

wl_tmp_name      dd 0
wl_tmp_ver       dd 0
wl_tmp_serial    dd 0

wl_msg_start     dq 0
wl_msg_size      dq 0
wl_parse_off     dq 0
wl_in_len        dq 0

wl_title_ptr     dq 0

align 8
wl_out      rb WL_OUT_CAP
wl_in       rb WL_IN_CAP
wl_in_ctl   rb 64

align 8
wl_iov      dq wl_out, 0
wl_riov     dq 0, 0
wl_ctl      rb 24

align 8
wl_msghdr   rb 56
wl_rmsghdr  rb 56

wl_addr     rb 112

wl_sock_path rb 256

wl_app_id         db 'asm-wayland', 0
wl_memfd_name     db 'wl_shm', 0
wl_default_runtime db '/run/user/1000', 0
wl_default_display db 'wayland-0', 0
k_xdg_runtime     db 'XDG_RUNTIME_DIR', 0
k_wayland_display db 'WAYLAND_DISPLAY', 0
iface_wl_compositor db 'wl_compositor', 0
iface_wl_shm        db 'wl_shm', 0
iface_xdg_wm_base   db 'xdg_wm_base', 0
wl_err_prefix     db 'wayland error: '
wl_err_prefix_len = $ - wl_err_prefix
