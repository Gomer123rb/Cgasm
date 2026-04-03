#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

// Opcodes
#define OPC_DEF 0x10
#define OPC_MOV 0x01
#define OPC_ADD 0x02
#define OPC_MUL 0x03
#define OPC_DP3 0x04
#define OPC_FRC 0x05
#define OPC_SLT 0x06
#define OPC_SGE 0x07
#define OPC_MAD 0x08
#define OPC_DP4  0x09
#define OPC_RCP  0x0A
#define OPC_RSQ  0x0B
#define OPC_MIN  0x0C
#define OPC_MAX  0x0D
#define OPC_TEX  0x0E      // tex2D
#define OPC_TEXB 0x0F      // tex2Dbias
#define OPC_LIT  0x11      // lit

static void trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

static int lookup_reg(const char *name) {
    int neg = 0;

    // Handle unary minus
    if (name[0] == '-') {
        neg = 1;
        name++;
    }

    int reg_id = -1;

    if (strcmp(name, "oPos") == 0) {
        reg_id = 32;
    }
    else if (name[0] == 'o' && name[1] == 'D') {
        int idx = atoi(name + 2);
        reg_id = 40 + idx; // oD0 = 40
    }
    else if (name[0] == 'o' && name[1] == 'T') {
        int idx = atoi(name + 2);
        reg_id = 48 + idx; // oT0 = 48
    }
    else if (name[0] == 'v') {
        int idx = atoi(name + 1);
        reg_id = idx;      // v0 = 0
    }
    else if (name[0] == 'c') {
        int idx = atoi(name + 1);
        reg_id = 16 + idx; // c0 = 16
    }
    else if (name[0] == 'r') {
        int idx = atoi(name + 1);
        reg_id = 80 + idx; // r0 = 80
    }

    if (reg_id < 0)
        return -1;

    // Apply negation flag in high bit
    return (neg ? 0x80 : 0) | reg_id;
}

static uint8_t parse_mask(const char *s) {
    if (!s || !s[0]) return 0x0F;
    if (s[0] != '.') return 0x0F;
    uint8_t m = 0;
    for (const char *p = s + 1; *p; ++p) {
        if (*p == 'x') m |= 1 << 0;
        if (*p == 'y') m |= 1 << 1;
        if (*p == 'z') m |= 1 << 2;
        if (*p == 'w') m |= 1 << 3;
    }
    return m ? m : 0x0F;
}

