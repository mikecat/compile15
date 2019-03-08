#ifndef ASM_HPP_GUARD_743BD006_142B_4AD8_B928_8E4A3FF3C24B
#define ASM_HPP_GUARD_743BD006_142B_4AD8_B928_8E4A3FF3C24B

#include <cstdint>
#include <string>

// LIT : リテラル
// REG : レジスタ
// メモリアクセス系以外は、dest(REG)を省略して表記する
enum asm_inst_kind {
	EMPTY, // 空行 or コメントのみ
	LABEL, // ラベル
	DB, // 1バイトのデータ
	DW, // 2バイトのデータ
	DD, // 4バイトのデータ
	MOV_LIT,
	MOV_REG,
	ADD_LIT,
	SUB_LIT,
	ADD_PC_LIT,
	ADD_REG_LIT,
	SUB_REG_LIT,
	ADD_REG_REG,
	SUB_REG_REG,
	NEG_REG,
	MUL_REG,
	SHL_REG_LIT,
	SHR_REG_LIT,
	SHL_REG,
	SHR_REG,
	NOT_REG,
	AND_REG,
	OR_REG,
	XOR_REG,
	LDB_REG_LIT, // load byte
	LDW_REG_LIT, // load word
	LDL_REG_LIT, // load long
	LDL_PC_LIT,
	LDB_REG_REG,
	LDBS_REG_REG, // load byte signed
	LDW_REG_REG,
	LDWS_REG_REG, // load word signed
	LDL_REG_REG,
	STB_REG_LIT, // store byte
	STW_REG_LIT, // store word
	STL_REG_LIT, // store long
	STB_REG_REG,
	STW_REG_REG,
	STL_REG_REG,
	CMP_REG_LIT, // SUB and drop result
	CMP_REG_REG,
	CADD_REG_REG, // ADD and drop result
	TEST_REG_REG, // AND and drop result
	JCC,
	JMP_DIRECT,
	JMP_INDIRECT,
	CALL_DIRECT,
	CALL_INDIRECT,
	RET,
	PUSH_REGS,
	POP_REGS,
	ADDSP_LIT, // SP += u7
	SUBSP_LIT, // SP -= u7
	ADD_SP_LIT, // Rd = SP + u8
	LDL_SP_LIT,
	STL_SP_LIT,
	REV_REG,
	REV16_REG,
	REVSH_REG,
	ASR_REG_LIT,
	ASR_REG,
	BIC_REG,
	ROR_REG,
	ADC_REG,
	SBC_REG,
	LDM_REGS,
	STM_REGS,
	NOP,
	CPSID,
	CPSIE,
	WFI
};

enum jcc_cond {
	ZERO,
	NONZERO,
	EQ,
	NEQ,
	CARRY,
	NO_CARRY,
	GE_UNSIGN,
	L_UNSIGN,
	NEGATIVE,
	NON_NEGATIVE,
	OVERFLOW,
	NO_OVERFLOW,
	G_UNSIGN,
	LE_UNSIGN,
	GE_SIGN,
	L_SIGN,
	G_SIGN,
	LE_SIGN,
	ALWAYS
};

struct asm_inst {
	asm_inst_kind kind;
	std::string label;
	uint32_t params[3];
	std::string comment;

	asm_inst() {}
	asm_inst(asm_inst_kind kind_, const std::string& label_,
	uint32_t p0 = 0, uint32_t p1 = 0, uint32_t p2 = 0) :
		kind(kind_), label(label_), params{p0, p1, p2} {}
	asm_inst(asm_inst_kind kind_, uint32_t p0, const std::string& label_,
	uint32_t p1 = 0, uint32_t p2 = 0) :
		kind(kind_), label(label_), params{p0, p1, p2} {}
	asm_inst(asm_inst_kind kind_,
	uint32_t p0 = 0, uint32_t p1 = 0, uint32_t p2 = 0) :
		kind(kind_), label(), params{p0, p1, p2} {}

	std::string to_string() const;
};

#endif
