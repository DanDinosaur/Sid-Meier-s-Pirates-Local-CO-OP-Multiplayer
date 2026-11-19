# Pirates! co-op mod - register-level stubs (AT&T syntax, i686 mingw: C symbols have a leading underscore)
    .text

# ---- replaces "call 0x4f0ba0" (Xbox XInputGetState emulation). In: ESI = XINPUT_STATE* (0x8ca33c + port*100)
    .globl _stub_padfill
_stub_padfill:
    pushl %ecx
    pushl %edx
    pushl %esi
    call _c_padfill
    addl $4, %esp
    popl %edx
    popl %ecx
    ret

# ---- replaces "call 0x42d320" (pad -> key mapper). In: EAX = mode name ("Sail", "Duel", ...)
    .globl _stub_padmap
_stub_padmap:
    pushl %eax
    pushl %ecx
    pushl %edx
    pushl %eax
    call _c_padmap_pre
    addl $4, %esp
    popl %edx
    popl %ecx
    popl %eax
    call *_p_padmap
    pushl %ecx
    pushl %edx
    pushl %eax
    call _c_padmap_post        # PC15: may replace the key (land battle D-pad = army move)
    addl $4, %esp
    popl %edx
    popl %ecx
    ret

# ---- replaces "call 0x4db270" at 0x46857b (fleet ship target heading). In: EAX=dY ECX=dX EBP=ship*0x45c
    .globl _stub_fleet_atan
_stub_fleet_atan:
    call *_p_atan2
    pushl %ecx
    pushl %edx
    pushl %eax
    pushl %ebp
    call _c_fleet_heading
    addl $8, %esp
    popl %edx
    popl %ecx
    ret

# ---- replaces every "call 0x41b1d0" (sword duel). Convention unknown -> keep ECX and the stack untouched.
    .globl _stub_duel
_stub_duel:
    pushal
    call _c_duel_begin
    popal
    popl _g_duel_ret
    call *_p_duel
    pushl _g_duel_ret
    pushal
    call _c_duel_end
    popal
    ret

# ---- jmp at 0x47ba76: draw the side-0 battle HUD only for ship 0 (player 1), never for player 2's ship
    .globl _stub_bhud
_stub_bhud:
    movl 0x20(%esp), %eax
    addl $8, %esp
    orl 0x3c(%esp), %eax
    testl %eax, %eax
    jne 1f
    jmp *_p_bhud_draw
1:  jmp *_p_bhud_skip

# ---- helper: game atan2. int game_atan2(int dx, int dy)  (cdecl)
    .globl _game_atan2
_game_atan2:
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebp
    movl 20(%esp), %ecx
    movl 24(%esp), %eax
    call *_p_atan2
    popl %ebp
    popl %edi
    popl %esi
    popl %ebx
    ret

# ---- helper: game trig. int game_trig(int r, unsigned angle) = r * f(angle)   (cdecl)
    .globl _game_trig
_game_trig:
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebp
    movl 20(%esp), %eax
    movl 24(%esp), %ecx
    call *_p_trig
    popl %ebp
    popl %edi
    popl %esi
    popl %ebx
    ret

# ---- helper: land test in sea battle. int game_battle_blocked(int x, int y)  (cdecl, returns char)
    .globl _game_battle_blocked
_game_battle_blocked:
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebp
    pushl 24(%esp)
    pushl 24(%esp)
    call *_p_landtest
    addl $8, %esp
    movzbl %al, %eax
    popl %ebp
    popl %edi
    popl %esi
    popl %ebx
    ret

# ---- jmp at 0x468efa: never show "Wait for me!" for Player 2's ship (EBP = ship*0x45c, EBX = distance)
    .globl _stub_waitforme
_stub_waitforme:
    cmpl _g_p2_ebpoff, %ebp
    je 1f
    cmpl $1000000, %ebx
    jle 1f
    jmp *_p_wfm_show
1:  jmp *_p_wfm_skip

# ---- generic "wrap a call": begin(); original(same stack, same registers); end(). Used for the name entry,
#      ship naming and shipwright loops. Not re-entrant (each has its own saved return address).
    .globl _stub_name_entry
_stub_name_entry:
    pushal
    call _c_name_begin
    popal
    popl _g_ret_name
    call *_p_name_entry
    pushl _g_ret_name
    pushal
    call _c_name_end
    popal
    ret

    .globl _stub_ship_name
_stub_ship_name:
    pushal
    call _c_shipname_begin
    popal
    popl _g_ret_ship
    call *_p_ship_name
    pushl _g_ret_ship
    pushal
    call _c_shipname_end
    popal
    ret

    .globl _stub_shipyard
_stub_shipyard:
    pushal
    pushl 36(%esp)
    call _c_yard_begin
    addl $4, %esp
    popal
    popl _g_ret_yard
    call *_p_shipyard
    pushl _g_ret_yard
    pushal
    call _c_yard_end
    popal
    ret

