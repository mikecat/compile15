#include <cstdio>
#include <cinttypes>
#include <sstream>
#include "asm.hpp"

static std::string to_hex(uint32_t num, int digits = 0) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%0*" PRIX32, digits, num);
	return buf;
}

std::string asm_inst::to_string() const {
	std::stringstream inst;
	switch (kind) {
	case EMPTY: break;
	case LABEL: inst << "@" << label; break;
	case DB: inst << "DATAB #" << to_hex(params[0], 2); break;
	case DW: inst << "DATAW #" << to_hex(params[0], 4); break;
	case DD: inst <<"DATAL #" << to_hex(params[0], 8); break;
	case MOV_LIT: inst << "R" << params[0] << " = " << params[1]; break;
	case MOV_REG: inst << "R" << params[0] << " = R" << params[1]; break;
	case ADD_LIT: inst << "R" << params[0] << " += " << params[1]; break;
	case SUB_LIT: inst << "R" << params[0] << " -= " << params[1]; break;
	case ADD_PC_LIT: inst << "R" << params[0] << " = PC + " << params[1]; break;
	case ADD_REG_LIT: inst << "R" << params[0] << " = R" << params[1] << " + " << params[2]; break;
	case SUB_REG_LIT: inst << "R" << params[0] << " = R" << params[1] << " - " << params[2]; break;
	case ADD_REG_REG: inst << "R" << params[0] << " = R" << params[1] << " + R" << params[2]; break;
	case SUB_REG_REG: inst << "R" << params[0] << " = R" << params[1] << " - R" << params[2]; break;
	case NEG_REG: inst << "R" << params[0] << " = -R" << params[1]; break;
	case MUL_REG: inst << "R" << params[0] << " *= R" << params[1]; break;
	case SHL_REG_LIT:  inst << "R" << params[0] << " = R" << params[1] << " << " << params[2]; break;
	case SHR_REG_LIT:  inst << "R" << params[0] << " = R" << params[1] << " >> " << params[2]; break;
	case SHL_REG:  inst << "R" << params[0] << " <<= R" << params[1]; break;
	case SHR_REG:  inst << "R" << params[0] << " >>= R" << params[1]; break;
	case NOT_REG: inst << "R" << params[0] << " = ~R" << params[1]; break;
	case AND_REG: inst << "R" << params[0] << " &= R" << params[1]; break;
	case OR_REG: inst << "R" << params[0] << " |= R" << params[1]; break;
	case XOR_REG: inst << "R" << params[0] << " ~= R" << params[1]; break;
	case LDB_REG_LIT: inst << "R" << params[0] << " = [R" << params[1] << " + " << params[2] << "]"; break;
	case LDW_REG_LIT: inst << "R" << params[0] << " = [R" << params[1] << " + " << params[2] << "]W"; break;
	case LDL_REG_LIT: inst << "R" << params[0] << " = [R" << params[1] << " + " << params[2] << "]L"; break;
	case LDL_PC_LIT: inst << "R" << params[0] << " = [PC + " << params[1] << "]L"; break;
	case LDB_REG_REG: inst << "R" << params[0] << " = [R" << params[1] << " + R" << params[2] << "]"; break;
	case LDBS_REG_REG: inst << "R" << params[0] << " = [R" << params[1] << " + R" << params[2] << "]C"; break;
	case LDW_REG_REG: inst << "R" << params[0] << " = [R" << params[1] << " + R" << params[2] << "]W"; break;
	case LDWS_REG_REG: inst << "R" << params[0] << " = [R" << params[1] << " + R" << params[2] << "]S"; break;
	case LDL_REG_REG: inst << "R" << params[0] << " = [R" << params[1] << " + R" << params[2] << "]L"; break;
	case STB_REG_LIT: inst << "[R" << params[0] << " + " << params[1] << "] = R" << params[2]; break;
	case STW_REG_LIT: inst << "[R" << params[0] << " + " << params[1] << "]W = R" << params[2]; break;
	case STL_REG_LIT: inst << "[R" << params[0] << " + " << params[1] << "]L = R" << params[2]; break;
	case STB_REG_REG: inst << "[R" << params[0] << " + R" << params[1] << "] = R" << params[2]; break;
	case STW_REG_REG: inst << "[R" << params[0] << " + R" << params[1] << "]W = R" << params[2]; break;
	case STL_REG_REG: inst << "[R" << params[0] << " + R" << params[1] << "]L = R" << params[2]; break;
	case CMP_REG_LIT: inst << "R" << params[0] << " - " << params[1]; break;
	case CMP_REG_REG: inst << "R" << params[0] << " - R" << params[1]; break;
	case CADD_REG_REG: inst << "R" << params[0] << " + R" << params[1]; break;
	case TEST_REG_REG: inst << "R" << params[0] << " & R" << params[1]; break;
	case JCC:
		inst << "IF ";
		switch (params[0]) {
			case ZERO: inst << "0"; break;
			case NONZERO: inst << "!0"; break;
			case EQ: inst << "EQ"; break;
			case NEQ: inst << "NE"; break;
			case CARRY: inst << "CS"; break;
			case NO_CARRY: inst << "CC"; break;
			case GE_UNSIGN: inst << "CS"; break;
			case L_UNSIGN: inst << "CC"; break;
			case NEGATIVE: inst << "MI"; break;
			case NON_NEGATIVE: inst << "PL"; break;
			case OVERFLOW: inst << "VS"; break;
			case NO_OVERFLOW: inst << "VC"; break;
			case G_UNSIGN: inst << "HI"; break;
			case LE_UNSIGN: inst << "LS"; break;
			case GE_SIGN: inst << "GE"; break;
			case L_SIGN: inst << "LT"; break;
			case G_SIGN: inst << "GT"; break;
			case LE_SIGN: inst << "LE"; break;
			case ALWAYS: inst << "AL"; break;
		}
		inst << " GOTO @" << label;
		break;
	case JMP_DIRECT: inst << "GOTO @" << label; break;
	case JMP_INDIRECT: inst << "GOTO R" << params[0]; break;
	case CALL_DIRECT: inst << "GOSUB @" << label; break;
	case CALL_INDIRECT: inst << "GOSUB R" << params[0]; break;
	case RET: inst << "RET"; break;
	case PUSH_REGS: {
		bool isFirst = true;
		inst << "PUSH {";
		if (params[0] & 0x100) { inst << "LR"; isFirst = false; }
		for (int i = 0; i <= 7; i++) {
			if ((params[0] >> i) & 1) {
				if (!isFirst) inst << ", ";
				inst << "R" << i;
				isFirst = false;
			}
		}
		inst << "}";
		} break;
	case POP_REGS: {
		bool isFirst = true;
		inst << "POP {";
		if (params[0] & 0x100) { inst << "PC"; isFirst = false; }
		for (int i = 0; i <= 7; i++) {
			if ((params[0] >> i) & 1) {
				if (!isFirst) inst << ", ";
				inst << "R" << i;
				isFirst = false;
			}
		}
		inst << "}";
		} break;
	case ADDSP_LIT: inst << "SP += " << params[0]; break;
	case SUBSP_LIT: inst << "SP -= " << params[0]; break;
	case ADD_SP_LIT: inst << "R" << params[0] << " = SP + " << params[1]; break;
	case LDL_SP_LIT: inst << "R" << params[0] << " = [SP + " << params[1] << "]L"; break;
	case STL_SP_LIT: inst << "[SP + " << params[0] << "]L = R" << params[1]; break;
	case REV_REG: inst << "R" << params[0] << " = REV(R" << params[1] << ")"; break;
	case REV16_REG: inst << "R" << params[0] << " = REV16(R" << params[1] << ")"; break;
	case REVSH_REG: inst << "R" << params[0] << " = REVSH(R" << params[1] << ")"; break;
	case ASR_REG_LIT: inst << "R" << params[0] << " = ASR(R" << params[1] << ", " << params[2] << ")"; break;
	case ASR_REG: inst << "ASR R" << params[0] << ", R" << params[1]; break;
	case BIC_REG: inst << "BIC R" << params[0] << ", R" << params[1]; break;
	case ROR_REG: inst << "ROR R" << params[0] << ", R" << params[1]; break;
	case ADC_REG: inst << "ADC R" << params[0] << ", R" << params[1]; break;
	case SBC_REG: inst << "SBC R" << params[0] << ", R" << params[1]; break;
	case LDM_REGS: {
		bool isFirst = true;
		inst << "LDM R" << params[0] << ", {";
		for (int i = 0; i <= 7; i++) {
			if ((params[1] >> i) & 1) {
				if (!isFirst) inst << ", ";
				inst << "R" << i;
				isFirst = false;
			}
		}
		inst << "}";
		} break;
	case STM_REGS: {
		bool isFirst = true;
		inst << "STM R" << params[0] << ", {";
		for (int i = 0; i <= 7; i++) {
			if ((params[1] >> i) & 1) {
				if (!isFirst) inst << ", ";
				inst << "R" << i;
				isFirst = false;
			}
		}
		inst << "}";
		} break;
	case NOP: inst << "NOP"; break;
	case CPSID: inst << "CPSID"; break;
	case CPSIE: inst << "CPSIE"; break;
	case WFI: inst << "WFI"; break;
	}
	if (comment != "") {
		if (kind != EMPTY) inst << " ";
		inst << "' " << comment;
	}
	return inst.str();
}
