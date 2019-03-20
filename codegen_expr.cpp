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

// 指定のノードのポインタを、一発でメモリアクセスできる形で表そうとする
offset_fold_result* offset_fold(expression_node* node) {
	if (node == nullptr) return nullptr;
	switch (node->kind) {
	case EXPR_INTEGER_LITERAL:
		return nullptr;
	case EXPR_IDENTIFIER:
		return new offset_fold_result(node->info.ident.info, 0, nullptr, nullptr, false);
	case EXPR_OPERATOR:
		switch (node->info.op.kind) {
		case OP_NONE:
		case OP_PARENTHESIS:
		case OP_ADDRESS: // 「値をまだ読まれていないポインタ」から「ポインタ」に状態を変えるだけ
		case OP_INDIRECTION: // 「ポインタ」から「値をまだ読まれていないポインタ」に状態を変えるだけ
		case OP_ARRAY_TO_POINTER: // 「配列のアドレス」から「配列の先頭要素のポインタ」に状態を変えるだけ
		case OP_FUNC_TO_FPTR: // 「関数のアドレス」から「関数ポインタ」に状態を変えるだけ
			return offset_fold(node->info.op.operands[0]);
		case OP_CAST:
			// ポインタへのキャストなら、メモリアクセスが可能となる
			if (node->info.op.cast_to != nullptr && node->info.op.cast_to->kind == TYPE_POINTER) {
				offset_fold_result* ofr = offset_fold(node->info.op.operands[0]);
				if(ofr != nullptr) {
					return ofr;
				} else {
					return new offset_fold_result(nullptr, 0, node->info.op.operands[0], nullptr, false);
				}
			}
			break;
		case OP_ARRAY_REF: // A[B] -> *((A)+(B)) 間接演算子は状態を変えるだけ
		case OP_ADD:
			{
				offset_fold_result* ofr;
				expression_node *ptr_node = nullptr, *integer_node = nullptr;
				if (node->info.op.operands[0]->type != nullptr &&
				node->info.op.operands[0]->type->kind == TYPE_POINTER) {
					ptr_node = node->info.op.operands[0];
					integer_node = node->info.op.operands[1];
				} else if (node->info.op.operands[1]->type != nullptr &&
				node->info.op.operands[1]->type->kind == TYPE_POINTER) {
					ptr_node = node->info.op.operands[1];
					integer_node = node->info.op.operands[0];
				}
				if (ptr_node != nullptr && ptr_node->type->info.target_type != nullptr) {
					// ポインタ + 整数 or 整数 + ポインタ
					if (integer_node->kind == EXPR_INTEGER_LITERAL) {
						uint32_t raw_offset = integer_node->info.value;
						int offset = raw_offset & UINT32_C(0x80000000) ? -(int)(-raw_offset) : raw_offset;
						offset *= ptr_node->type->info.target_type->size;
						ofr = offset_fold(ptr_node);
						if (ofr != nullptr && ofr->vinfo != nullptr && ofr->offset_node == nullptr) {
							// 変数情報があってノードを評価せずにアクセスできる → そこにオフセットを加える
							return new offset_fold_result(ofr->vinfo, ofr->additional_offset + offset,
								ptr_node, nullptr, false);
						} else {
							// 変数情報が使えない → ノードの評価を行う
							return new offset_fold_result(nullptr, offset, ptr_node, nullptr, false);
						}
					} else {
						// レジスタ + レジスタ を用いる
						return new offset_fold_result(nullptr, 0, ptr_node, integer_node, false);
					}
				}
			}
			break;
		case OP_SUB:
			if (node->info.op.operands[0]->type != nullptr &&
			node->info.op.operands[0]->type->kind == TYPE_POINTER) {
				expression_node* ptr_node = node->info.op.operands[0];
				expression_node* integer_node = node->info.op.operands[1];
				if (integer_node->kind == EXPR_INTEGER_LITERAL) {
					uint32_t raw_offset = integer_node->info.value;
					int offset = raw_offset & UINT32_C(0x80000000) ? -(int)(-raw_offset) : raw_offset;
					offset *= ptr_node->type->info.target_type->size;
					offset_fold_result* ofr = offset_fold(ptr_node);
					if (ofr != nullptr && ofr->vinfo != nullptr && ofr->offset_node == nullptr) {
						// 変数情報があってノードを評価せずにアクセスできる → そこにオフセットを加える
						return new offset_fold_result(ofr->vinfo, ofr->additional_offset - offset,
							ptr_node, nullptr, false);
					} else {
						// 変数情報が使えない → ノードの評価を行う
						return new offset_fold_result(nullptr, -offset, ptr_node, nullptr, true);
					}
				} else {
					// レジスタ + レジスタ を用いる
					return new offset_fold_result(nullptr, 0, ptr_node, integer_node, true);
				}
			}
			break;
		default:
			// 直接メモリアクセスには向かない演算子
			break;
		}
		break;
	}
	return nullptr;
}

