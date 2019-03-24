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
		return new offset_fold_result(node->info.ident.info, 0, node, nullptr, false);
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

// キャッシュを用いたメモリ/レジスタ変数アクセスのコード生成を行う
codegen_expr_result codegen_mem_from_cache(const codegen_mem_cache& cache, int lineno,
int input_or_result_prefer_reg, bool is_write,
bool prefer_callee_save, int regs_available, codegen_status& status) {
	std::vector<asm_inst> result;
	int result_reg = -1;
	if (cache.is_register) {
		if (cache.use_two_params) {
			throw codegen_error(lineno, "register variable won't use two params");
		}
		if (is_write) {
			if (cache.mem_param1 != (uint32_t)input_or_result_prefer_reg) {
				result.push_back(asm_inst(MOV_REG, cache.mem_param1, input_or_result_prefer_reg));
				status.registers_written |= 1 << cache.mem_param1;
			}
			if (cache.size < 4) {
				// 符号拡張 or ゼロ拡張
				int shift_width = 8 * (4 - cache.size);
				result.push_back(asm_inst(SHL_REG_LIT, cache.mem_param1, cache.mem_param1, shift_width));
				result.push_back(asm_inst(cache.is_signed ? ASR_REG_LIT : SHR_REG_LIT,
					cache.mem_param1, cache.mem_param1, shift_width));
				status.registers_written |= 1 << cache.mem_param1;
			}
		} else {
			result_reg = input_or_result_prefer_reg >= 0 ? input_or_result_prefer_reg : cache.mem_param1;
			if ((uint32_t)result_reg != cache.mem_param1) {
				result.push_back(asm_inst(MOV_REG, result_reg, cache.mem_param1));
				status.registers_written |= 1 << result_reg;
			}
		}
	} else {
		if (is_write) {
			if (cache.use_two_params) {
				result.push_back(asm_inst(cache.write_inst,
					cache.mem_param1, cache.mem_param2, input_or_result_prefer_reg));
			} else {
				result.push_back(asm_inst(cache.write_inst,
					cache.mem_param1, input_or_result_prefer_reg));
			}
		} else {
			result_reg = input_or_result_prefer_reg >= 0 ? input_or_result_prefer_reg :
				get_reg_to_use(lineno, regs_available, prefer_callee_save);
			status.registers_written |= 1 << result_reg;
			if (cache.use_two_params) {
				result.push_back(asm_inst(cache.read_inst,
					result_reg, cache.mem_param1, cache.mem_param2));
			} else {
				result.push_back(asm_inst(cache.read_inst,
					result_reg, cache.mem_param1));
			}
		}
	}
	return codegen_expr_result(result, result_reg);
}

