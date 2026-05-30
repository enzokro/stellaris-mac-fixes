/* Unit test for the instruction decoders used by Fix 4 (data-SEGV
 * recovery in stellaris_fix.c, v1.7+).
 *
 * The decoder algorithms are duplicated here intentionally — keeping
 * standalone tests that don't depend on the dylib's signal-handler
 * machinery makes correctness easy to verify and CI to wire up. If you
 * change `sfix_decode_simple_load` or `sfix_decode_cmp_mem_imm` in
 * stellaris_fix.c, mirror the change here and add a test case.
 *
 * Compile + run via Makefile: `make test_decoder` */
#include <stdio.h>
#include <stdint.h>

static int decode_load(const uint8_t *pc, int *dest_reg_idx) {
    int off = 0;
    uint8_t rex = 0;
    if ((pc[off] & 0xf0) == 0x40) { rex = pc[off]; off++; }
    if (!(rex & 0x08)) return 0;
    if (pc[off] != 0x8b) return 0;
    off++;
    uint8_t modrm = pc[off++];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t reg = ((modrm >> 3) & 7) | ((rex & 0x04) ? 8 : 0);
    uint8_t rm  = modrm & 7;
    if (rm == 4) return 0;
    if (mod == 0 && rm == 5) return 0;
    if (mod == 3) return 0;
    if (mod != 0 && mod != 1 && mod != 2) return 0;
    if (mod == 1) off += 1;
    else if (mod == 2) off += 4;
    *dest_reg_idx = reg;
    return off;
}

/* Mirror of sfix_decode_cmp_mem_imm (v1.10.2). */
struct cmp_decode { int instr_len; int operand_size; int64_t imm; };
static int decode_cmp(const uint8_t *pc, struct cmp_decode *out) {
    int off = 0;
    uint8_t rex = 0;
    int op_size = 4;
    if (pc[off] == 0x66) return 0;
    if ((pc[off] & 0xf0) == 0x40) {
        rex = pc[off];
        if (rex & 0x08) op_size = 8;
        off++;
    }
    uint8_t opcode = pc[off++];
    if (opcode == 0x80)      op_size = 1;
    else if (opcode == 0x83) ; /* op_size already set */
    else return 0;
    uint8_t modrm = pc[off++];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t subop = (modrm >> 3) & 7;
    uint8_t rm  = modrm & 7;
    if (subop != 7) return 0;
    if (mod == 3) return 0;
    if (rm == 4) return 0;
    if (mod == 0 && rm == 5) return 0;
    if (mod == 1) off += 1;
    else if (mod == 2) off += 4;
    int8_t imm8 = (int8_t)pc[off++];
    out->instr_len = off;
    out->operand_size = op_size;
    out->imm = (int64_t)imm8;
    (void)rex;
    return 1;
}

/* Mirror of sfix_apply_cmp_zero_flags's flag derivation. Returns packed:
 *   bit 0: CF, bit 1: PF, bit 2: AF, bit 3: ZF, bit 4: SF, bit 5: OF
 * (test-local packing, distinct from RFLAGS layout — keeps the test pure
 * arithmetic, no thread-state dependency). */
static unsigned cmp_zero_flags(int op_size, int64_t imm_s) {
    uint64_t imm_u, result_u;
    if (op_size == 1) {
        imm_u = (uint8_t)imm_s;
        result_u = (uint64_t)((uint8_t)(0u - (uint8_t)imm_u));
    } else if (op_size == 4) {
        imm_u = (uint32_t)imm_s;
        result_u = (uint64_t)((uint32_t)(0u - (uint32_t)imm_u));
    } else {
        imm_u = (uint64_t)imm_s;
        result_u = (uint64_t)(0ull - imm_u);
    }
    int zf = (imm_u == 0);
    int cf = (imm_u != 0);
    int sf, of;
    if (op_size == 1) { sf = (result_u >> 7) & 1; of = ((uint8_t)imm_s == 0x80); }
    else if (op_size == 4) { sf = (result_u >> 31) & 1; of = 0; }
    else { sf = (result_u >> 63) & 1; of = 0; }
    int af = ((imm_s & 0xf) != 0);
    uint8_t low = (uint8_t)(result_u & 0xff);
    low ^= low >> 4; low ^= low >> 2; low ^= low >> 1;
    int pf = !(low & 1);
    return (cf<<0) | (pf<<1) | (af<<2) | (zf<<3) | (sf<<4) | (of<<5);
}

/* Mirror of sfix_decode_call_indirect_mem (v1.10.3). */
static int decode_call(const uint8_t *pc) {
    int off = 0;
    if (pc[off] == 0x66) return 0;
    if ((pc[off] & 0xf0) == 0x40) off++;
    if (pc[off] != 0xff) return 0;
    off++;
    uint8_t modrm = pc[off++];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t subop = (modrm >> 3) & 7;
    uint8_t rm = modrm & 7;
    if (subop != 2) return 0;
    if (mod == 3) return 0;
    if (rm == 4) return 0;
    if (mod == 0 && rm == 5) return 0;
    if (mod == 1) off += 1;
    else if (mod == 2) off += 4;
    return off;
}