// Parse full swizzle (e.g. ".zyxw" -> 2bits per component)
// Each 2 bits encode source: 00=x, 01=y, 10=z, 11=w
static uint8_t parse_swizzle(const char *s) {
    if (!s || !s[0] || s[0] != '.') return 0xE4; // default .xyzw
    
    const char swiz[] = "xyzw";
    uint8_t result = 0;
    int comp = 0;
    
    for (const char *p = s + 1; *p && comp < 4; ++p) {
        for (int i = 0; i < 4; ++i) {
            if (*p == swiz[i]) {
                result |= (i & 0x3) << (comp * 2);
                comp++;
                break;
            }
        }
    }
    
    // Fill remaining with identity
    while (comp < 4) {
        result |= comp << (comp * 2);
        comp++;
    }
    
    return result;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s input.asm output.bin\n", argv[0]);
        return 1;
    }

    FILE *fin = fopen(argv[1], "r");
    if (!fin) { perror("input"); return 1; }

    FILE *fout = fopen(argv[2], "wb");
    if (!fout) { perror("output"); fclose(fin); return 1; }

    char line[512];

    while (fgets(line, sizeof(line), fin)) {
        trim(line);
        if (!line[0] || line[0] == ';') continue;

        // --- DEF handling ---
        if (!strncmp(line, "def", 3) || !strncmp(line, "DEF", 3)) {
            char reg[32];
            float x, y, z, w;

            if (sscanf(line, "def %31[^,], %f, %f, %f, %f",
                       reg, &x, &y, &z, &w) != 5 &&
                sscanf(line, "DEF %31[^,], %f, %f, %f, %f",
                       reg, &x, &y, &z, &w) != 5) {
                fprintf(stderr, "bad def: %s\n", line);
                continue;
            }

            trim(reg);
            int reg_id = lookup_reg(reg);
            if (reg_id < 0) {
                fprintf(stderr, "unknown reg in def: %s\n", line);
                continue;
            }

            uint8_t op = OPC_DEF;
            uint8_t id = (uint8_t)reg_id;

            fwrite(&op, 1, 1, fout);
            fwrite(&id, 1, 1, fout);
            fwrite(&x, 4, 1, fout);
            fwrite(&y, 4, 1, fout);
            fwrite(&z, 4, 1, fout);
            fwrite(&w, 4, 1, fout);
            continue;
        }

        // --- instruction handling ---
        char op[32]   = {0};
        char dst[64]  = {0};
        char src1[64] = {0};
        char src2[64] = {0};

        int n = sscanf(line, "%31s %63[^,], %63[^,], %63s",
                       op, dst, src1, src2);

        if (n < 3) {
            fprintf(stderr, "parse error: %s\n", line);
            continue;
        }

        trim(op);
        trim(dst);
        trim(src1);
        trim(src2);

        uint8_t opcode = 0;
	if (!strcasecmp(op, "mov"))      opcode = OPC_MOV;
		else if (!strcasecmp(op, "add")) opcode = OPC_ADD;
		else if (!strcasecmp(op, "mul")) opcode = OPC_MUL;
		else if (!strcasecmp(op, "dp3")) opcode = OPC_DP3;
		else if (!strcasecmp(op, "dp4")) opcode = OPC_DP4;
		else if (!strcasecmp(op, "frc")) opcode = OPC_FRC;
		else if (!strcasecmp(op, "slt")) opcode = OPC_SLT;
		else if (!strcasecmp(op, "sge")) opcode = OPC_SGE;
		else if (!strcasecmp(op, "mad")) opcode = OPC_MAD;
		else if (!strcasecmp(op, "rcp")) opcode = OPC_RCP;
		else if (!strcasecmp(op, "rsq")) opcode = OPC_RSQ;
		else if (!strcasecmp(op, "min")) opcode = OPC_MIN;
		else if (!strcasecmp(op, "max")) opcode = OPC_MAX;
        else if (!strcasecmp(op, "lit")) opcode = OPC_LIT;

	// --- NVIDIA texture mnemonics ---
	else if (!strcasecmp(op, "tex2D") ||
         	!strcasecmp(op, "texld"))
    	opcode = OPC_TEX;

	else if (!strcasecmp(op, "tex2Dbias") ||
         	!strcasecmp(op, "texld_bias"))
    	opcode = OPC_TEXB;

	// --- NTint mnemonics ---
	else if (!strcasecmp(op, "tex"))
    		opcode = OPC_TEX;

	else if (!strcasecmp(op, "texb"))
    		opcode = OPC_TEXB;

	else {
    	   fprintf(stderr, "unknown op: %s\n", op);
    	   continue;
	}



        // --- split dst into base + mask (e.g. oT1.xyz) ---
        char dst_base[64] = {0};
        char dst_mask_str[16] = {0};
        char *dst_dot = strchr(dst, '.');
        if (dst_dot) {
            size_t len = (size_t)(dst_dot - dst);
            if (len >= sizeof(dst_base)) len = sizeof(dst_base) - 1;
            memcpy(dst_base, dst, len);
            dst_base[len] = '\0';
            strncpy(dst_mask_str, dst_dot, sizeof(dst_mask_str) - 1);
        } else {
            strncpy(dst_base, dst, sizeof(dst_base) - 1);
        }

        // --- src1 may have swizzle: c1.xxyz ---
        char src1_base[64] = {0};
        char src1_swiz[16] = {0};
        char *dot = strchr(src1, '.');
        if (dot) {
            size_t len = (size_t)(dot - src1);
            if (len >= sizeof(src1_base)) len = sizeof(src1_base) - 1;
            memcpy(src1_base, src1, len);
            src1_base[len] = '\0';
            strncpy(src1_swiz, dot, sizeof(src1_swiz) - 1);
        } else {
            strncpy(src1_base, src1, sizeof(src1_base) - 1);
        }

        int dst_id = lookup_reg(dst_base);
        int s1_id  = lookup_reg(src1_base);
        int s2_id  = (n >= 4) ? lookup_reg(src2) : -1;

        if (dst_id < 0 || s1_id < 0) {
            fprintf(stderr, "bad regs in line: %s\n", line);
            continue;
        }

        uint8_t inst[4];
        inst[0] = opcode;
        inst[1] = (uint8_t)dst_id;

        if (opcode == OPC_MOV) {
            uint8_t dst_mask = parse_mask(dst_mask_str);
            uint8_t src_mask = parse_mask(src1_swiz);
            uint8_t packed = (uint8_t)(((dst_mask & 0x0F) << 4) | (src_mask & 0x0F));
            inst[2] = (uint8_t)s1_id;
            inst[3] = packed;
        } else if (opcode == OPC_ADD ||
                   opcode == OPC_MUL ||
                   opcode == OPC_DP3 ||
		   opcode == OPC_DP4 ||
                   opcode == OPC_SLT ||
                   opcode == OPC_SGE ||
		   opcode == OPC_MIN ||
		   opcode == OPC_MAX) {
            if (s2_id < 0) {
                fprintf(stderr, "missing src2 in line: %s\n", line);
                continue;
            }
            inst[2] = (uint8_t)s1_id;
            inst[3] = (uint8_t)s2_id;
        } else if (opcode == OPC_FRC ||
         	   opcode == OPC_RCP ||
                   opcode == OPC_RSQ ||
                   opcode == OPC_LIT) {
          // For these unary ops, respect write mask and full source swizzle
          uint8_t dst_mask = parse_mask(dst_mask_str);
          uint8_t src_swiz = parse_swizzle(src1_swiz);
          inst[2] = (uint8_t)s1_id;
          inst[3] = src_swiz;  // Now encodes full swizzle, not just mask
        } else if (opcode == OPC_TEX || opcode == OPC_TEXB) {
    		// assume: tex r0, v0, t0
    		// dst = r#, src1 = coord (v#), src2 = sampler (c#/r#/special)
    	if (s2_id < 0) {
        	fprintf(stderr, "missing sampler in tex: %s\n", line);
        	continue;
    	}
    	inst[2] = (uint8_t)s1_id;  // coord
    	inst[3] = (uint8_t)s2_id;  // sampler
        	} else if (opcode == OPC_MAD) {
    if (s2_id < 0) {
        fprintf(stderr, "missing src2 in mad: %s\n", line);
        continue;
    }

    // parse src3 manually
    char *p = strchr(line, ',');
    if (!p) { fprintf(stderr, "bad mad: %s\n", line); continue; }
    p = strchr(p+1, ',');
    if (!p) { fprintf(stderr, "bad mad: %s\n", line); continue; }
    p = strchr(p+1, ',');
    if (!p) { fprintf(stderr, "bad mad: %s\n", line); continue; }

    char src3[64];
    strcpy(src3, p+1);
    trim(src3);

    int s3_id = lookup_reg(src3);
    if (s3_id < 0) {
        fprintf(stderr, "bad src3 in mad: %s\n", line);
        continue;
    }

    uint8_t inst5[5];
    inst5[0] = OPC_MAD;
    inst5[1] = (uint8_t)dst_id;
    inst5[2] = (uint8_t)s1_id;
    inst5[3] = (uint8_t)s2_id;
    inst5[4] = (uint8_t)s3_id;

    fwrite(inst5, 1, 5, fout);
    continue;
}


        fwrite(inst, 1, 4, fout);
    } 

    fclose(fin);
    fclose(fout);
    return 0;
}