// メモリアクセス(レジスタ変数を含む)のコード生成を行う
codegen_mem_result codegen_mem(expression_node* expr, offset_fold_result* ofr, int lineno,
expression_node* value_node, bool is_write, bool preserve_cache, bool prefer_callee_save,
int result_prefer_reg, int regs_available, int stack_extra_offset, codegen_status& status) {
	if (expr == nullptr || ofr == nullptr) {
		throw codegen_error(lineno, "NULL passed to codegen_mem()");
	}
	if (expr->type == nullptr) {
		throw codegen_error(lineno, "type missing for memory access");
	}
	std::vector<asm_inst> result;
	int result_reg = -1;
	codegen_mem_cache cache;
	cache.size = expr->type->size;
	cache.is_signed = expr->type->kind == TYPE_INTEGER && expr->type->info.is_signed;
	if (ofr->vinfo != NULL && ofr->vinfo->is_register) {
		int variable_reg = status.lv_reg_assign.at(ofr->vinfo->offset);
		codegen_expr_result value_res;
		cache.is_register = true;
		cache.use_two_params = false;
		cache.read_inst = cache.write_inst = EMPTY;
		cache.mem_param1 = variable_reg;
		cache.mem_param2 = 0;
		cache.regs_in_cache = 1 << variable_reg;
		if (is_write) {
			value_res = codegen_expr(value_node, lineno, true, false,
				variable_reg, regs_available, stack_extra_offset, status);
			result.insert(result.end(), value_res.insts.begin(), value_res.insts.end());
		}
		codegen_expr_result access_res = codegen_mem_from_cache(cache, lineno,
			is_write ? value_res.result_reg : result_prefer_reg, is_write,
			prefer_callee_save, regs_available, status);
		result.insert(result.end(), access_res.insts.begin(), access_res.insts.end());
		result_reg = access_res.result_reg;
	} else {
		cache.is_register = false;
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
			// キャッシュを保存する場合、キャッシュ対象が壊すレジスタに割り当てられないようにする
			if (preserve_cache && result_prefer_reg >= 0) regs_available2 &= ~(1 << result_prefer_reg);
			// 値の方を先に評価するべきなら、する
			bool value_evaluated = false;
			if (is_write && cmp_expr_info(expr->hint, value_node->hint) < 0) {
				expr_value = codegen_expr(value_node, lineno, true,
					!direct_ok && expr->hint != nullptr && expr->hint->func_call_exists,
					-1, regs_available2, stack_extra_offset, status);
				result.insert(result.end(), expr_value.insts.begin(), expr_value.insts.end());
				regs_available2 &= ~(1 << expr_value.result_reg);
				value_evaluated = true;
			}
			bool prefer_callee_save_variable = is_write && !value_evaluated &&
				value_node->hint != nullptr && value_node->hint->func_call_exists;
			if (!direct_ok) {
				// 直接アクセスできないので、式を評価してアドレスをレジスタに積んでもらう
				expr_variable = codegen_expr(expr, lineno, true, prefer_callee_save_variable,
					-1, regs_available2, stack_extra_offset, status);
				offset = 0;
				result.insert(result.end(), expr_variable.insts.begin(), expr_variable.insts.end());
				regs_available2 &= ~(1 << expr_variable.result_reg);
				variable_reg = expr_variable.result_reg;
			} else if (use_stack_buffer) {
				// 直接SPを使えないので、SPの値を他のレジスタにコピーする
				variable_reg = result_prefer_reg >= 0 && !preserve_cache ? result_prefer_reg :
					get_reg_to_use(lineno, regs_available2, prefer_callee_save_variable);
				regs_available2 &= ~(1 << variable_reg);
				status.registers_written |= 1 << variable_reg;
				result.push_back(asm_inst(MOV_REG, variable_reg, 13));
			}
			if (is_write) {
				if (!value_evaluated) {
					expr_value = codegen_expr(value_node, lineno, true, false, -1,
						regs_available2, stack_extra_offset, status);
					result.insert(result.end(), expr_value.insts.begin(), expr_value.insts.end());
					regs_available2 &= ~(1 << expr_value.result_reg);
				}
			}
			// 一般アドレス or グローバル変数(基準レジスタを使用) or 射程距離外 or SPをコピーして使用
			if (ofr->vinfo == nullptr || ofr->vinfo->is_global || !direct_ok || use_stack_buffer) {
				cache.use_two_params = true;
				switch (expr->type->size) {
				case 1:
					cache.read_inst = LDB_REG_LIT;
					cache.write_inst = STB_REG_LIT;
					break;
				case 2:
					cache.read_inst = LDW_REG_LIT;
					cache.write_inst = STW_REG_LIT;
					break;
				case 4:
					cache.read_inst = LDL_REG_LIT;
					cache.write_inst = STL_REG_LIT;
					break;
				default:
					throw codegen_error(lineno, "unsupported memory access size");
				}
				cache.mem_param1 = variable_reg;
				cache.mem_param2 = offset;
				cache.regs_in_cache = 1 << variable_reg;
			} else {
				cache.use_two_params = false;
				cache.read_inst = LDL_SP_LIT;
				cache.write_inst = STL_SP_LIT;
				cache.mem_param1 = offset;
				cache.mem_param2 = 0;
				cache.regs_in_cache = 0;
			}
			// 読んだ瞬間アドレスのレジスタは用済みになる場合、regs_availableで良い
			codegen_expr_result access_res = codegen_mem_from_cache(cache, lineno,
				is_write ? expr_value.result_reg : result_prefer_reg, is_write,
				prefer_callee_save, preserve_cache ? regs_available2 : regs_available, status);
			result.insert(result.end(), access_res.insts.begin(), access_res.insts.end());
			result_reg = access_res.result_reg;
		} else {
			// レジスタ+レジスタ (ノード評価)
			codegen_expr_result expr_variable, expr_offset, expr_value;
			expr_info* variable_hint = ofr->vnode->hint;
			expr_info* offset_hint = ofr->offset_node != nullptr ? ofr->offset_node->hint : nullptr;
			expr_info* value_hint = is_write ? value_node->hint : nullptr;
			bool variable_generated = false, value_generated = false;
			int regs_available2 = regs_available;
			// キャッシュを保存する場合、キャッシュ対象が壊すレジスタに割り当てられないようにする
			if (preserve_cache && result_prefer_reg >= 0) regs_available2 &= ~(1 << result_prefer_reg);
			if (cmp_expr_info(variable_hint, offset_hint) <= 0) {
				bool offset_funcall_exists = (offset_hint != nullptr && offset_hint->func_call_exists);
				if (is_write && cmp_expr_info(value_hint, variable_hint) < 0) {
					expr_value = codegen_expr(value_node, lineno, true,
						(variable_hint != nullptr && variable_hint->func_call_exists) ||
							offset_funcall_exists,
						-1, regs_available2, stack_extra_offset, status);
					result.insert(result.end(), expr_value.insts.begin(), expr_value.insts.end());
					regs_available2 = regs_available2 & ~(1 << expr_value.result_reg);
					value_generated = true;
				}
				expr_variable = codegen_expr(ofr->vnode, lineno, true,
					(!is_write && !value_generated &&
						value_hint != nullptr && value_hint->func_call_exists) ||
						offset_funcall_exists,
					-1, regs_available2, stack_extra_offset, status);
				result.insert(result.end(), expr_variable.insts.begin(), expr_variable.insts.end());
				regs_available2 = regs_available2 & ~(1 << expr_variable.result_reg);
				variable_generated = true;
				if (is_write && !value_generated && cmp_expr_info(value_hint, offset_hint) < 0) {
					expr_value = codegen_expr(value_node, lineno, true, offset_funcall_exists, -1,
						regs_available2, stack_extra_offset, status);
					result.insert(result.end(), expr_value.insts.begin(), expr_value.insts.end());
					regs_available2 = regs_available2 & ~(1 << expr_value.result_reg);
					value_generated = true;
				}
			}
			bool offset_prefer_callee_save =
				(!variable_generated && variable_hint != nullptr && variable_hint->func_call_exists) ||
				(is_write && !value_generated &&
					value_hint != nullptr && value_hint->func_call_exists);
			int offset_reg;
			if (ofr->offset_node != nullptr) {
				expr_offset = codegen_expr(ofr->offset_node, lineno, true, offset_prefer_callee_save, -1,
					regs_available2, stack_extra_offset, status);
				result.insert(result.end(), expr_offset.insts.begin(), expr_offset.insts.end());
				regs_available2 = regs_available2 & ~(1 << expr_offset.result_reg);
				offset_reg = expr_offset.result_reg;
				if (expr->type->size == 2 || expr->type->size == 4 || ofr->negate_offset_node) {
					// オフセットが書き込み禁止なので、別のレジスタを使う
					if (!((regs_available2 >> offset_reg) & 1)) {
						// どうせ読んだ値を書いて壊すレジスタが決まっているなら、それを使う
						offset_reg = result_prefer_reg >= 0 && !preserve_cache ? result_prefer_reg :
							get_reg_to_use(lineno, regs_available2, offset_prefer_callee_save);
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
				offset_reg = result_prefer_reg >= 0 && !preserve_cache ? result_prefer_reg :
					get_reg_to_use(lineno, regs_available2, offset_prefer_callee_save);
				std::vector<asm_inst> ncode = codegen_put_number(offset_reg, ofr->additional_offset);
				result.insert(result.end(), ncode.begin(), ncode.end());
				status.registers_written |= 1 << offset_reg;
				regs_available2 = regs_available2 & ~(1 << offset_reg);
			}
			// TODO: レジスタ数に余裕が無い時はADD_REG命令を使ってまとめる
			if (!variable_generated && (!is_write || cmp_expr_info(variable_hint, value_hint) <= 0)) {
				expr_variable = codegen_expr(ofr->vnode, lineno, true,
					is_write && !value_generated &&
						value_hint != nullptr && value_hint->func_call_exists,
					-1, regs_available, stack_extra_offset, status);
				result.insert(result.end(), expr_variable.insts.begin(), expr_variable.insts.end());
				regs_available2 = regs_available2 & ~(1 << expr_variable.result_reg);
				variable_generated = true;
			}
			if (is_write) {
				if (!value_generated) {
					expr_value = codegen_expr(value_node, lineno, true,
						!variable_generated &&
							variable_hint != nullptr && variable_hint->func_call_exists,
						-1, regs_available2, stack_extra_offset, status);
					result.insert(result.end(), expr_value.insts.begin(), expr_value.insts.end());
					regs_available2 = regs_available2 & ~(1 << expr_value.result_reg);
					value_generated = true;
				}
				if (!variable_generated) {
					expr_variable = codegen_expr(ofr->vnode, lineno, true, false, -1,
						regs_available, stack_extra_offset, status);
					result.insert(result.end(), expr_variable.insts.begin(), expr_variable.insts.end());
					regs_available2 = regs_available2 & ~(1 << expr_variable.result_reg);
					variable_generated = true;
				}
			}
			cache.use_two_params = true;
			switch (expr->type->size) {
			case 1:
				cache.read_inst = cache.is_signed ? LDBS_REG_REG : LDB_REG_REG;
				cache.write_inst = STB_REG_REG;
				break;
			case 2:
				cache.read_inst = cache.is_signed ? LDWS_REG_REG : LDW_REG_REG;
				cache.write_inst = STW_REG_REG;
				break;
			case 4:
				cache.read_inst = LDL_REG_REG;
				cache.write_inst = STL_REG_REG;
				break;
			default:
				throw codegen_error(lineno, "unsupported memory access size");
			}
			cache.mem_param1 = expr_variable.result_reg;
			cache.mem_param2 = offset_reg;
			cache.regs_in_cache = (1 << expr_variable.result_reg) | (1 << offset_reg);
			// 読んだ瞬間アドレスのレジスタは用済みになる場合、regs_availableで良い
			codegen_expr_result access_res = codegen_mem_from_cache(cache, lineno,
				is_write ? expr_value.result_reg : result_prefer_reg, is_write,
				prefer_callee_save, preserve_cache ? regs_available2 : regs_available, status);
			result.insert(result.end(), access_res.insts.begin(), access_res.insts.end());
			result_reg = access_res.result_reg;
		}
	}
	return codegen_mem_result(codegen_expr_result(result, result_reg), cache);
}

// 式のコード生成を行う
// * want_result: 結果の値が欲しいか (いらない場合、後置インクリメントなどでコードが減る場合がある)
// * prefer_callee_save: 結果用のレジスタ割り当て時にcallee-saveレジスタを優先すべきか
//   (通常は関数の出入り時の退避を避けるためcaller-saveレジスタ優先)
//   (後の評価で関数呼び出しが控えている場合は、そこでの退避を避けるためcallee-saveレジスタ優先)
// * result_prefer_reg : 結果の値を置いてほしいレジスタ (負の数を指定すると自動になる)
// * regs_available : 値を保存しなくていいレジスタ (ビットマスク)
// * stack_extra_offset : 値の退避で使われたスタックのバイト数 (ローカル変数アクセスの補正用)
// result_prefer_regはregs_availableに入っているレジスタでなくてもいい
// 返されたresult_regがregs_availableに入っていない場合、result_regにデータが入っているが破壊してはいけない
//   (レジスタ変数やグローバル変数アクセス用レジスタなど)
// result_prefer_regが非負の場合、指定されたレジスタはregs_availableに入っていなくても破壊(結果の配置)してよい
//   (レジスタ変数への代入など)
// status.registers_writtenの更新を忘れないように！ (callee-saveレジスタの退避に用いる情報)
codegen_expr_result codegen_expr(expression_node* expr, int lineno, bool want_result, bool prefer_callee_save,
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
				get_reg_to_use(lineno, regs_available, prefer_callee_save);
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
							get_reg_to_use(lineno, regs_available, prefer_callee_save);
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
						get_reg_to_use(lineno, regs_available, prefer_callee_save);
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
				codegen_expr_result res = codegen_expr(
					expr->info.op.operands[0], lineno, want_result, prefer_callee_save,
					result_prefer_reg, regs_available, stack_extra_offset, status);
				result = res.insts;
				result_reg = res.result_reg;
			}
			break;
		// 後置インクリメント/デクリメント
		case OP_POST_INC: case OP_POST_DEC:
			{
				// 加減算の幅を設定する
				int add_size = 1;
				if (is_pointer_type(expr->type) && expr->type->info.target_type != nullptr) {
					add_size = expr->type->info.target_type->size;
				}
				// 値を読み込む
				offset_fold_result* ofr = offset_fold(expr->info.op.operands[0]);
				codegen_mem_result res = codegen_mem(expr->info.op.operands[0], ofr, lineno,
					nullptr, false, true, prefer_callee_save,
					result_prefer_reg, regs_available, stack_extra_offset, status);
				if (res.cache.is_register && result_prefer_reg >= 0) {
					// レジスタ変数だった場合、書き込み先レジスタの指定を解除して生成し直す
					res = codegen_mem(expr->info.op.operands[0], ofr, lineno,
						nullptr, false, true, prefer_callee_save,
						-1, regs_available, stack_extra_offset, status);
				}
				result = res.code.insts;
				if (want_result) {
					// 評価結果の値を要求されているので、保存する
					result_reg = res.code.result_reg;
					// TODO: レジスタに余裕が無い時は、一度書き換えた値を戻すことで処理を行う
					if (res.cache.is_register) {
						// レジスタ変数
						// 結果用に別のレジスタを用意し、値をコピーする
						result_reg = result_prefer_reg >= 0 && result_reg != result_prefer_reg ?
							result_prefer_reg :
							get_reg_to_use(lineno,
								regs_available & ~res.cache.regs_in_cache & ~result_reg, prefer_callee_save);
						result.push_back(asm_inst(MOV_REG, result_reg, res.code.result_reg));
						// 結果が格納されていたレジスタを更新し、書き込む
						result.push_back(asm_inst(expr->info.op.kind == OP_POST_INC ? ADD_LIT : SUB_LIT,
							res.code.result_reg, add_size));
						codegen_expr_result wres = codegen_mem_from_cache(res.cache, lineno,
							res.code.result_reg, true, false, regs_available & ~(1 << result_reg), status);
						result.insert(result.end(), wres.insts.begin(), wres.insts.end());
					} else {
						// メモリ変数
						// 作業用に別のレジスタを用意する
						int work_reg = get_reg_to_use(lineno,
							regs_available & ~res.cache.regs_in_cache & ~(1 << result_reg), false);
						// そのレジスタに更新後の値を書き込む
						result.push_back(asm_inst(expr->info.op.kind == OP_POST_INC ? ADD_REG_LIT : SUB_REG_LIT,
							work_reg, result_reg, add_size));
						// それを変数に書き込む
						codegen_expr_result wres = codegen_mem_from_cache(res.cache, lineno,
							work_reg, true, false, regs_available & ~(1 << result_reg), status);
						result.insert(result.end(), wres.insts.begin(), wres.insts.end());
					}
				} else {
					// 評価結果の値は要求されていないので、破壊する
					// res.code.result_regは書き込み先の変数レジスタまたは空きレジスタのはずなので、破壊してよい
					result.push_back(asm_inst(
						expr->info.op.kind == OP_POST_INC ? ADD_LIT : SUB_LIT, res.code.result_reg, add_size));
					codegen_expr_result wres = codegen_mem_from_cache(res.cache, lineno,
						res.code.result_reg, true, false, regs_available, status);
					result.insert(result.end(), wres.insts.begin(), wres.insts.end());
				}
			}
			break;
		// 前置インクリメント/デクリメント
		case OP_PRE_INC: case OP_PRE_DEC:
			{
				// 加減算の幅を設定する
				int add_size = 1;
				if (is_pointer_type(expr->type) && expr->type->info.target_type != nullptr) {
					add_size = expr->type->info.target_type->size;
				}
				// 値を読み込む
				offset_fold_result* ofr = offset_fold(expr->info.op.operands[0]);
				codegen_mem_result res = codegen_mem(expr->info.op.operands[0], ofr, lineno,
					nullptr, false, true, prefer_callee_save,
					result_prefer_reg, regs_available, stack_extra_offset, status);
				if (res.cache.is_register && result_prefer_reg >= 0) {
					// レジスタ変数だった場合、書き込み先レジスタの指定を解除して生成し直す
					res = codegen_mem(expr->info.op.operands[0], ofr, lineno,
						nullptr, false, true, prefer_callee_save,
						-1, regs_available, stack_extra_offset, status);
				}
				result = res.code.insts;
				result_reg = res.code.result_reg;
				// 値を更新する
				result.push_back(asm_inst(
					expr->info.op.kind == OP_PRE_INC ? ADD_LIT : SUB_LIT, result_reg, add_size));
				bool do_extend = want_result && expr->type != nullptr && expr->type->size < 4;
				bool read_again = false;
				if (do_extend) {
					if (res.cache.is_register) {
						read_again = true;
					} else {
						// 符号拡張 or ゼロ拡張
						int shift_width = 8 * (4 - expr->type->size);
						result.push_back(asm_inst(SHL_REG_LIT, result_reg, result_reg, shift_width));
						result.push_back(asm_inst(
							expr->type->kind == TYPE_INTEGER && expr->type->info.is_signed ? ASR_REG_LIT : SHR_REG_LIT,
							result_reg, result_reg, shift_width));
					}
				}
				codegen_expr_result wres = codegen_mem_from_cache(res.cache, lineno,
					result_reg, true, read_again,
					read_again ? regs_available & ~res.cache.regs_in_cache : regs_available, status);
				result.insert(result.end(), wres.insts.begin(), wres.insts.end());
				// レジスタ変数の場合、変数から値を読み込む
				// 結果格納用と同じレジスタになっているはずだが、念の為
				if (do_extend && res.cache.is_register) {
					codegen_expr_result rres = codegen_mem_from_cache(res.cache, lineno,
						result_reg, false, false, regs_available, status);
					result.insert(result.end(), rres.insts.begin(), rres.insts.end());
					result_reg = rres.result_reg;
				}
			}
			break;
		// sizeof 式 (sizeof(型)は構文解析の時点で整数リテラルに変換される)
		case OP_SIZEOF:
			if (want_result) {
				if (expr->type == nullptr) {
					throw codegen_error(lineno, "size of null type requested");
				}
				result_reg = result_prefer_reg >= 0 ? result_prefer_reg :
					get_reg_to_use(lineno, regs_available, prefer_callee_save);
				status.registers_written |= 1 << result_reg;
				std::vector<asm_inst> ncode = codegen_put_number(result_reg, expr->type->size);
				result.insert(result.end(), ncode.begin(), ncode.end());
			}
			break;
		// その他の単項演算子
		case OP_NEG: // 単項-
		case OP_NOT: // 単項~
		case OP_CAST: // キャスト
			{
				codegen_expr_result res = codegen_expr(
					expr->info.op.operands[0], lineno, want_result, prefer_callee_save,
					result_prefer_reg, regs_available, stack_extra_offset, status);
				result = res.insts;
				if ((result_prefer_reg >= 0 && res.result_reg == result_prefer_reg) ||
				res.result_reg < 0 || ((regs_available >> res.result_reg) & 1) != 0) {
					// 降ってきた結果のレジスタが使い回せる状況
					// * 降ってきた結果のレジスタがこのノードを格納するべきレジスタと同じ
					// * そもそも降ってきた結果のレジスタが無い
					// * 降ってきた結果のレジスタが空いている
					result_reg = res.result_reg;
				} else {
					// 降ってきた結果のレジスタが使い回せないので、新たなレジスタを割り当てる
					result_reg = get_reg_to_use(lineno, regs_available, prefer_callee_save);
				}
				if (want_result) {
					if (result_reg < 0) {
						throw codegen_error(lineno, "wanted result not given");
					}
					switch (expr->info.op.kind) {
					case OP_NEG:
						result.push_back(asm_inst(NEG_REG, result_reg, res.result_reg));
						break;
					case OP_NOT:
						result.push_back(asm_inst(NOT_REG, result_reg, res.result_reg));
						break;
					case OP_CAST:
						{
							type_node* type = expr->info.op.cast_to;
							if (type != nullptr && type->size < 4) {
								// 符号拡張 or ゼロ拡張
								int shift_width = 8 * (4 - type->size);
								result.push_back(asm_inst(SHL_REG_LIT, result_reg, res.result_reg, shift_width));
								result.push_back(asm_inst(
									type->kind == TYPE_INTEGER && type->info.is_signed ? ASR_REG_LIT : SHR_REG_LIT,
									result_reg, result_reg, shift_width));
							}
						}
						break;
					default:
						throw codegen_error(lineno, "unexpected operator kind given");
					}
					status.registers_written |= 1 << result_reg;
				}
			}
			break;
		// メモリ(やレジスタ変数)から値を読み出す
		case OP_READ_VALUE:
			{
				offset_fold_result* ofr = offset_fold(expr->info.op.operands[0]);
				codegen_mem_result res = codegen_mem(expr->info.op.operands[0], ofr, lineno,
					nullptr, false, false, prefer_callee_save,
					result_prefer_reg, regs_available, stack_extra_offset, status);
				result = res.code.insts;
				result_reg = res.code.result_reg;
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
