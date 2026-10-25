; Tiny MinHook detour for the real player camera struct pointer.
;
; Adapted from the technique in FransBouma/InjectableGenericCameraSystem
; (Cameras/SOTTR/InjectableGenericCameraSystem/Interceptor.asm), BSD-style
; licensed, copyright(c) 2017 Frans Bouma:
; https://github.com/FransBouma/InjectableGenericCameraSystem
;
; The original hooks a mid-function instruction by hand-splicing a JMP and
; manually re-executing the overwritten original instructions before jumping
; back. We simplify this by hooking through MinHook instead, which already
; handles relocating the overwritten instructions into a trampoline for us --
; our detour only needs to capture RDX (the camera struct pointer, confirmed
; live at this exact instruction via Cheat Engine breakpoint earlier this
; session) and then jump into that trampoline.
;
; Target instruction sequence (found via our own AOB scan at runtime, since
; the Steam Definitive Edition build's address differs from the original
; project's documented offset):
;   89 59 28              mov [rcx+28h], ebx
;   45 33 C0              xor r8d, r8d
;   0F 28 82 80 00 00 00  movaps xmm0, [rdx+80h]   <- camera struct ptr in RDX here
;   4C 8B F9              mov r15, rcx
;   41 0F B6 D1           movzx edx, r9b

PUBLIC cameraStructInterceptor
EXTERN g_cameraStructAddress: qword
EXTERN g_cameraOwnerAddress: qword
EXTERN g_cameraStructTrampoline: qword

.code

; RCX here is FUN_14049a220's first parameter -- the gameplay camera object (aim work, docs/AIM.md).
cameraStructInterceptor PROC
    mov [g_cameraStructAddress], rdx
    mov [g_cameraOwnerAddress], rcx
    jmp qword ptr [g_cameraStructTrampoline]
cameraStructInterceptor ENDP


; ---------------------------------------------------------------------------
; Write blocker. The game overwrites the camera struct's position/rotation/FOV
; every frame from a second source (RDX here, a *different* pointer than the
; struct itself) via this exact instruction sequence -- confirmed by testing:
; a one-off write to the struct reverted within one frame until this hook was
; added. Adapted from CAMERA_WRITE1_INTERCEPT_KEY in the same reference
; project (see THIRD_PARTY_NOTICES.md).
;
; Original instructions this replaces (34 bytes total, found via our own AOB
; scan since the exact address shifts per build):
;   66 0F 7F 81 80 00 00 00   movdqa [rcx+80h], xmm0      <- write position
;   0F 28 8A A0 00 00 00      movaps xmm1, [rdx+0A0h]
;   0F 29 89 A0 00 00 00      movaps [rcx+0A0h], xmm1      <- write rotation
;   8B 82 B0 00 00 00         mov eax, [rdx+0B0h]
;   89 81 B0 00 00 00         mov [rcx+0B0h], eax           <- write FOV
;
; This is no longer a pure write *blocker*: the game's own writes always run
; (so position keeps following Lara and the mouse-driven rotation still
; arrives), and when the override is on and the target is our camera struct we
; then call ApplyCameraHeadTracking(rcx) to compose the head rotation onto the
; game's rotation and force our FOV, in place, before the renderer reads it.
;
; We reproduce the original instructions ourselves (rather than jumping into a
; MinHook trampoline) and always resume at a fixed continuation address
; (target + 0x22) computed independently in C++ from the AOB match location,
; regardless of how many bytes MinHook's own patch ends up using.
;
; The call happens in the middle of a game function, so everything a callee
; may clobber is preserved: flags, rax/rcx/rdx/r8-r11, rbp (used to re-align
; the stack), and ymm0-ymm5 (full width -- the game itself uses AVX here).
;
; Instrumentation (Phase 1): before replaying the game's write, snapshot what the struct holds right now --
; i.e. our previous write as it survived -- into g_preWriteSnap. Uses only xmm1 and eax, which the replayed
; instructions overwrite anyway, and flags, which the early-exit path below already clobbers. Unaligned moves,
; so neither address needs 16-byte alignment.
PUBLIC cameraWriteInterceptor
EXTERN g_cameraOverrideEnabled: byte
EXTERN g_cameraWriteContinueAddress: qword
EXTERN g_preWriteSnap: byte
EXTERN g_cameraOtherWrites: qword
EXTERN ApplyCameraHeadTracking: PROC
EXTERN ApplyAimInjection: PROC
EXTERN g_aimInjectEnabled: byte

cameraWriteInterceptor PROC
    cmp rcx, [g_cameraStructAddress]
    jne other
    movups xmm1, xmmword ptr [rcx+80h]
    movups xmmword ptr [g_preWriteSnap], xmm1
    movups xmm1, xmmword ptr [rcx+0A0h]
    movups xmmword ptr [g_preWriteSnap+10h], xmm1
    mov eax, dword ptr [rcx+0B0h]
    mov dword ptr [g_preWriteSnap+20h], eax
    jmp replay
other:
    ; The same copy writes other camera structs too; count them (instrumentation), they are never modified.
    lock inc qword ptr [g_cameraOtherWrites]
replay:
    movdqa xmmword ptr [rcx+80h], xmm0
    movaps xmm1, xmmword ptr [rdx+0A0h]
    movaps xmmword ptr [rcx+0A0h], xmm1
    mov eax, dword ptr [rdx+0B0h]
    mov dword ptr [rcx+0B0h], eax

    cmp byte ptr [g_cameraOverrideEnabled], 0
    je done
    cmp rcx, [g_cameraStructAddress]
    je ours
    ; A2 (docs/AIM.md): another record. The copy function is a leaf (no prologue), so [rsp] is its caller's return
    ; address; ApplyAimInjection(dst = rcx, src = rdx, return address) acts only on the camera component's record.
    cmp byte ptr [g_aimInjectEnabled], 0
    je done
    pushfq
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push rbp
    mov r8, [rsp+48h]
    mov rbp, rsp
    sub rsp, 100h
    and rsp, -16
    vmovups ymmword ptr [rsp+20h], ymm0
    vmovups ymmword ptr [rsp+40h], ymm1
    vmovups ymmword ptr [rsp+60h], ymm2
    vmovups ymmword ptr [rsp+80h], ymm3
    vmovups ymmword ptr [rsp+0A0h], ymm4
    vmovups ymmword ptr [rsp+0C0h], ymm5
    call ApplyAimInjection
    vmovups ymm0, ymmword ptr [rsp+20h]
    vmovups ymm1, ymmword ptr [rsp+40h]
    vmovups ymm2, ymmword ptr [rsp+60h]
    vmovups ymm3, ymmword ptr [rsp+80h]
    vmovups ymm4, ymmword ptr [rsp+0A0h]
    vmovups ymm5, ymmword ptr [rsp+0C0h]
    mov rsp, rbp
    pop rbp
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    popfq
    jmp done

ours:
    pushfq
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push rbp
    mov rbp, rsp
    sub rsp, 100h
    and rsp, -16
    vmovups ymmword ptr [rsp+20h], ymm0
    vmovups ymmword ptr [rsp+40h], ymm1
    vmovups ymmword ptr [rsp+60h], ymm2
    vmovups ymmword ptr [rsp+80h], ymm3
    vmovups ymmword ptr [rsp+0A0h], ymm4
    vmovups ymmword ptr [rsp+0C0h], ymm5
    call ApplyCameraHeadTracking
    vmovups ymm0, ymmword ptr [rsp+20h]
    vmovups ymm1, ymmword ptr [rsp+40h]
    vmovups ymm2, ymmword ptr [rsp+60h]
    vmovups ymm3, ymmword ptr [rsp+80h]
    vmovups ymm4, ymmword ptr [rsp+0A0h]
    vmovups ymm5, ymmword ptr [rsp+0C0h]
    mov rsp, rbp
    pop rbp
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    popfq
done:
    jmp qword ptr [g_cameraWriteContinueAddress]
cameraWriteInterceptor ENDP

END