# ---- replaces "call 0x42e440" at 0x4ddfb7 in the trade/plunder screen. EBP = screen object, ESI = mode string
    .globl _stub_tradekey
_stub_tradekey:
    pushal
    pushl %ebp
    call _c_trade_frame
    addl $4, %esp
    popal
    call *_p_getkey2
    pushl %ecx
    pushl %edx
    pushl %eax
    pushl %ebp
    call _c_trade_key
    addl $8, %esp
    popl %edx
    popl %ecx
    ret

# ---- jmp at 0x430190 (generic choice menu entry): let C adjust the default choice, then run the
#      displaced first instruction "mov eax,[0x8cacd8]" and continue at 0x430195
    .globl _stub_menu
_stub_menu:
    pushal
    leal 36(%esp), %eax
    pushl %eax
    call _c_menu_pre
    addl $4, %esp
    popal
    movl 0x8cacd8, %eax
    jmp *_p_menu_cont

# ---- jmp at 0x54aa00 (NiFrustum setter, thiscall, 7 args, ret 0x1c). Replaces the whole function.
    .globl _stub_frustum
_stub_frustum:
    pushl %ecx
    pushl %edx
    leal 12(%esp), %eax
    pushl %eax
    pushl %ecx
    call _c_frustum
    addl $8, %esp
    popl %edx
    popl %ecx
    movl %ecx, %eax
    ret $0x1c

# ---- replaces "call 0x404e50" (rotate fleet / change flagship). EAX is an input; keep it for a 2nd call.
    .globl _stub_rotate
_stub_rotate:
    movl %eax, _g_rot_eax
    pushal
    call _c_rotate_pre
    popal
    cmpl $0, _g_rot_skip
    jne 9f
    call *_p_rotate
    pushal
    call _c_rotate_post
    popal
    cmpl $0, _g_rot_again
    je 9f
    movl _g_rot_eax, %eax
    call *_p_rotate
    pushal
    call _c_rotate_post2
    popal
9:  ret

# ---- helper: game's 2D text. void game_text(const char *s, int x, int y, int size, unsigned argb, int flags)
#      FUN_004b06c0(text,x,y,size,color,flags,-1,0) cdecl, EAX = font (0 = default)
    .globl _game_text
_game_text:
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebp
    pushl $0
    pushl $-1
    pushl 48(%esp)
    pushl 48(%esp)
    pushl 48(%esp)
    pushl 48(%esp)
    pushl 48(%esp)
    pushl 48(%esp)
    xorl %eax, %eax
    call *_p_text
    addl $32, %esp
    popl %ebp
    popl %edi
    popl %esi
    popl %ebx
    ret

# ---- replaces "call 0x4d0cd0" (draw the frame) inside the two name-entry loops: add the keyboard first
    .globl _stub_osk_frame
_stub_osk_frame:
    pushal
    call _c_osk_gamedraw
    popal
    jmp *_p_frame

# ---- original FUN_004af080 (2D quad) after our jmp: its first 8 bytes, then continue at 0x4af088
    .globl _o_quad_tramp
_o_quad_tramp:
    subl $0x28, %esp
    movl 0x72637c, %eax
    jmp *_p_quad_cont

# ---- PC5: jmp at 0x54bc20 (renderer: register a visible object, thiscall ECX=list, arg = object, ret 4).
# Crash 0x54bc49: object with no render state ([obj+0xb4] == 0). Skip it (return 0 = not drawn) instead of crashing.
    .globl _stub_regobj
_stub_regobj:
    movl 4(%esp), %eax
    testl %eax, %eax
    je 1f
    cmpl $0, 0xb4(%eax)
    je 1f
    pushl %ebx
    movl 8(%esp), %ebx
    jmp *_p_regobj_cont
1:  incl _g_regskip_n
    movl %eax, _g_regskip_obj
    movl (%esp), %eax
    movl %eax, _g_regskip_ret
    xorl %eax, %eax
    ret $4

# ---- PC5: calls to 0x477450 (speed vs wind; EAX = heading, ECX = wind, stack arg = ship type, caller pops).
# Original result, then c_wind_*(result, type, EBP) may raise it for the player's own ships.
    .globl _stub_wind_sail
_stub_wind_sail:
    movl 4(%esp), %edx
    pushl %edx
    pushl %edx
    call *_p_wind
    addl $4, %esp
    popl %edx
    pushl %ebp
    pushl %edx
    pushl %eax
    call _c_wind_sail
    addl $12, %esp
    ret

    .globl _stub_wind_battle
_stub_wind_battle:
    movl 4(%esp), %edx
    pushl %edx
    pushl %edx
    call *_p_wind
    addl $4, %esp
    popl %edx
    pushl %ebp
    pushl %edx
    pushl %eax
    call _c_wind_battle
    addl $12, %esp
    ret