struct LoadC { const char *desc; uint8_t bytes[8]; int expect_len; int expect_reg; };
struct CmpC  { const char *desc; uint8_t bytes[8]; int expect_len; int expect_size; int64_t expect_imm; unsigned expect_flags; };
struct CallC { const char *desc; uint8_t bytes[8]; int expect_len; };

int main(void) {
    struct LoadC load_cases[] = {
        { "mov rax, [rdi]",      {0x48, 0x8B, 0x07},                      3, 0 },
        { "mov rax, [rdi+0x10]", {0x48, 0x8B, 0x47, 0x10},                4, 0 },
        { "mov rdx, [rax+0x8]",  {0x48, 0x8B, 0x50, 0x08},                4, 2 },
        { "mov r8, [rdi+0x10]",  {0x4C, 0x8B, 0x47, 0x10},                4, 8 },
        { "mov rcx,[rcx+0x218]", {0x48, 0x8B, 0x89, 0x18, 0x02, 0x00, 0x00}, 7, 1 },
        { "WRITE: mov [rdi],rax",{0x48, 0x89, 0x07},                      0, 0 },
        { "SIB: mov rax,[rsp]",  {0x48, 0x8B, 0x04, 0x24},                0, 0 },
        { "RIP-rel mov",         {0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00}, 0, 0 },
        { "32-bit mov eax",      {0x8B, 0x07},                            0, 0 },
        { "reg-direct mov",      {0x48, 0x8B, 0xC7},                      0, 0 },
    };

    /* Flag-bit indices in our test-local packing: CF=0,PF=1,AF=2,ZF=3,SF=4,OF=5 */
    #define F(cf,pf,af,zf,sf,of) ((cf)|((pf)<<1)|((af)<<2)|((zf)<<3)|((sf)<<4)|((of)<<5))

    struct CmpC cmp_cases[] = {
        /* The actual tonight's-crash instruction: cmp byte [rdi+0x30], 0 */
        { "cmp byte [rdi+0x30], 0",     {0x80, 0x7F, 0x30, 0x00},      4, 1, 0,
          /* 0 - 0: ZF=1, PF=1 (parity of 0=even), rest 0 */
          F(0,1,0,1,0,0) },
        /* Companion form on r15: 41 80 7f 30 00 (cmp byte [r15+0x30], 0) */
        { "cmp byte [r15+0x30], 0",     {0x41, 0x80, 0x7F, 0x30, 0x00}, 5, 1, 0,
          F(0,1,0,1,0,0) },
        /* cmp byte [rax], 1: result = 0 - 1 = 0xff, ZF=0, CF=1, SF=1, AF=1, PF=1 (0xff has 8 set bits=even), OF=0 */
        { "cmp byte [rax], 1",          {0x80, 0x38, 0x01},            3, 1, 1,
          F(1,1,1,0,1,0) },
        /* cmp byte [rax], -128 (0x80): triggers OF for op_size=1 */
        { "cmp byte [rax], 0x80",       {0x80, 0x38, 0x80},            3, 1, -128,
          /* 0 - 0x80 = 0x80; CF=1, SF=1, ZF=0, AF=0, PF=1 (0x80 has 1 set bit=odd → PF=0), OF=1 */
          F(1,0,0,0,1,1) },
        /* cmp dword [rdi+0x30], 0 (the 32-bit twin pattern) */
        { "cmp dword [rdi+0x30], 0",    {0x83, 0x7F, 0x30, 0x00},      4, 4, 0,
          F(0,1,0,1,0,0) },
        /* cmp qword [rdi+0x30], 0 with REX.W */
        { "cmp qword [rdi+0x30], 0",    {0x48, 0x83, 0x7F, 0x30, 0x00},5, 8, 0,
          F(0,1,0,1,0,0) },
        /* cmp dword [rdi], 0 (no disp) */
        { "cmp dword [rdi], 0",         {0x83, 0x3F, 0x00},            3, 4, 0,
          F(0,1,0,1,0,0) },
        /* cmp dword [rdi+disp32], 5 */
        { "cmp dword [rdi+0x218], 5",   {0x83, 0xBF, 0x18, 0x02, 0x00, 0x00, 0x05}, 7, 4, 5,
          /* 0 - 5 = 0xfffffffb; CF=1, SF=1, ZF=0, AF=1, OF=0, low byte 0xfb has 7 bits = odd → PF=0 */
          F(1,0,1,0,1,0) },
        /* Reject: cmp byte [rdi+0x30], 0 but with /6 (xor instead of cmp) */
        { "REJECT: xor byte [rdi+0x30], 0", {0x80, 0x77, 0x30, 0x00}, 0, 0, 0, 0 },
        /* Reject: register-direct (mod=3) */
        { "REJECT: cmp dil, 0 (reg-direct)", {0x40, 0x80, 0xFF, 0x00}, 0, 0, 0, 0 },
        /* Reject: SIB (rm=4) */
        { "REJECT: cmp byte [rsp], 0",  {0x80, 0x3C, 0x24, 0x00}, 0, 0, 0, 0 },
        /* Reject: RIP-relative (mod=0, rm=5) */
        { "REJECT: cmp byte [rip+x],0", {0x80, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x00}, 0, 0, 0, 0 },
        /* Reject: 16-bit operand size (0x66 prefix) */
        { "REJECT: 16-bit cmp",         {0x66, 0x83, 0x3F, 0x00}, 0, 0, 0, 0 },
        /* Reject: wrong opcode (mov, not group 1) */
        { "REJECT: not group 1",        {0x8B, 0x07}, 0, 0, 0, 0 },
    };

    struct CallC call_cases[] = {
        /* Tonight's exact cascade-second-fault bytes: call qword [rax+0x68] */
        { "call [rax+0x68]",            {0xFF, 0x50, 0x68},                3 },
        /* No-disp form: call qword [rax] */
        { "call [rax]",                 {0xFF, 0x10},                      2 },
        /* disp32 form: call qword [rdi+0x200] */
        { "call [rdi+0x200]",           {0xFF, 0x97, 0x00, 0x02, 0x00, 0x00}, 6 },
        /* REX-prefixed base register: call qword [r8+0x68] (REX.B for r8 base) */
        { "call [r8+0x68]",             {0x41, 0xFF, 0x50, 0x68},          4 },
        /* REX-prefixed disp32: call qword [r15+0x200] */
        { "call [r15+0x200]",           {0x41, 0xFF, 0x97, 0x00, 0x02, 0x00, 0x00}, 7 },
        /* Reject: FF /0 (inc qword [rax]) — RMW, not handled */
        { "REJECT: inc qword [rax]",    {0xFF, 0x00},                      0 },
        /* Reject: SIB (rm=4): call [rsp] */
        { "REJECT: call [rsp]",         {0xFF, 0x14, 0x24},                0 },
        /* Reject: RIP-relative call (mod=0, rm=5) */
        { "REJECT: call [rip+x]",       {0xFF, 0x15, 0x00, 0x00, 0x00, 0x00}, 0 },
        /* Reject: register-direct call rax (mod=3) */
        { "REJECT: call rax (reg)",     {0xFF, 0xD0},                      0 },
        /* Reject: 16-bit override (meaningless for indirect call) */
        { "REJECT: 16-bit prefix",      {0x66, 0xFF, 0x10},                0 },
    };

    int pass = 0, fail = 0;

    printf("\nload decoder\n");
    for (size_t i = 0; i < sizeof(load_cases)/sizeof(load_cases[0]); i++) {
        int reg = -1;
        int n = decode_load(load_cases[i].bytes, &reg);
        int ok = (n == load_cases[i].expect_len) && (n == 0 || reg == load_cases[i].expect_reg);
        printf("  %s %-30s (got len=%d reg=%d, want len=%d reg=%d)\n",
               ok ? "PASS" : "FAIL", load_cases[i].desc, n, reg,
               load_cases[i].expect_len, load_cases[i].expect_reg);
        if (ok) pass++; else fail++;
    }

    printf("\ncmp decoder + flag emulation\n");
    for (size_t i = 0; i < sizeof(cmp_cases)/sizeof(cmp_cases[0]); i++) {
        struct cmp_decode d = {0};
        int got = decode_cmp(cmp_cases[i].bytes, &d);
        int ok;
        unsigned got_flags = 0;
        if (cmp_cases[i].expect_len == 0) {
            ok = (got == 0);
        } else {
            got_flags = cmp_zero_flags(d.operand_size, d.imm);
            ok = got &&
                 d.instr_len == cmp_cases[i].expect_len &&
                 d.operand_size == cmp_cases[i].expect_size &&
                 d.imm == cmp_cases[i].expect_imm &&
                 got_flags == cmp_cases[i].expect_flags;
        }
        printf("  %s %-35s (len=%d size=%d imm=%lld flags=0x%02x; want len=%d size=%d imm=%lld flags=0x%02x)\n",
               ok ? "PASS" : "FAIL", cmp_cases[i].desc,
               d.instr_len, d.operand_size, (long long)d.imm, got_flags,
               cmp_cases[i].expect_len, cmp_cases[i].expect_size,
               (long long)cmp_cases[i].expect_imm, cmp_cases[i].expect_flags);
        if (ok) pass++; else fail++;
    }

    printf("\nindirect-call decoder\n");
    for (size_t i = 0; i < sizeof(call_cases)/sizeof(call_cases[0]); i++) {
        int n = decode_call(call_cases[i].bytes);
        int ok = (n == call_cases[i].expect_len);
        printf("  %s %-30s (got len=%d, want len=%d)\n",
               ok ? "PASS" : "FAIL", call_cases[i].desc, n, call_cases[i].expect_len);
        if (ok) pass++; else fail++;
    }

    printf("\n%d/%d pass\n", pass, pass + fail);
    return fail ? 1 : 0;
}