// メモリアクセス(レジスタ変数を含む)のコード生成を行う
codegen_expr_result codegen_mem(expression_node* expr, offset_fold_result* ofr, int lineno,
expression_node* value_node, bool is_write,
int result_prefer_reg, int regs_available, int stack_extra_offset, codegen_status& status) {
	if (expr == nullptr || ofr == nullptr) {
		throw codegen_error(lineno, "NULL passed to codegen_mem()");
	}
	if (expr->type == nullptr) {
		throw codegen_error(lineno, "type missing for memory access");
	}
	std::vector<asm_inst> result;
	int result_reg = -1;
	if (ofr->vinfo != NULL && ofr->vinfo->is_register) {
		int variable_reg = status.lv_reg_assign.at(ofr->vinfo->offset);
		if (is_write) {
			codegen_expr_result value_res = codegen_expr(value_node, lineno, true,
				variable_reg, regs_available, stack_extra_offset, status);
			result.insert(result.end(), value_res.insts.begin(), value_res.insts.end());
			if (value_res.result_reg != variable_reg) {
				result.push_back(asm_inst(MOV_REG, variable_reg, value_res.result_reg));
				status.registers_written |= 1 << variable_reg;
			}
			if (expr->type->size < 4) {
				// TODO: 符号拡張 or ゼロ拡張
				throw codegen_error(lineno, "writing register variable with size smaller than 4 not supported yet");
			}
		} else {
			if (result_prefer_reg < 0) {
				result_reg = variable_reg;
			} else {
				result_reg = result_prefer_reg;
				result.push_back(asm_inst(MOV_REG, result_reg, variable_reg));
				status.registers_written |= 1 << result_reg;
			}
		}
	} else {
		// 符号付きで小さい領域を読み込むには、Rn + u5 が使えない
		if (ofr->offset_node == nullptr &&
		(expr->type->size == 4 || expr->type->kind != TYPE_INTEGER || !expr->type->info.is_signed)) {
			// 基準レジスタ+即値オフセット or アドレス(ノード評価)
			bool direct_ok = false, use_stack_buffer = false;
			int variable_reg = status.gv_access_register;
			int offset = 0;
			if (ofr->vinfo != nullptr) {
				offset = ofr->vinfo->offset + ofr->additional_offset;
				// 射程距離内なら、直接アクセスできる (普通のレジスタ:u5, SP:u8)
				if (offset % expr->type->size == 0 && 0 <= offset && ((offset / expr->type->size) < 32 ||
				(!ofr->vinfo->is_global && expr->type->size == 4 && (offset / expr->type->size) < 256))) {
					direct_ok = true;
					offset /= expr->type->size;
				}
				// スタックを用いる場合、直接SPを使えるのは4バイトの場合のみ
				if (!ofr->vinfo->is_global && expr->type->size != 4) {
					use_stack_buffer = true;
				}
			}
			codegen_expr_result expr_variable, expr_value;
			int regs_available2 = regs_available;
			// TODO: スケジューリング
			if (!direct_ok) {
				// 直接アクセスできないので、式を評価してアドレスをレジスタに積んでもらう
				expr_variable = codegen_expr(expr, lineno, true, -1,
					regs_available, stack_extra_offset, status);
				offset = 0;
				result.insert(result.end(), expr_variable.insts.begin(), expr_variable.insts.end());
				regs_available2 &= ~(1 << expr_variable.result_reg);
				variable_reg = expr_variable.result_reg;
			} else if (use_stack_buffer) {
				// 直接SPを使えないので、SPの値を他のレジスタにコピーする
				variable_reg = result_prefer_reg >= 0 ? result_prefer_reg :
					get_reg_to_use(lineno, regs_available, false);
				regs_available2 &= ~(1 << variable_reg);
				status.registers_written |= 1 << variable_reg;
				result.push_back(asm_inst(MOV_REG, variable_reg, 13));
			}
			if (is_write) {
				expr_value = codegen_expr(value_node, lineno, true, -1,
					regs_available2, stack_extra_offset, status);
				result.insert(result.end(), expr_value.insts.begin(), expr_value.insts.end());
				asm_inst_kind inst = EMPTY;
				switch (expr->type->size) {
				case 1: inst = STB_REG_LIT; break;
				case 2: inst = STW_REG_LIT; break;
				case 4: inst = STL_REG_LIT; break;
				default: throw codegen_error(lineno, "unsupported memory access size");
				}
				// 一般アドレス or グローバル変数(基準レジスタを使用) or 射程距離外 or SPをコピーして使用
				if (ofr->vinfo == nullptr || ofr->vinfo->is_global || !direct_ok || use_stack_buffer) {
					result.push_back(asm_inst(inst, variable_reg, offset, expr_value.result_reg));
				} else {
					result.push_back(asm_inst(STL_SP_LIT, offset, expr_value.result_reg));
				}
			} else {
				// 読んだ瞬間アドレスのレジスタは用済みなので、regs_availableで良い
				result_reg = result_prefer_reg >= 0 ? result_prefer_reg :
					get_reg_to_use(lineno, regs_available, false);
				status.registers_written |= 1 << result_reg;
				asm_inst_kind inst = EMPTY;
				switch (expr->type->size) {
				case 1: inst = LDB_REG_LIT; break;
				case 2: inst = LDW_REG_LIT; break;
				case 4: inst = LDL_REG_LIT; break;
				default: throw codegen_error(lineno, "unsupported memory access size");
				}
				// 一般アドレス or グローバル変数(基準レジスタを使用) or 射程距離外 or SPをコピーして使用
				if (ofr->vinfo == nullptr || ofr->vinfo->is_global || !direct_ok || use_stack_buffer) {
					result.push_back(asm_inst(inst, result_reg, variable_reg, offset));
				} else {
					result.push_back(asm_inst(LDL_SP_LIT, result_reg, offset));
				}
			}
		} else {
			// レジスタ+レジスタ (ノード評価)
			codegen_expr_result expr_variable, expr_offset, expr_value;
			// TODO: スケジューリング
			expr_variable = codegen_expr(ofr->vnode, lineno, true, -1,
				regs_available, stack_extra_offset, status);
			result.insert(result.end(), expr_variable.insts.begin(), expr_variable.insts.end());
			int regs_available2 = regs_available & ~(1 << expr_variable.result_reg);
			int regs_available3 = regs_available2;
			int offset_reg;
			if (ofr->offset_node != nullptr) {
				expr_offset = codegen_expr(ofr->offset_node, lineno, true, -1,
					regs_available2, stack_extra_offset, status);
				regs_available3 = regs_available & ~(1 << expr_offset.result_reg);
				result.insert(result.end(), expr_offset.insts.begin(), expr_offset.insts.end());
				offset_reg = expr_offset.result_reg;
				if (expr->type->size == 2 || expr->type->size == 4 || ofr->negate_offset_node) {
					// オフセットが書き込み禁止なので、別のレジスタを使う
					if (!((regs_available2 >> offset_reg) & 1)) {
						// どうせ読んだ値を書いて壊すレジスタが決まっているなら、それを使う
						offset_reg = result_prefer_reg >= 0 ? result_prefer_reg :
							get_reg_to_use(lineno, regs_available2, false);
					}
					if (expr->type->size > 1) {
						result.push_back(asm_inst(SHL_REG_LIT,
							offset_reg, expr_offset.result_reg, expr->type->size == 2 ? 1 : 2));
						if (ofr->negate_offset_node) {
							result.push_back(asm_inst(NEG_REG, offset_reg, offset_reg));
						}
					} else { // 上位のifより、ofr->negate_offset_nodeがtrue
						result.push_back(asm_inst(NEG_REG, offset_reg, expr_offset.result_reg));
					}
					status.registers_written |= 1 << offset_reg;
				}
			} else {
				// オフセットを適当なレジスタに置く
				// どうせ読んだ値を書いて壊すレジスタが決まっているなら、それを使う
				offset_reg = result_prefer_reg >= 0 ? result_prefer_reg :
					get_reg_to_use(lineno, regs_available2, false);
				std::vector<asm_inst> ncode = codegen_put_number(offset_reg, ofr->additional_offset);
				result.insert(result.end(), ncode.begin(), ncode.end());
				status.registers_written |= 1 << offset_reg;
			}
			// TODO: レジスタ数に余裕が無い時はADD_REG命令を使ってまとめる
			if (is_write) {
				expr_value = codegen_expr(value_node, lineno, true, -1,
					regs_available3, stack_extra_offset, status);
				result.insert(result.end(), expr_value.insts.begin(), expr_value.insts.end());
				asm_inst_kind inst = EMPTY;
				switch (expr->type->size) {
				case 1: inst = STB_REG_REG; break;
				case 2: inst = STW_REG_REG; break;
				case 4: inst = STL_REG_REG; break;
				default: throw codegen_error(lineno, "unsupported memory access size");
				}
				result.push_back(asm_inst(inst,
					expr_variable.result_reg, offset_reg, expr_value.result_reg));
			} else {
				// 読んだ瞬間アドレスのレジスタは用済みなので、regs_availableで良い
				result_reg = result_prefer_reg >= 0 ? result_prefer_reg :
					get_reg_to_use(lineno, regs_available, false);
				status.registers_written |= 1 << result_reg;
				asm_inst_kind inst = EMPTY;
				bool is_signed = (expr->type->kind == TYPE_INTEGER && expr->type->info.is_signed);
				switch (expr->type->size) {
				case 1: inst = is_signed ? LDBS_REG_REG : LDB_REG_REG; break;
				case 2: inst = is_signed ? LDWS_REG_REG : LDW_REG_REG; break;
				case 4: inst = LDL_REG_REG; break;
				default: throw codegen_error(lineno, "unsupported memory access size");
				}
				result.push_back(asm_inst(inst,
					result_reg, expr_variable.result_reg, offset_reg));
			}
		}
	}
	return codegen_expr_result(result, result_reg);
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
		// 何もしない単項演算子たち
		case OP_NONE: case OP_PARENTHESIS: // スルー
		case OP_ADDRESS: case OP_INDIRECTION: // 状態を変えるだけ
		case OP_ARRAY_TO_POINTER: case OP_FUNC_TO_FPTR: // bitcast
		case OP_PLUS: // 拡張は子で行う
			{
				codegen_expr_result res = codegen_expr(expr->info.op.operands[0], lineno, want_result,
					result_prefer_reg, regs_available, stack_extra_offset, status);
				result = res.insts;
				result_reg = res.result_reg;
			}
			break;
		// メモリ(やレジスタ変数)から値を読み出す
		case OP_READ_VALUE:
			{
				offset_fold_result* ofr = offset_fold(expr->info.op.operands[0]);
				codegen_expr_result res = codegen_mem(expr->info.op.operands[0], ofr, lineno,
					nullptr, false, result_prefer_reg, regs_available, stack_extra_offset, status);
				result = res.insts;
				result_reg = res.result_reg;
			}
			break;
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