# ---- PC8 helper: int game_addship(int src)  (cdecl). FUN_00404b70: EAX = world ship to copy into the first free
#      fleet slot (name taken from [0x7267d0]); returns 1 = added, 0 = fleet full.
    .globl _game_addship
_game_addship:
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebp
    movl 20(%esp), %eax
    call *_p_addship
    popl %ebp
    popl %edi
    popl %esi
    popl %ebx
    ret

# ---- PC8: jmp at 0x404810 (crew mood 0..4, 0 = mutinous). "Crew always happy" -> 4.
    .globl _stub_happy
_stub_happy:
    cmpl $0, _g_happy_on
    je 1f
    movl $4, %eax
    ret
1:  movswl 0x869a76, %eax
    jmp *_p_happy_cont

# ---- PC8: replaces "call 0x489360" at 0x47b36d (sea battle camera: EAX/ECX/EDI + 5 stack args, arg3 = distance).
#      c_bcam may enlarge the distance (D-pad UP = far view).
    .globl _stub_bcam
_stub_bcam:
    pushal
    leal 36(%esp), %eax
    pushl %eax
    call _c_bcam
    addl $4, %esp
    popal
    jmp *_p_bcam

# ---- PC9: jmp at 0x43fd70 (fame points, computed, no args). Mod menu "Max fame": result raised to g_fame_value.
    .globl _stub_fame
_stub_fame:
    call _o_fame_tramp
    cmpl $0, _g_fame_on
    je 1f
    cmpl _g_fame_value, %eax
    jge 1f
    movl _g_fame_value, %eax
1:  ret
    .globl _o_fame_tramp
_o_fame_tramp:
    pushl %ecx
    pushl %ebx
    pushl %ebp
    pushl %esi
    xorl %ebx, %ebx
    jmp *_p_fame_cont

# ---- PC10: jmp at 0x40fe00 ("may trade in town EDX", fastcall). "All nations friendly" -> always 1.
    .globl _stub_trade_ok
_stub_trade_ok:
    cmpl $0, _g_friendly_on
    je 1f
    movl $1, %eax
    ret
1:  movl %edx, %ecx
    shll $5, %ecx
    jmp *_p_tradeok_cont

# ---- PC11: replaces "call 0x4068f0" at 0x406db5 (tavern person picker). In: EAX = highlighted person, EDI = out ptr
    .globl _stub_tavern_pick
_stub_tavern_pick:
    pushl %ecx
    pushl %edx
    pushl %eax
    call _c_tavern_pre
    addl $4, %esp
    popl %edx
    popl %ecx
    jmp *_p_tavern_pick

# ---- PC11: jmp at 0x47e651 (add esp,0x14 ; test eax,eax) after "The ships collide". EAX = [0x8caaa4]
    .globl _stub_surrender
_stub_surrender:
    addl $0x14, %esp
    pushl %eax
    pushl %ecx
    pushl %edx
    call _c_surrender
    movl %eax, _g_surr_r
    popl %edx
    popl %ecx
    popl %eax
    cmpl $1, _g_surr_r
    je 1f
    cmpl $2, _g_surr_r
    je 2f
    testl %eax, %eax
    je 2f
    jmp *_p_surr_vanilla
1:  jmp *_p_surr_yes
2:  jmp *_p_surr_duel

# ---- PC12: int land_at(float x, float y) - the game's own sailing land test (as at 0x46914a), 1 = land
    .globl _land_at
_land_at:
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebp
    movl %esp, %ebp
    movl 20(%ebp), %ebx
    movl 24(%ebp), %esi
    movl 0x72cd78, %edi
    pushl $1
    pushl %esi
    pushl %ebx
    movb $1, %al
    call *_p_land1
    call *_p_land2
    movl %ebp, %esp
    testl %eax, %eax
    jne 1f
    pushl $0
    pushl $0x725618
    pushl %esi
    pushl %ebx
    movl 0x72cd78, %eax
    call *_p_land3
    movl %ebp, %esp
    testb %al, %al
    jne 1f
    xorl %eax, %eax
    jmp 2f
1:  movl $1, %eax
2:  popl %ebp
    popl %edi
    popl %esi
    popl %ebx
    ret

# ---- PC12: replaces "call 0x54aa00" at 0x503d01 (button-screen camera frustum, thiscall ECX, 7 stack args)
    .globl _stub_btn_frustum
_stub_btn_frustum:
    pushl %ecx
    pushl %edx
    leal 12(%esp), %eax
    pushl %eax
    call _c_btn_frustum
    addl $4, %esp
    popl %edx
    popl %ecx
    jmp *_p_frustum_set

# ---- PC12: replaces "call 0x4e00d0" at 0x4de162 (EDI = trade object, AL/BL = args)
    .globl _stub_shipbtn
_stub_shipbtn:
    cmpl $0, 0xb4(%edi)
    je 1f
    cmpl $0, 0xb8(%edi)
    je 1f
    jmp *_p_shipbtn
1:  pushal
    call _c_shipbtn_skip
    popal
    ret
