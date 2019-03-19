#include <vector>
#include "codegen_internal.hpp"

// 使えるレジスタの中から使うレジスタを適当に選ぶ
int get_reg_to_use(int lineno, int regs_available, bool prefer_callee_save) {
	if (prefer_callee_save) {
		// callee-saveレジスタを優先
		for (int i = 7; i >= 0; i--) {
			if ((regs_available >> i) & 1) return i;
		}
	} else {
		// caller-saveレジスタを優先
		for (int i = 0; i < 8; i++) {
			if ((regs_available >> i) & 1) return i;
		}
	}
	throw codegen_error(lineno, "no registers available");
}

// 式のコード生成を行う
// * want_result: 結果の値が欲しいか (いらない場合、後置インクリメントなどでコードが減る場合がある)
// * result_prefer_reg : 結果の値を置いてほしいレジスタ (負の数を指定すると自動になる)
// * regs_available : 値を保存しなくていいレジスタ (ビットマスク)
// * stack_extra_offset : 値の退避で使われたスタックのバイト数 (ローカル変数アクセスの補正用)
// result_prefer_regはregs_availableに入っているレジスタでなくてもいい
// 返されたresult_regがregs_availableに入っていない場合、result_regにデータが入っているが破壊してはいけない
//   (レジスタ変数やグローバル変数アクセス用レジスタなど)
// result_prefer_regが非負の場合、指定されたレジスタはregs_availableに入っていなくても破壊(結果の配置)してよい
//   (レジスタ変数への代入など)
// status.registers_writtenの更新を忘れないように！ (callee-saveレジスタの退避に用いる情報)
codegen_expr_result codegen_expr(expression_node* expr, int lineno, bool want_result,
int result_prefer_reg, int regs_available, int stack_extra_offset, codegen_status& status) {
	if (expr == nullptr) {
		throw codegen_error(lineno, "NULL passed to codegen_expr()");
	}
	std::vector<asm_inst> result;
	int result_reg = -1;
	switch (expr->kind) {
	case EXPR_INTEGER_LITERAL:
		// 整数リテラル → その値をレジスタに置く
		if (want_result) {
			result_reg = result_prefer_reg >= 0 ? result_prefer_reg :
				get_reg_to_use(lineno, regs_available, false);
			status.registers_written |= 1 << result_reg;
			std::vector<asm_inst> ncode = codegen_put_number(result_reg, expr->info.value);
			result.insert(result.end(), ncode.begin(), ncode.end());
		}
		break;
	case EXPR_IDENTIFIER:
		// 識別子 → メモリ上の場合は、そのアドレスをレジスタに置く
		if (want_result) {
			var_info* vinfo = expr->info.ident.info;
			if (vinfo == nullptr) throw codegen_error(lineno, "identifier information is NULL");
			if (vinfo->is_register) {
				// レジスタ上の場合、コードは無い
				result_reg = status.lv_reg_assign.at(vinfo->offset);
			} else {
				int offset = vinfo->offset;
				if (vinfo->is_global) {
					// グローバル変数 : 起点レジスタからのオフセット
					int gvreg = status.gv_access_register;
					if (offset == 0) {
						// そのまま
						result_reg = gvreg;
					} else {
						result_reg = result_prefer_reg >= 0 ? result_prefer_reg :
							get_reg_to_use(lineno, regs_available, false);
						status.registers_written |= 1 << result_reg;
						if (result_reg == gvreg) {
							throw codegen_error(lineno, "global variable access register will be broken");
						}
						if (0 < offset) {
							if (offset < 8) {
								result.push_back(asm_inst(ADD_REG_LIT, result_reg, gvreg, offset));
							} else if (offset < 256) {
								result.push_back(asm_inst(MOV_REG, result_reg, gvreg));
								result.push_back(asm_inst(ADD_LIT, result_reg, offset));
							} else if (offset <= 255 * 2) {
								result.push_back(asm_inst(MOV_REG, result_reg, gvreg));
								result.push_back(asm_inst(ADD_LIT, result_reg, 255));
								result.push_back(asm_inst(ADD_LIT, result_reg, offset - 255));
							} else {
								std::vector<asm_inst> ncode = codegen_put_number(result_reg, offset);
								result.insert(result.end(), ncode.begin(), ncode.end());
								result.push_back(asm_inst(ADD_REG, result_reg, gvreg));
							}
						} else {
							int noffset = -offset;
							if (noffset < 8) {
								result.push_back(asm_inst(SUB_REG_LIT, result_reg, gvreg, noffset));
							} else if (noffset < 256) {
								result.push_back(asm_inst(MOV_REG, result_reg, gvreg));
								result.push_back(asm_inst(SUB_LIT, result_reg, noffset));
							} else if (noffset <= 255 * 2) {
								result.push_back(asm_inst(MOV_REG, result_reg, gvreg));
								result.push_back(asm_inst(SUB_LIT, result_reg, 255));
								result.push_back(asm_inst(SUB_LIT, result_reg, noffset - 255));
							} else {
								// offset + gvreg なので、引き算命令にはできない
								std::vector<asm_inst> ncode = codegen_put_number(result_reg, offset);
								result.insert(result.end(), ncode.begin(), ncode.end());
								result.push_back(asm_inst(ADD_REG, result_reg, gvreg));
							}
						}
					}
				} else {
					// ローカル変数 : スタックポインタからのオフセット
					const int stack_reg = 13;
					result_reg = result_prefer_reg >= 0 ? result_prefer_reg :
						get_reg_to_use(lineno, regs_available, false);
					status.registers_written |= 1 << result_reg;
					offset += stack_extra_offset;
					if (offset == 0) {
						result.push_back(asm_inst(MOV_REG, result_reg, stack_reg));
					} else if (0 < offset && offset % 4 == 0 && offset / 4 < 256) {
						result.push_back(asm_inst(ADD_SP_LIT, result_reg, offset / 4));
					} else if (0 < offset && offset < 256) {
						result.push_back(asm_inst(MOV_REG, result_reg, stack_reg));
						result.push_back(asm_inst(ADD_LIT, result_reg, offset));
					} else if (255 <= offset && offset <= 255 * 2) {
						result.push_back(asm_inst(MOV_REG, result_reg, stack_reg));
						result.push_back(asm_inst(ADD_LIT, result_reg, 255));
						result.push_back(asm_inst(ADD_LIT, result_reg, offset - 255));
					} else if (-256 < offset && offset < 0) {
						result.push_back(asm_inst(MOV_REG, result_reg, stack_reg));
						result.push_back(asm_inst(SUB_LIT, result_reg, -offset));
					} else if (-(255 * 2) < offset && offset <= -255) {
						result.push_back(asm_inst(MOV_REG, result_reg, stack_reg));
						result.push_back(asm_inst(SUB_LIT, result_reg, 255));
						result.push_back(asm_inst(SUB_LIT, result_reg, (-offset) - 255));
					} else {
						std::vector<asm_inst> ncode = codegen_put_number(result_reg, offset);
						result.insert(result.end(), ncode.begin(), ncode.end());
						result.push_back(asm_inst(ADD_REG, result_reg, stack_reg));
					}
				}
			}
		}
		break;
	case EXPR_OPERATOR:
		switch (expr->info.op.kind) {
		default:
			throw codegen_error(lineno, "unsupported or invalid operator");
		}
		break;
	}
	if (result_reg >= 0 && result_prefer_reg >= 0 && result_reg != result_prefer_reg) {
		result.push_back(asm_inst(MOV_REG, result_prefer_reg, result_reg));
		result_reg = result_prefer_reg;
	}
	return codegen_expr_result(result, result_reg);
}
