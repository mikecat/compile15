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
			if (is_pointer_type(node->info.op.cast_to)) {
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
				if (is_pointer_type(node->info.op.operands[0]->type)) {
					ptr_node = node->info.op.operands[0];
					integer_node = node->info.op.operands[1];
				} else if (is_pointer_type(node->info.op.operands[1]->type)) {
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
			if (is_pointer_type(node->info.op.operands[0]->type)) {
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
			// ポインタを返す演算子なら、メモリアクセスが可能
			if (is_pointer_type(node->type)) {
				return new offset_fold_result(nullptr, 0, node, nullptr, false);
			}
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
			result_reg = input_or_result_prefer_reg;
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
			result_reg = input_or_result_prefer_reg;
		} else {
			result_reg = input_or_result_prefer_reg >= 0 ? input_or_result_prefer_reg :
				get_reg_to_use(lineno, regs_available, prefer_callee_save);
			if (cache.use_two_params) {
				result.push_back(asm_inst(cache.read_inst,
					result_reg, cache.mem_param1, cache.mem_param2));
			} else {
				result.push_back(asm_inst(cache.read_inst,
					result_reg, cache.mem_param1));
			}
			status.registers_written |= 1 << result_reg;
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
		if (is_write && value_node->kind == EXPR_INTEGER_LITERAL) {
			uint32_t value = value_node->info.value;
			uint32_t mask = (UINT32_C(0xffffffff) >> (8 * (4 - cache.size)));
			if (cache.size < 4 && cache.is_signed && ((value >> (8 * cache.size - 1)) & 1)) {
				value |= ~mask; // 符号拡張、負の場合
			} else {
				value &= mask; // ゼロ拡張 or 符号拡張、正の場合
			}
			std::vector<asm_inst> ncode = codegen_put_number(variable_reg, value);
			result.insert(result.end(), ncode.begin(), ncode.end());
			result_reg = variable_reg;
			status.registers_written |= 1 << variable_reg;
		} else {
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
		}
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
				offset = ofr->vinfo->offset + ofr->additional_offset + (ofr->vinfo->is_global ? 0 : stack_extra_offset);
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
				variable_reg = result_prefer_reg >= 0 && !preserve_cache &&
					(!value_evaluated || result_prefer_reg != expr_value.result_reg) ? result_prefer_reg :
						get_reg_to_use(lineno, regs_available2, prefer_callee_save_variable);
				regs_available2 &= ~(1 << variable_reg);
				result.push_back(asm_inst(MOV_REG, variable_reg, 13));
				status.registers_written |= 1 << variable_reg;
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
			int regs_decided = 0;
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
					regs_decided |= (1 << expr_value.result_reg);
					value_generated = true;
				}
				expr_variable = codegen_expr(ofr->vnode, lineno, true,
					(!is_write && !value_generated &&
						value_hint != nullptr && value_hint->func_call_exists) ||
						offset_funcall_exists,
					-1, regs_available2, stack_extra_offset, status);
				result.insert(result.end(), expr_variable.insts.begin(), expr_variable.insts.end());
				regs_available2 = regs_available2 & ~(1 << expr_variable.result_reg);
				regs_decided |= (1 << expr_variable.result_reg);
				variable_generated = true;
				if (is_write && !value_generated && cmp_expr_info(value_hint, offset_hint) < 0) {
					expr_value = codegen_expr(value_node, lineno, true, offset_funcall_exists, -1,
						regs_available2, stack_extra_offset, status);
					result.insert(result.end(), expr_value.insts.begin(), expr_value.insts.end());
					regs_available2 = regs_available2 & ~(1 << expr_value.result_reg);
					regs_decided |= (1 << expr_value.result_reg);
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
				if (expr->type->size > 1 || ofr->negate_offset_node) {
					int size_shift = get_two_pow_num(expr->type->size);
					// オフセットが書き込み禁止、または掛け算をするので、別のレジスタを使う
					if (!((regs_available2 >> offset_reg) & 1) || size_shift < 0) {
						// どうせ読んだ値を書いて壊すレジスタが決まっているなら、それを使う
						// ただし、掛け算をする場合は、必ず別のレジスタを使う
						offset_reg = result_prefer_reg >= 0 && !preserve_cache &&
							!((regs_decided >> result_prefer_reg) & 1) &&
							(size_shift >= 0 || result_prefer_reg != offset_reg) ? result_prefer_reg :
								get_reg_to_use(lineno, regs_available2, offset_prefer_callee_save);
					}
					if (expr->type->size > 1) {
						if (size_shift >= 0) {
							result.push_back(asm_inst(SHL_REG_LIT,
								offset_reg, expr_offset.result_reg, size_shift));
						} else {
							std::vector<asm_inst> ncode = codegen_put_number(offset_reg, expr->type->size);
							result.insert(result.end(), ncode.begin(), ncode.end());
							result.push_back(asm_inst(MUL_REG, offset_reg, expr_offset.result_reg));
						}
						if (ofr->negate_offset_node) {
							result.push_back(asm_inst(NEG_REG, offset_reg, offset_reg));
						}
					} else { // 上位のifより、ofr->negate_offset_nodeがtrue
						result.push_back(asm_inst(NEG_REG, offset_reg, expr_offset.result_reg));
					}
					status.registers_written |= 1 << offset_reg;
				}
				regs_decided |= (1 << offset_reg);
			} else {
				// オフセットを適当なレジスタに置く
				// どうせ読んだ値を書いて壊すレジスタが決まっているなら、それを使う
				offset_reg = result_prefer_reg >= 0 && !preserve_cache &&
					!((regs_decided >> result_prefer_reg) & 1) ? result_prefer_reg :
						get_reg_to_use(lineno, regs_available2, offset_prefer_callee_save);
				std::vector<asm_inst> ncode = codegen_put_number(offset_reg, ofr->additional_offset);
				result.insert(result.end(), ncode.begin(), ncode.end());
				status.registers_written |= 1 << offset_reg;
				regs_available2 = regs_available2 & ~(1 << offset_reg);
				regs_decided |= (1 << offset_reg);
			}
			// TODO: レジスタ数に余裕が無い時はADD_REG命令を使ってまとめる
			if (!variable_generated && (!is_write || cmp_expr_info(variable_hint, value_hint) <= 0)) {
				expr_variable = codegen_expr(ofr->vnode, lineno, true,
					is_write && !value_generated &&
						value_hint != nullptr && value_hint->func_call_exists,
					-1, regs_available, stack_extra_offset, status);
				result.insert(result.end(), expr_variable.insts.begin(), expr_variable.insts.end());
				regs_available2 = regs_available2 & ~(1 << expr_variable.result_reg);
				regs_decided |= (1 << expr_variable.result_reg);
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
					regs_decided |= (1 << expr_value.result_reg);
					value_generated = true;
				}
				if (!variable_generated) {
					expr_variable = codegen_expr(ofr->vnode, lineno, true, false, -1,
						regs_available, stack_extra_offset, status);
					result.insert(result.end(), expr_variable.insts.begin(), expr_variable.insts.end());
					regs_available2 = regs_available2 & ~(1 << expr_variable.result_reg);
					regs_decided |= (1 << expr_variable.result_reg);
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
						status.registers_written |= 1 << result_reg;
					}
				} else {
					// ローカル変数 : スタックポインタからのオフセット
					const int stack_reg = 13;
					result_reg = result_prefer_reg >= 0 ? result_prefer_reg :
						get_reg_to_use(lineno, regs_available, prefer_callee_save);
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
					status.registers_written |= 1 << result_reg;
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
				auto checkpoint = status.save_checkpoint();
				codegen_mem_result res = codegen_mem(expr->info.op.operands[0], ofr, lineno,
					nullptr, false, true, prefer_callee_save,
					result_prefer_reg, regs_available, stack_extra_offset, status);
				if (res.cache.is_register && result_prefer_reg >= 0) {
					// レジスタ変数だった場合、書き込み先レジスタの指定を解除して生成し直す
					// このことにより、無駄なデータのコピーを避けられる
					status.load_checkpoint(checkpoint);
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
								regs_available & ~res.cache.regs_in_cache & ~(1 << result_reg),
								prefer_callee_save);
						result.push_back(asm_inst(MOV_REG, result_reg, res.code.result_reg));
						status.registers_written |= 1 << result_reg;
						// 結果が格納されていたレジスタを更新し、書き込む
						if (add_size <= 255 * 2) {
							asm_inst_kind inst = expr->info.op.kind == OP_POST_INC ? ADD_LIT : SUB_LIT;
							if (add_size < 256) {
								result.push_back(asm_inst(inst, res.code.result_reg, add_size));
							} else {
								result.push_back(asm_inst(inst, res.code.result_reg, 255));
								result.push_back(asm_inst(inst, res.code.result_reg, add_size - 255));
							}
							status.registers_written |= 1 << res.code.result_reg;
						} else {
							int num_reg = get_reg_to_use(lineno,
								regs_available & ~res.cache.regs_in_cache &
								~(1 << res.code.result_reg) & ~(1 << result_reg), false);
							std::vector<asm_inst> ncode = codegen_put_number(num_reg, add_size);
							status.registers_written |= 1 << num_reg;
							result.insert(result.end(), ncode.begin(), ncode.end());
							result.push_back(asm_inst(
								expr->info.op.kind == OP_POST_INC ? ADD_REG_REG : SUB_REG_REG,
								res.code.result_reg, res.code.result_reg, num_reg));
							status.registers_written |= 1 << res.code.result_reg;
						}
						codegen_expr_result wres = codegen_mem_from_cache(res.cache, lineno,
							res.code.result_reg, true, false, regs_available & ~(1 << result_reg), status);
						result.insert(result.end(), wres.insts.begin(), wres.insts.end());
					} else {
						// メモリ変数
						// 作業用に別のレジスタを用意する
						int work_reg = get_reg_to_use(lineno,
							regs_available & ~res.cache.regs_in_cache & ~(1 << result_reg), false);
						// そのレジスタに更新後の値を書き込む
						if (add_size < 8) {
							result.push_back(asm_inst(
								expr->info.op.kind == OP_POST_INC ? ADD_REG_LIT : SUB_REG_LIT,
								work_reg, result_reg, add_size));
						} else if (add_size <= 255 * 2) {
							asm_inst_kind inst = expr->info.op.kind == OP_POST_INC ? ADD_LIT : SUB_LIT;
							result.push_back(asm_inst(MOV_REG, work_reg, result_reg));
							if (add_size < 256) {
								result.push_back(asm_inst(inst, work_reg, add_size));
							} else {
								result.push_back(asm_inst(inst, work_reg, 255));
								result.push_back(asm_inst(inst, work_reg, add_size - 255));
							}
						} else {
							std::vector<asm_inst> ncode = codegen_put_number(work_reg, add_size);
							result.insert(result.end(), ncode.begin(), ncode.end());
							result.push_back(asm_inst(
								expr->info.op.kind == OP_POST_INC ? ADD_REG_REG : SUB_REG_REG,
								work_reg, result_reg, work_reg));
						}
						status.registers_written |= 1 << work_reg;
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
					status.registers_written |= 1 << res.code.result_reg;
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
				auto checkpoint = status.save_checkpoint();
				codegen_mem_result res = codegen_mem(expr->info.op.operands[0], ofr, lineno,
					nullptr, false, true, prefer_callee_save,
					result_prefer_reg, regs_available, stack_extra_offset, status);
				if (res.cache.is_register && result_prefer_reg >= 0) {
					// レジスタ変数だった場合、書き込み先レジスタの指定を解除して生成し直す
					// このことにより、無駄なデータのコピーを避けられる
					status.load_checkpoint(checkpoint);
					res = codegen_mem(expr->info.op.operands[0], ofr, lineno,
						nullptr, false, true, prefer_callee_save,
						-1, regs_available, stack_extra_offset, status);
				}
				result = res.code.insts;
				result_reg = res.code.result_reg;
				// 値を更新する
				if (add_size <= 255 * 2) {
					asm_inst_kind inst = expr->info.op.kind == OP_PRE_INC ? ADD_LIT : SUB_LIT;
					if (add_size < 256) {
						result.push_back(asm_inst(inst, result_reg, add_size));
					} else {
						result.push_back(asm_inst(inst, result_reg, 255));
						result.push_back(asm_inst(inst, result_reg, add_size - 255));
					}
				} else {
					int num_reg = get_reg_to_use(lineno,
						regs_available & ~res.cache.regs_in_cache & ~(1 << result_reg), false);
					std::vector<asm_inst> ncode = codegen_put_number(num_reg, add_size);
					result.insert(result.end(), ncode.begin(), ncode.end());
					status.registers_written |= 1 << num_reg;
					result.push_back(asm_inst(
						expr->info.op.kind == OP_PRE_INC ? ADD_REG_REG : SUB_REG_REG,
						result_reg, result_reg, num_reg));
				}
				status.registers_written |= 1 << result_reg;
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
						status.registers_written |= 1 << result_reg;
					}
				}
				codegen_expr_result wres = codegen_mem_from_cache(res.cache, lineno,
					result_reg, true, read_again,
					read_again ? regs_available & ~res.cache.regs_in_cache : (regs_available & ~(1 << result_reg)), status);
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
		// 論理否定
		case OP_LNOT:
			if (want_result) {
				// TOOD: レジスタに余裕が無い時は、分岐の後のみで値を設定する
				expression_node* operand = expr->info.op.operands[0];
				std::string label = get_label(status.next_label++);
				result_reg = result_prefer_reg >= 0 && ((regs_available >> result_prefer_reg) & 1) ?
					result_prefer_reg : get_reg_to_use(lineno, regs_available,
						operand->hint != nullptr && operand->hint->func_call_exists);
				result.push_back(asm_inst(MOV_LIT, result_reg, 1));
				std::vector<asm_inst> bcode = codegen_conditional_jump(expr, lineno, label, true,
					regs_available & ~(1 << result_reg), stack_extra_offset, status);
				result.insert(result.end(), bcode.begin(), bcode.end());
				result.push_back(asm_inst(MOV_LIT, result_reg, 0));
				result.push_back(asm_inst(LABEL, label));
			} else {
				codegen_expr_result res = codegen_expr(
					expr->info.op.operands[0], lineno, false, false,
					-1, regs_available, stack_extra_offset, status);
				result = res.insts;
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
		// 足し算
		case OP_ARRAY_REF: case OP_ADD:
			{
				// 評価順を決める
				expression_node *operand0, *operand1;
				if (expr->info.op.operands[1]->kind == EXPR_INTEGER_LITERAL ||
				(expr->info.op.operands[0]->kind != EXPR_INTEGER_LITERAL &&
				cmp_expr_info(expr->info.op.operands[0]->hint, expr->info.op.operands[1]->hint) <= 0)) {
					operand0 = expr->info.op.operands[0];
					operand1 = expr->info.op.operands[1];
				} else {
					operand0 = expr->info.op.operands[1];
					operand1 = expr->info.op.operands[0];
				}
				// ポインタ用の係数を決める
				int mult0 = is_pointer_type(operand1->type) &&
					operand1->type->info.target_type != nullptr ?
					operand1->type->info.target_type->size : 1;
				int mult1 = is_pointer_type(operand0->type) &&
					operand0->type->info.target_type != nullptr ?
					operand0->type->info.target_type->size : 1;
				codegen_expr_result result0, result1;
				if (operand1->kind == EXPR_INTEGER_LITERAL) {
					uint32_t add_value = operand1->info.value * mult1;
					uint32_t add_value_neg = -add_value;
					result0 = codegen_expr(operand0, lineno, want_result, false,
						add_value < 8 || add_value_neg < 8 ||
						(add_value > 255 * 2 && add_value_neg > 255 * 2) ? -1 : result_prefer_reg,
						add_value <= 255 * 2 || add_value_neg <= 255 * 2 ? regs_available : -1,
						stack_extra_offset, status);
					result.insert(result.end(), result0.insts.begin(), result0.insts.end());
					if (want_result) {
						if (add_value == 0) {
							result_reg = result0.result_reg;
						} else if ((result_prefer_reg >= 0 && result0.result_reg == result_prefer_reg) ||
						(result_prefer_reg < 0 && ((regs_available >> result0.result_reg) & 1))) {
							// 帰ってきた結果に直接書き込むべき状況
							result_reg = result0.result_reg;
							if (add_value < 256) {
								result.push_back(asm_inst(ADD_LIT, result_reg, add_value));
							} else if (add_value_neg < 256) {
								result.push_back(asm_inst(SUB_LIT, result_reg, add_value_neg));
							} else if (add_value <= 255 * 2) {
								result.push_back(asm_inst(ADD_LIT, result_reg, 255));
								result.push_back(asm_inst(ADD_LIT, result_reg, add_value - 255));
							} else if (add_value_neg <= 255 * 2) {
								result.push_back(asm_inst(SUB_LIT, result_reg, 255));
								result.push_back(asm_inst(SUB_LIT, result_reg, add_value_neg - 255));
							} else {
								int num_reg = get_reg_to_use(lineno, regs_available & ~(1 << result_reg), false);
								std::vector<asm_inst> ncode = codegen_put_number(num_reg, add_value);
								std::vector<asm_inst> ncode_neg = codegen_put_number(num_reg, add_value_neg);
								if (ncode.size() <= ncode_neg.size()) {
									result.insert(result.end(), ncode.begin(), ncode.end());
									result.push_back(asm_inst(ADD_REG_REG, result_reg, result_reg, num_reg));
								} else {
									result.insert(result.end(), ncode_neg.begin(), ncode_neg.end());
									result.push_back(asm_inst(SUB_REG_REG, result_reg, result_reg, num_reg));
								}
							}
							status.registers_written |= 1 << result_reg;
						} else {
							// 帰ってきた結果に直接書き込めない状況
							result_reg = result_prefer_reg >= 0 ? result_prefer_reg :
								get_reg_to_use(lineno, regs_available & ~(1 << result0.result_reg),
									prefer_callee_save);
							if (add_value < 8) {
								result.push_back(asm_inst(ADD_REG_LIT, result_reg, result0.result_reg, add_value));
							} else if (add_value_neg < 8) {
								result.push_back(asm_inst(SUB_REG_LIT, result_reg, result0.result_reg, add_value_neg));
							} else {
								std::vector<asm_inst> ncode = codegen_put_number(result_reg, add_value);
								std::vector<asm_inst> ncode_neg = codegen_put_number(result_reg, add_value_neg);
								if (ncode.size() <= ncode_neg.size()) {
									result.insert(result.end(), ncode.begin(), ncode.end());
									result.push_back(asm_inst(ADD_REG_REG, result_reg, result0.result_reg, result_reg));
								} else {
									result.insert(result.end(), ncode_neg.begin(), ncode_neg.end());
									result.push_back(asm_inst(SUB_REG_REG, result_reg, result0.result_reg, result_reg));
								}
							}
							status.registers_written |= 1 << result_reg;
						}
					}
				} else {
					// 上で入れ替えた可能性があるが
					// ここでは先に評価する辺を「左辺」、後に評価する辺を「右辺」と呼ぶ
					auto checkpoint0 = status.save_checkpoint();
					result0 = codegen_expr(operand0, lineno, want_result,
						operand1->hint != nullptr && operand1->hint->func_call_exists,
						-1, regs_available, stack_extra_offset, status);
					auto checkpoint1 = status.save_checkpoint();
					result1 = codegen_expr(operand1, lineno, want_result, false,
						-1, regs_available & ~(1 << result0.result_reg), stack_extra_offset, status);
					// result_prefer_regが設定されていて、どっちの辺もそこに置かれなかった
					if (want_result && result_prefer_reg >= 0 &&
					result0.result_reg != result_prefer_reg && result1.result_reg != result_prefer_reg) {
						// 左辺が書き換え対象、かつ書き換え不可のレジスタにある
						if (mult0 > 1 && !((regs_available >> result0.result_reg) & 1)) {
							// 左辺にresult_prefer_regを設定して生成し直す
							status.load_checkpoint(checkpoint0);
							result0 = codegen_expr(operand0, lineno, want_result,
								operand1->hint != nullptr && operand1->hint->func_call_exists,
								result_prefer_reg, regs_available, stack_extra_offset, status);
							result1 = codegen_expr(operand1, lineno, want_result, false,
								-1, regs_available & ~(1 << result0.result_reg), stack_extra_offset, status);
						} else if (mult1 > 1 && !((regs_available >> result1.result_reg) & 1)) {
							// 右辺が書き換え対象、かつ書き換え不可のレジスタにある
							// → 右辺にresult_prefer_regを設定して生成し直す
							status.load_checkpoint(checkpoint1);
							result1 = codegen_expr(operand1, lineno, want_result, false,
								result_prefer_reg, regs_available & ~(1 << result0.result_reg),
								stack_extra_offset, status);
						}
					}
					// ポインタの計算用の係数を反映させる
					int reg0 = result0.result_reg, reg1 = result1.result_reg;
					result.insert(result.end(), result0.insts.begin(), result0.insts.end());
					if (want_result && mult0 > 1) {
						int tpn = get_two_pow_num(mult0);
						if (tpn >= 0 &&
						((result_prefer_reg >= 0 && reg0 == result_prefer_reg && reg1 != result_prefer_reg) ||
						((regs_available >> reg0) & 1))) {
							// reg0に上書きできるので、そのまま
						} else {
							// reg0に上書きできないので、新しいレジスタを割り当てる
							reg0 = get_reg_to_use(lineno, regs_available & ~(1 << reg1),
								(operand1->hint != nullptr && operand1->hint->func_call_exists) ||
								(prefer_callee_save && reg1 != result_prefer_reg));
						}
						if (tpn > 0) {
							result.push_back(asm_inst(SHL_REG_LIT, reg0, result0.result_reg, tpn));
						} else {
							std::vector<asm_inst> ncode = codegen_put_number(reg0, mult0);
							result.insert(result.end(), ncode.begin(), ncode.end());
							result.push_back(asm_inst(MUL_REG, reg0, result_reg));
						}
						status.registers_written |= 1 << reg0;
					}
					result.insert(result.end(), result1.insts.begin(), result1.insts.end());
					if (want_result && mult1 > 1) {
						int tpn = get_two_pow_num(mult0);
						if (tpn >= 0 &&
						((result_prefer_reg >= 0 && reg1 == result_prefer_reg && reg0 != result_prefer_reg) ||
						((regs_available >> reg1) & 1))) {
							// reg1に上書きできるので、そのまま
						} else {
							// reg1に上書きできないので、新しいレジスタを割り当てる
							reg1 = get_reg_to_use(lineno, regs_available & ~(1 << reg0) & ~(1 << reg1), false);
						}
						if (tpn > 0) {
							result.push_back(asm_inst(SHL_REG_LIT, reg1, result1.result_reg, tpn));
						} else {
							std::vector<asm_inst> ncode = codegen_put_number(reg1, mult1);
							result.insert(result.end(), ncode.begin(), ncode.end());
							result.push_back(asm_inst(MUL_REG, reg1, result_reg));
						}
						status.registers_written |= 1 << reg1;
					}
					// 足し算を行う
					if (want_result) {
						if (result_prefer_reg >= 0) {
							// result_prefer_regが設定されている → result_prefer_regに出力する
							result_reg = result_prefer_reg;
						} else if ((regs_available >> reg0) & 1) {
							// reg0が書き込み可能 → reg0に出力する
							result_reg = reg0;
						} else if ((regs_available >> reg1) & 1) {
							// reg1が書き込み可能 → reg1に出力する
							result_reg = reg1;
						} else {
							// 第三のレジスタに出力する
							result_reg = get_reg_to_use(lineno, regs_available,
								prefer_callee_save);
						}
						result.push_back(asm_inst(ADD_REG_REG, result_reg, reg0, reg1));
						status.registers_written |= 1 << result_reg;
					}
				}
			}
			break;
		// 引き算
		case OP_SUB:
			{
				expression_node* operand0 = expr->info.op.operands[0];
				expression_node* operand1 = expr->info.op.operands[1];
				// ポインタ用の係数
				int mult = is_pointer_type(operand0->type) &&
					operand0->type->info.target_type != nullptr ?
					operand0->type->info.target_type->size : 1;
				codegen_expr_result result0, result1;
				if (operand1->kind == EXPR_INTEGER_LITERAL) {
					uint32_t sub_value = operand1->info.value * mult;
					uint32_t sub_value_neg = -sub_value;
					result0 = codegen_expr(operand0, lineno, want_result, prefer_callee_save,
						sub_value < 8 || sub_value_neg < 8 ||
						(sub_value > 255 * 2 && sub_value_neg > 255 * 2) ? -1 : result_prefer_reg,
						regs_available, stack_extra_offset, status);
					result.insert(result.end(), result0.insts.begin(), result0.insts.end());
					if (want_result) {
						result_reg = result0.result_reg;
						if (sub_value > 0) {
							if (result_reg != result_prefer_reg && !((regs_available >> result_reg) & 1)) {
								// 結果が書き込み禁止なので、別のレジスタを割り当てる
								result_reg = get_reg_to_use(lineno, regs_available, prefer_callee_save);
								if (sub_value < 8) {
									result.push_back(asm_inst(SUB_REG_LIT, result_reg, result0.result_reg, sub_value));
								} else if (sub_value_neg < 8) {
									result.push_back(asm_inst(ADD_REG_LIT, result_reg, result0.result_reg, sub_value_neg));
								} else if (sub_value <= 255 * 2 || sub_value_neg <= 255 * 2) {
									result.push_back(asm_inst(MOV_REG, result_reg, result0.result_reg));
									if (sub_value < 256) {
										result.push_back(asm_inst(SUB_LIT, result_reg, sub_value));
									} else if (sub_value_neg < 256) {
										result.push_back(asm_inst(ADD_LIT, result_reg, sub_value_neg));
									} else if (sub_value <= 255 * 2) {
										result.push_back(asm_inst(SUB_LIT, result_reg, 255));
										result.push_back(asm_inst(SUB_LIT, result_reg, sub_value - 255));
									} else {
										result.push_back(asm_inst(ADD_LIT, result_reg, 255));
										result.push_back(asm_inst(ADD_LIT, result_reg, sub_value_neg - 255));
									}
								} else {
									std::vector<asm_inst> ncode = codegen_put_number(result_reg, sub_value);
									std::vector<asm_inst> ncode_neg = codegen_put_number(result_reg, sub_value_neg);
									if (ncode.size() <= ncode_neg.size()) {
										result.insert(result.end(), ncode.begin(), ncode.end());
										result.push_back(asm_inst(SUB_REG_REG, result_reg, result0.result_reg, result_reg));
									} else {
										result.insert(result.end(), ncode_neg.begin(), ncode_neg.end());
										result.push_back(asm_inst(ADD_REG_REG, result_reg, result0.result_reg, result_reg));
									}
								}
							} else {
								// 結果を直接書き換える
								if (sub_value < 256) {
									result.push_back(asm_inst(SUB_LIT, result_reg, sub_value));
								} else if (sub_value_neg < 256) {
									result.push_back(asm_inst(ADD_LIT, result_reg, sub_value_neg));
								} else if (sub_value <= 255 * 2) {
									result.push_back(asm_inst(SUB_LIT, result_reg, 255));
									result.push_back(asm_inst(SUB_LIT, result_reg, sub_value - 255));
								} else if (sub_value_neg <= 255 * 2) {
									result.push_back(asm_inst(ADD_LIT, result_reg, 255));
									result.push_back(asm_inst(ADD_LIT, result_reg, sub_value_neg - 255));
								} else {
									int work_reg = get_reg_to_use(lineno, regs_available & ~(1 << result_reg), prefer_callee_save);
									std::vector<asm_inst> ncode = codegen_put_number(work_reg, sub_value);
									std::vector<asm_inst> ncode_neg = codegen_put_number(work_reg, sub_value_neg);
									status.registers_written |= 1 << work_reg;
									if (ncode.size() <= ncode_neg.size()) {
										result.insert(result.end(), ncode.begin(), ncode.end());
										result.push_back(asm_inst(SUB_REG_REG, result_reg, result_reg, work_reg));
									} else {
										result.insert(result.end(), ncode_neg.begin(), ncode_neg.end());
										result.push_back(asm_inst(ADD_REG_REG, result_reg, result_reg, work_reg));
									}
								}
							}
							status.registers_written |= 1 << result_reg;
						}
					}
				} else {
					bool zero_first = (cmp_expr_info(operand0->hint, operand1->hint) <= 0);
					// オペランドの値を得る
					if (zero_first) {
						result0 = codegen_expr(operand0, lineno, want_result,
							operand1->hint != nullptr && operand1->hint->func_call_exists,
							-1, regs_available, stack_extra_offset, status);
						auto checkpoint = status.save_checkpoint();
						result1 = codegen_expr(operand1, lineno, want_result, false, -1,
							regs_available & ~(1 << result0.result_reg), stack_extra_offset, status);
						// result_prefer_reg設定あり && 結果が乗っていない &&
						// 書き換え対象が書き換え不可 → 書き換え対象にresult_prefer_regを設定
						if (result_prefer_reg >= 0 &&
						result0.result_reg != result_prefer_reg && result1.result_reg != result_prefer_reg &&
						mult > 1 && !((regs_available >> result1.result_reg) & 1)) {
							status.load_checkpoint(checkpoint);
							result1 = codegen_expr(operand1, lineno, want_result, false, result_prefer_reg,
								regs_available & ~(1 << result0.result_reg), stack_extra_offset, status);
						}
						result.insert(result.end(), result0.insts.begin(), result0.insts.end());
						result.insert(result.end(), result1.insts.begin(), result1.insts.end());
					} else {
						auto checkpoint = status.save_checkpoint();
						result1 = codegen_expr(operand1, lineno, want_result,
							operand0->hint != nullptr && operand0->hint->func_call_exists,
							-1, regs_available, stack_extra_offset, status);
						result0 = codegen_expr(operand0, lineno, want_result, false,
							-1, regs_available & ~(1 << result1.result_reg), stack_extra_offset, status);
						// result_prefer_reg設定あり && 結果が乗っていない &&
						// 書き換え対象が書き換え不可 → 書き換え対象にresult_prefer_regを設定
						if (result_prefer_reg >= 0 &&
						result1.result_reg != result_prefer_reg && result0.result_reg != result_prefer_reg &&
						mult > 1 && !((regs_available >> result1.result_reg) & 1)) {
							status.load_checkpoint(checkpoint);
							result1 = codegen_expr(operand1, lineno, want_result,
								operand0->hint != nullptr && operand0->hint->func_call_exists,
								result_prefer_reg, regs_available, stack_extra_offset, status);
							result0 = codegen_expr(operand0, lineno, want_result, false,
								-1, regs_available & ~(1 << result1.result_reg), stack_extra_offset, status);
						}
						result.insert(result.end(), result1.insts.begin(), result1.insts.end());
						result.insert(result.end(), result0.insts.begin(), result0.insts.end());
					}
					// 係数を反映させる
					int reg0 = result0.result_reg, reg1 = result1.result_reg;
					if (want_result) {
						// 係数を反映させる
						if (!is_pointer_type(operand1->type) && mult > 1) {
							int tpn = get_two_pow_num(mult);
							if (tpn < 0 ||
							(result1.result_reg != result_prefer_reg && !((regs_available >> result1.result_reg) & 1))) {
								// reg1が書き込み禁止または掛け算に使うので、新しいレジスタを割り当てる
								reg1 = get_reg_to_use(lineno, regs_available & ~(1 << reg0) & ~(1 << reg1), false);
							}
							if (tpn >= 0) {
								result.push_back(asm_inst(SHL_REG_LIT, reg1, result1.result_reg, tpn));
							} else {
								std::vector<asm_inst> ncode = codegen_put_number(reg1, mult);
								result.insert(result.end(), ncode.begin(), ncode.end());
								result.push_back(asm_inst(MUL_REG, reg1, result1.result_reg));
							}
							status.registers_written |= 1 << reg1;
						}
						// 引き算をする
						result_reg = result_prefer_reg >= 0 ? result_prefer_reg :
							get_reg_to_use(lineno, regs_available, prefer_callee_save);
						result.push_back(asm_inst(SUB_REG_REG, result_reg, reg0, reg1));
						// ポインタ同士の引き算の場合、要素サイズで割る
						if (is_pointer_type(operand1->type) && mult > 1) {
							int tpn = get_two_pow_num(mult);
							if (tpn >= 0) {
								result.push_back(asm_inst(ASR_REG_LIT, result_reg, result_reg, tpn));
							} else {
								throw codegen_error(lineno, "generic division not implemented yet");
							}
						}
						status.registers_written |= 1 << result_reg;
					}
				}
			}
			break;
		// シフト演算 (対称性なし、即値使用可能、引き算と比べてシンプル)
		case OP_SHL: case OP_SHR:
			{
				expression_node* operand0 = expr->info.op.operands[0];
				expression_node* operand1 = expr->info.op.operands[1];
				codegen_expr_result result0, result1;
				bool integer_mode = (operand1->kind == EXPR_INTEGER_LITERAL);
				if (integer_mode) {
					result0 = codegen_expr(operand0, lineno, want_result, prefer_callee_save,
						result_prefer_reg, regs_available, stack_extra_offset, status);
					result.insert(result.end(), result0.insts.begin(), result0.insts.end());
				} else {
					bool zero_first = (cmp_expr_info(operand0->hint, operand1->hint) <= 0);
					// オペランドの値を得る
					if (zero_first) {
						result0 = codegen_expr(operand0, lineno, want_result,
							prefer_callee_save || (operand1->hint != nullptr && operand1->hint->func_call_exists),
							result_prefer_reg >= 0 && (regs_available & (1 << result_prefer_reg)) ? result_prefer_reg : -1,
							regs_available, stack_extra_offset, status);
						result1 = codegen_expr(operand1, lineno, want_result, false,
							-1, regs_available & ~(1 << result0.result_reg), stack_extra_offset, status);
						result.insert(result.end(), result0.insts.begin(), result0.insts.end());
						result.insert(result.end(), result1.insts.begin(), result1.insts.end());
					} else {
						result1 = codegen_expr(operand1, lineno, want_result,
							operand0->hint != nullptr && operand0->hint->func_call_exists,
							-1, regs_available, stack_extra_offset, status);
						result0 = codegen_expr(operand0, lineno, want_result, prefer_callee_save,
							result_prefer_reg >= 0 && result1.result_reg != result_prefer_reg ? result_prefer_reg : -1,
							regs_available & ~(1 << result1.result_reg), stack_extra_offset, status);
						result.insert(result.end(), result1.insts.begin(), result1.insts.end());
						result.insert(result.end(), result0.insts.begin(), result0.insts.end());
					}
				}
				// シフト演算を行う
				if (want_result) {
					result_reg = result0.result_reg;
					// 左辺の結果が書き込み禁止の場合、別のレジスタを割り当てる
					if (result_reg != result_prefer_reg && !((regs_available >> result_reg) & 1)) {
						result_reg = get_reg_to_use(lineno,
							regs_available & ~(1 << result1.result_reg), prefer_callee_save);
						if (!integer_mode) {
							result.push_back(asm_inst(MOV_REG, result_reg, result0.result_reg));
						}
					}
					// シフト演算の本体
					if (integer_mode) {
						result.push_back(asm_inst(
							expr->info.op.kind == OP_SHL ? SHL_REG_LIT :
							(is_integer_type(expr->type) && expr->type->info.is_signed ? ASR_REG_LIT : SHR_REG_LIT),
							result_reg, result0.result_reg, operand1->info.value & 31));
					} else {
						result.push_back(asm_inst(
							expr->info.op.kind == OP_SHL ? SHL_REG :
							(is_integer_type(expr->type) && expr->type->info.is_signed ? ASR_REG : SHR_REG),
							result_reg, result1.result_reg));
					}
					status.registers_written |= 1 << result_reg;
				}
			}
			break;
		// 各種二項演算子 (対称性あり、即値使用不可)
		case OP_MUL: case OP_AND: case OP_XOR: case OP_OR:
			{
				expression_node* operand0 = expr->info.op.operands[0];
				expression_node* operand1 = expr->info.op.operands[1];
				codegen_expr_result result0, result1;
				bool zero_first = (cmp_expr_info(operand0->hint, operand1->hint) <= 0);
				// オペランドの値を得る
				if (zero_first) {
					result0 = codegen_expr(operand0, lineno, want_result,
						operand1->hint != nullptr && operand1->hint->func_call_exists,
						-1, regs_available, stack_extra_offset, status);
					result1 = codegen_expr(operand1, lineno, want_result, prefer_callee_save,
						result0.result_reg != result_prefer_reg ? result_prefer_reg : -1,
						regs_available & ~(1 << result0.result_reg), stack_extra_offset, status);
					result.insert(result.end(), result0.insts.begin(), result0.insts.end());
					result.insert(result.end(), result1.insts.begin(), result1.insts.end());
				} else {
					result1 = codegen_expr(operand1, lineno, want_result,
						operand0->hint != nullptr && operand0->hint->func_call_exists,
						-1, regs_available, stack_extra_offset, status);
					result0 = codegen_expr(operand0, lineno, want_result, prefer_callee_save,
						result1.result_reg != result_prefer_reg ? result_prefer_reg : -1,
						regs_available & ~(1 << result1.result_reg), stack_extra_offset, status);
					result.insert(result.end(), result1.insts.begin(), result1.insts.end());
					result.insert(result.end(), result0.insts.begin(), result0.insts.end());
				}
				if (want_result) {
					// どっちを結果にするかを決める
					int reg0 = result0.result_reg, reg1 = result1.result_reg;
					bool result_on_zero;
					// result_prefer_regに該当するレジスタがあるなら、それ
					if (reg0 == result_prefer_reg) result_on_zero = true;
					else if (reg1 == result_prefer_reg) result_on_zero = false;
					// result_prefer_regに該当するレジスタが無いなら、書込み可能なレジスタ
					else if (regs_available & (1 << reg0)) result_on_zero = true;
					else if (regs_available & (1 << reg1)) result_on_zero = false;
					// 書込み可能なレジスタも無いので、新しいレジスタを割り当てる
					else {
						// 上のif文より、regs_availableにreg0もreg1も含まれないので、マスクは不要
						reg0 = get_reg_to_use(lineno, regs_available, prefer_callee_save);
						result.push_back(asm_inst(MOV_REG, reg0, result0.result_reg));
						status.registers_written |= 1 << reg0;
						result_on_zero = true;
					}
					// 決めた方に結果を作る
					asm_inst_kind inst;
					switch (expr->info.op.kind) {
					case OP_MUL: inst = MUL_REG; break;
					case OP_AND: inst = AND_REG; break;
					case OP_OR: inst = OR_REG; break;
					case OP_XOR: inst = XOR_REG; break;
					default: throw codegen_error(lineno, "unexpected operator kind kind");
					}
					if (result_on_zero) {
						result.push_back(asm_inst(inst, reg0, reg1));
						status.registers_written |= 1 << reg0;
						result_reg = reg0;
					} else {
						result.push_back(asm_inst(inst, reg1, reg0));
						status.registers_written |= 1 << reg1;
						result_reg = reg1;
					}
				}
			}
			break;
		// 比較演算子・二項論理演算子
		case OP_LESS: case OP_GREATER: case OP_LESS_EQUAL: case OP_GREATER_EQUAL:
		case OP_EQUAL: case OP_NOT_EQUAL:
		case OP_LAND: case OP_LOR:
			if (want_result) {
				// TOOD: レジスタに余裕が無い時は、分岐の後のみで値を設定する
				expression_node* operand0 = expr->info.op.operands[0];
				expression_node* operand1 = expr->info.op.operands[1];
				std::string label = get_label(status.next_label++);
				result_reg = result_prefer_reg >= 0 && ((regs_available >> result_prefer_reg) & 1) ?
					result_prefer_reg : get_reg_to_use(lineno, regs_available,
						(operand0->hint != nullptr && operand0->hint->func_call_exists) ||
						(operand1->hint != nullptr && operand1->hint->func_call_exists));
				result.push_back(asm_inst(MOV_LIT, result_reg, 1));
				std::vector<asm_inst> bcode = codegen_conditional_jump(expr, lineno, label, true,
					regs_available & ~(1 << result_reg), stack_extra_offset, status);
				result.insert(result.end(), bcode.begin(), bcode.end());
				result.push_back(asm_inst(MOV_LIT, result_reg, 0));
				result.push_back(asm_inst(LABEL, label));
				status.registers_written |= 1 << result_reg;
			} else {
				std::string label;
				std::vector<asm_inst> res0;
				bool use_label = false;
				codegen_expr_result res1 = codegen_expr(
					expr->info.op.operands[1], lineno, false, false,
					-1, regs_available, stack_extra_offset, status);
				// 右辺の評価に用いるコードが空の場合は、左辺を常に生成する
				if (!res1.insts.empty() && expr->info.op.kind == OP_LAND) {
					// &&演算子 → 左辺がtrueのときのみ右辺を評価する
					label = get_label(status.next_label++);
					use_label = true;
					res0 = codegen_conditional_jump(expr->info.op.operands[0], lineno, label, false,
						regs_available, stack_extra_offset, status);
				} else if (!res1.insts.empty() && expr->info.op.kind == OP_LOR) {
					// ||演算子 → 左辺がfalseのときのみ右辺を評価する
					label = get_label(status.next_label++);
					use_label = true;
					res0 = codegen_conditional_jump(expr->info.op.operands[0], lineno, label, true,
						regs_available, stack_extra_offset, status);
				} else {
					// 比較演算子 → 常に両辺を評価する
					codegen_expr_result res0_expr = codegen_expr(
						expr->info.op.operands[0], lineno, false, false,
						-1, regs_available, stack_extra_offset, status);
					res0 = res0_expr.insts;
				}
				result.insert(result.end(), res0.begin(), res0.end());
				result.insert(result.end(), res1.insts.begin(), res1.insts.end());
				if (use_label) result.push_back(asm_inst(LABEL, label));
			}
			break;
		// 代入
		case OP_ASSIGN:
			{
				offset_fold_result* ofr = offset_fold(expr->info.op.operands[0]);
				codegen_mem_result res = codegen_mem(expr->info.op.operands[0], ofr, lineno,
					expr->info.op.operands[1], true, false, prefer_callee_save,
					result_prefer_reg, regs_available, stack_extra_offset, status);
				result = res.code.insts;
				result_reg = res.code.result_reg;
				if (!res.cache.is_register && res.cache.size < 4) {
					// 符号拡張 or ゼロ拡張
					int shift_width = 8 * (4 - res.cache.size);
					int out_reg = result_prefer_reg >= 0 ? result_prefer_reg : result_reg;
					result.push_back(asm_inst(SHL_REG_LIT, out_reg, result_reg, shift_width));
					result.push_back(asm_inst(
						res.cache.is_signed ? ASR_REG_LIT : SHR_REG_LIT, out_reg, out_reg, shift_width));
					status.registers_written |= 1 << out_reg;
					result_reg = out_reg;
				}
			}
			break;
		// 複合代入
		case OP_ADD_ASSIGN: case OP_SUB_ASSIGN: // 即値あり、係数あり
		case OP_SHL_ASSIGN: case OP_SHR_ASSIGN: // 即値あり、係数なし
		case OP_MUL_ASSIGN: case OP_AND_ASSIGN: case OP_XOR_ASSIGN: case OP_OR_ASSIGN: // 即値なし、係数なし
			{
				bool is_add = expr->info.op.kind == OP_ADD_ASSIGN || expr->info.op.kind == OP_SUB_ASSIGN;
				bool is_shift = expr->info.op.kind == OP_SHL_ASSIGN || expr->info.op.kind == OP_SHR_ASSIGN;
				expression_node* operand0 = expr->info.op.operands[0];
				expression_node* operand1 = expr->info.op.operands[1];
				codegen_mem_result res0;
				codegen_expr_result res1;
				offset_fold_result* ofr = offset_fold(operand0);
				int mult = is_add && is_pointer_type(operand0->type) && operand0->type->info.target_type != nullptr ?
					operand0->type->info.target_type->size : 1;
				bool right_is_literal = operand1->kind == EXPR_INTEGER_LITERAL;
				uint32_t literal_value = right_is_literal ? mult * operand1->info.value : 0;
				// 値を読み、新しい値を計算する
				if (right_is_literal &&
				((is_add && (literal_value < 256 || UINT32_MAX - (256 - 1) < literal_value)) || is_shift)) {
					// 即値を使用する
					auto checkpoint = status.save_checkpoint();
					res0 = codegen_mem(operand0, ofr, lineno, nullptr, false, true, false,
						result_prefer_reg >= 0 && ((regs_available >> result_prefer_reg) & 1) ? result_prefer_reg : -1,
						regs_available, stack_extra_offset,  status);
					if (res0.cache.is_register) {
						// レジスタ変数なら、result_prefer_regの指定を解除して生成し直す
						status.load_checkpoint(checkpoint);
						res0 = codegen_mem(operand0, ofr, lineno, nullptr, false, true, false,
							-1, regs_available, stack_extra_offset,  status);
					}
					result.insert(result.end(), res0.code.insts.begin(), res0.code.insts.end());
					uint32_t immediate_value;
					asm_inst_kind inst;
					if (is_add) {
						immediate_value = literal_value < 256 ? literal_value : -literal_value;
						if (expr->info.op.kind == OP_ADD_ASSIGN) {
							inst = literal_value < 256 ? ADD_LIT : SUB_LIT;
						} else {
							inst = literal_value < 256 ? SUB_LIT : ADD_LIT;
						}
						result.push_back(asm_inst(inst, res0.code.result_reg, immediate_value));
					} else {
						immediate_value = literal_value & 31;
						if (expr->info.op.kind == OP_SHL_ASSIGN) {
							inst = SHL_REG_LIT;
						} else {
							inst = is_integer_type(operand0->type) && operand0->type->info.is_signed ?
								ASR_REG_LIT : SHR_REG_LIT;
						}
						result.push_back(asm_inst(inst,
							res0.code.result_reg, res0.code.result_reg, immediate_value));
					}
					status.registers_written |= 1 << res0.code.result_reg;
				} else {
					// 即値を使用しない
					if (cmp_expr_info(operand0->hint, operand1->hint) <= 0) {
						auto checkpoint = status.save_checkpoint();
						res0 = codegen_mem(operand0, ofr, lineno, nullptr, false, true,
							operand1->hint != nullptr && operand1->hint->func_call_exists,
							result_prefer_reg >= 0 && ((regs_available >> result_prefer_reg) & 1) ? result_prefer_reg : -1,
							regs_available, stack_extra_offset,  status);
						if (res0.cache.is_register) {
							// レジスタ変数なら、result_prefer_regの指定を解除して生成し直す
							status.load_checkpoint(checkpoint);
							res0 = codegen_mem(operand0, ofr, lineno, nullptr, false, true,
								operand1->hint != nullptr && operand1->hint->func_call_exists,
								-1, regs_available, stack_extra_offset,  status);
						}
						res1 = codegen_expr(operand1, lineno, true, false, -1,
							regs_available & ~res0.cache.regs_in_cache & ~(1 << res0.code.result_reg),
							stack_extra_offset, status);
						result.insert(result.end(), res0.code.insts.begin(), res0.code.insts.end());
						result.insert(result.end(), res1.insts.begin(), res1.insts.end());
					} else {
						res1 = codegen_expr(operand1, lineno, true,
							operand0->hint != nullptr && operand0->hint->func_call_exists,
							-1, regs_available, stack_extra_offset, status);
						auto checkpoint = status.save_checkpoint();
						res0 = codegen_mem(operand0, ofr, lineno, nullptr, false, true, false,
							result_prefer_reg, regs_available & ~(1 << res1.result_reg), stack_extra_offset, status);
						if (res0.cache.is_register) {
							// レジスタ変数なら、result_prefer_regの指定を解除して生成し直す
							status.load_checkpoint(checkpoint);
							res0 = codegen_mem(operand0, ofr, lineno, nullptr, false, true, false,
								-1, regs_available & ~(1 << res1.result_reg), stack_extra_offset, status);
						}
						result.insert(result.end(), res1.insts.begin(), res1.insts.end());
						result.insert(result.end(), res0.code.insts.begin(), res0.code.insts.end());
					}
					if (is_add) {
						result.push_back(asm_inst(
							expr->info.op.kind == OP_ADD_ASSIGN ? ADD_REG_REG : SUB_REG_REG,
							res0.code.result_reg, res0.code.result_reg, res1.result_reg));
					} else {
						asm_inst_kind inst;
						switch (expr->info.op.kind) {
						case OP_SHL_ASSIGN: inst = SHL_REG; break;
						case OP_SHR_ASSIGN:
							inst = is_integer_type(operand0->type) && operand0->type->info.is_signed ?
								ASR_REG : SHR_REG;
							break;
						case OP_MUL_ASSIGN: inst = MUL_REG; break;
						case OP_AND_ASSIGN: inst = AND_REG; break;
						case OP_XOR_ASSIGN: inst = XOR_REG; break;
						case OP_OR_ASSIGN: inst = OR_REG; break;
						default: throw codegen_error(lineno, "unexpected operator kind");
						}
						result.push_back(asm_inst(inst, res0.code.result_reg, res1.result_reg));
					}
					status.registers_written |= 1 << res0.code.result_reg;
				}
				// 計算した値を書き込む
				result_reg = res0.code.result_reg;
				bool do_extend = want_result && expr->type != nullptr && expr->type->size < 4;
				bool read_again = false;
				if (do_extend) {
					if (res0.cache.is_register) {
						read_again = true;
					} else {
						// 符号拡張 or ゼロ拡張
						int shift_width = 8 * (4 - expr->type->size);
						result.push_back(asm_inst(SHL_REG_LIT, result_reg, result_reg, shift_width));
						result.push_back(asm_inst(
							expr->type->kind == TYPE_INTEGER && expr->type->info.is_signed ? ASR_REG_LIT : SHR_REG_LIT,
							result_reg, result_reg, shift_width));
						status.registers_written |= 1 << result_reg;
					}
				}
				codegen_expr_result wres = codegen_mem_from_cache(res0.cache, lineno,
					result_reg, true, read_again,
					read_again ? regs_available & ~res0.cache.regs_in_cache : (regs_available & ~(1 << result_reg)), status);
				result.insert(result.end(), wres.insts.begin(), wres.insts.end());
				// レジスタ変数の場合、変数から値を読み込む
				// 結果格納用と同じレジスタになっているはずだが、念の為
				if (do_extend && res0.cache.is_register) {
					codegen_expr_result rres = codegen_mem_from_cache(res0.cache, lineno,
						result_reg, false, false, regs_available, status);
					result.insert(result.end(), rres.insts.begin(), rres.insts.end());
					result_reg = rres.result_reg;
				}
			}
			break;
		// コンマ演算子
		case OP_COMMA:
			{
				codegen_expr_result res = codegen_expr(
					expr->info.op.operands[0], lineno, false, false,
					-1, regs_available, stack_extra_offset, status);
				result = res.insts;
				res = codegen_expr(
					expr->info.op.operands[1], lineno, true, prefer_callee_save,
					result_prefer_reg, regs_available, stack_extra_offset, status);
				result.insert(result.end(), res.insts.begin(), res.insts.end());
				result_reg = res.result_reg;
			}
			break;
		// 条件演算子
		case OP_COND:
			{
				std::string false_start_label = get_label(status.next_label++);
				std::string true_end_label = get_label(status.next_label++);
				codegen_expr_result res_true, res_false;
				auto checkpoint = status.save_checkpoint();
				res_true = codegen_expr(expr->info.op.operands[1], lineno, want_result, prefer_callee_save,
					result_prefer_reg, regs_available, stack_extra_offset, status);
				auto checkpoint2 = status.save_checkpoint();
				res_false = codegen_expr(expr->info.op.operands[2], lineno, want_result, prefer_callee_save,
					result_prefer_reg, regs_available, stack_extra_offset, status);
				// trueのときとfalseのときの結果レジスタを合わせる
				if (want_result && res_true.result_reg != res_false.result_reg) {
					if (res_true.result_reg == result_prefer_reg || ((regs_available >> res_true.result_reg) & 1)) {
						// res_trueの結果が書き込み可能レジスタ → res_falseを再生成
						status.load_checkpoint(checkpoint2);
						res_false = codegen_expr(expr->info.op.operands[2], lineno, want_result, prefer_callee_save,
							res_true.result_reg, regs_available, stack_extra_offset, status);
					} else if (res_false.result_reg == result_prefer_reg || ((regs_available >> res_false.result_reg) & 1)) {
						// res_falseの結果が書き込み可能レジスタ → res_trueを再生成
						// 再生成すると結果が変わる可能性があるので、res_falseは再生成しない
						res_true = codegen_expr(expr->info.op.operands[1], lineno, want_result, prefer_callee_save,
							res_false.result_reg, regs_available, stack_extra_offset, status);
					} else {
						// 新しいレジスタに結果を置かせる
						int out_reg = get_reg_to_use(lineno, regs_available, prefer_callee_save);
						status.load_checkpoint(checkpoint);
						res_true = codegen_expr(expr->info.op.operands[1], lineno, want_result, prefer_callee_save,
							out_reg, regs_available, stack_extra_offset, status);
						res_false = codegen_expr(expr->info.op.operands[2], lineno, want_result, prefer_callee_save,
							out_reg, regs_available, stack_extra_offset, status);
					}
					if (res_true.result_reg != res_false.result_reg) {
						throw codegen_error(lineno, "conditional operator result register mismatch");
					}
				}
				if (want_result) result_reg = res_true.result_reg;
				std::vector<asm_inst> cond_code;
				if (!res_true.insts.empty() || !res_false.insts.empty()) {
					cond_code = codegen_conditional_jump(
						expr->info.op.operands[0], lineno, false_start_label, false,
						regs_available, stack_extra_offset, status);
				} else {
					// trueでもfalseでもコードが無いなら、空で評価を行う
					codegen_expr_result cond_result = codegen_expr(expr->info.op.operands[0], lineno,
						false, false, -1, regs_available, stack_extra_offset, status);
					cond_code = cond_result.insts;
				}
				result.insert(result.end(), cond_code.begin(), cond_code.end());
				result.insert(result.end(), res_true.insts.begin(), res_true.insts.end());
				result.push_back(asm_inst(JMP_DIRECT, true_end_label));
				result.push_back(asm_inst(LABEL, false_start_label));
				result.insert(result.end(), res_false.insts.begin(), res_false.insts.end());
				result.push_back(asm_inst(LABEL, true_end_label));
			}
			break;
		// 関数呼び出し
		case OP_FUNC_CALL_NOARGS: case OP_FUNC_CALL:
			{
				if (expr->info.op.argument_num > 4) {
					throw codegen_error(lineno, "unsupported function call (too many arguments)");
				}
				int argument_regs = (1 << expr->info.op.argument_num) - 1;
				if (argument_regs & status.registers_reserved) {
					throw codegen_error(lineno, "unsupported function call (argument registers are reserved)");
				}
				// 評価順を計算する
				std::vector<expression_node*> operands;
				std::vector<int> operands_order;
				operands.push_back(expr->info.op.operands[0]);
				operands_order.push_back(0);
				for (int i = 0; i < expr->info.op.argument_num; i++) {
					operands.push_back(expr->info.op.arguments[i]);
					operands_order.push_back(i + 1);
				}
				for (size_t i = operands_order.size() - 1; i > 0; i--) {
					for (size_t j = 0; j < i; j++) {
						if (cmp_expr_info(operands[operands_order[j]]->hint, operands[operands_order[j + 1]]->hint) > 0) {
							int temp = operands_order[j];
							operands_order[j] = operands_order[j + 1];
							operands_order[j + 1] = temp;
						}
					}
				}
				// 呼び出し対象の関数を求める
				offset_fold_result* ofr = offset_fold(expr->info.op.operands[0]);
				bool direct_call = false;
				std::string direct_call_label;
				if (ofr != nullptr && ofr->vinfo != nullptr &&  ofr->additional_offset == 0 &&
				ofr->offset_node == nullptr && ofr->vnode != nullptr && ofr->vnode->kind == EXPR_IDENTIFIER) {
					direct_call = true;
					direct_call_label = ofr->vnode->info.ident.name;
				}
				// オペランドの評価前に、引数として使うレジスタを保存する
				int regs_to_save = argument_regs & ~regs_available;
				if (result_prefer_reg >= 0) regs_to_save &= ~(1 << result_prefer_reg);
				if (regs_to_save != 0) result.push_back(asm_inst(PUSH_REGS, regs_to_save));
				int new_offset = stack_extra_offset;
				for (int regs = regs_to_save; regs > 0; regs >>= 1) {
					if (regs & 1) new_offset += 4;
				}
				// 評価を実行する
				int regs_available2 = regs_available;
				int function_reg = -1;
				for (auto itr = operands_order.begin(); itr != operands_order.end(); itr++) {
					if (*itr == 0) {
						if (!direct_call) {
							codegen_expr_result res = codegen_expr(operands[*itr], lineno, true, false,
								-1, regs_available2, new_offset, status);
							function_reg = res.result_reg;
							result.insert(result.end(), res.insts.begin(), res.insts.end());
							regs_available2 &= ~(1 << res.result_reg);
						}
					} else {
						codegen_expr_result res = codegen_expr(operands[*itr], lineno, true, false,
							*itr - 1, regs_available2, new_offset, status);
						if (res.result_reg != *itr - 1) {
							throw codegen_error(lineno, "register preference not satisfied");
						}
						result.insert(result.end(), res.insts.begin(), res.insts.end());
						regs_available2 &= ~(1 << res.result_reg);
					}
				}
				// caller-saveな予約済みレジスタを保存する
				int regs_to_save2 = status.registers_reserved & 0xf;
				if (result_prefer_reg >= 0) regs_to_save2 &= ~(1 << result_prefer_reg);
				if (regs_to_save2 != 0) result.push_back(asm_inst(PUSH_REGS, regs_to_save2));
				// 関数を呼び出す
				if (direct_call) {
					result.push_back(asm_inst(CALL_DIRECT, direct_call_label));
				} else {
					result.push_back(asm_inst(CALL_INDIRECT, function_reg));
				}
				// 必要に応じて返り値をコピーする
				if (want_result) {
					if (result_prefer_reg >= 0) {
						if (result_prefer_reg != 0) result.push_back(asm_inst(MOV_REG, result_prefer_reg, 0));
						status.registers_written |= 1 << result_prefer_reg;
						result_reg = result_prefer_reg;
					} else if (!(regs_available & 1)) {
						result_reg = get_reg_to_use(lineno, regs_available, prefer_callee_save);
						result.push_back(asm_inst(MOV_REG, result_reg, 0));
						status.registers_written |= 1 << result_reg;
					} else {
						result_reg = 0;
					}
				}
				// 退避したレジスタを復帰する
				if (regs_to_save2 != 0) result.push_back(asm_inst(POP_REGS, regs_to_save2));
				if (regs_to_save != 0) result.push_back(asm_inst(POP_REGS, regs_to_save));
			}
			break;
		default:
			throw codegen_error(lineno, "unsupported or invalid operator");
		}
		break;
	}
	if (result_reg >= 0 && result_prefer_reg >= 0 && result_reg != result_prefer_reg) {
		result.push_back(asm_inst(MOV_REG, result_prefer_reg, result_reg));
		status.registers_written |= 1 << result_prefer_reg;
		result_reg = result_prefer_reg;
	}
	return codegen_expr_result(result, result_reg);
}

// 条件分岐のコード生成を行う
std::vector<asm_inst> codegen_conditional_jump(expression_node* expr, int lineno,
const std::string& dest_label, bool jump_if_true,
int regs_available, int stack_extra_offset, codegen_status& status) {
	if (expr == nullptr) {
		throw codegen_error(lineno, "NULL passed to codegen_conditional_jump()");
	}
	std::vector<asm_inst> result;
	switch (expr->kind) {
	case EXPR_INTEGER_LITERAL:
		if (jump_if_true ? expr->info.value != 0 : expr->info.value == 0) {
			result.push_back(asm_inst(JMP_DIRECT, dest_label));
		}
		break;
	case EXPR_IDENTIFIER:
		{
			codegen_expr_result res = codegen_expr(expr, lineno, true, false, -1,
				regs_available, stack_extra_offset, status);
			result.insert(result.end(), res.insts.begin(), res.insts.end());
			result.push_back(asm_inst(TEST_REG_REG, res.result_reg, res.result_reg));
			result.push_back(asm_inst(JCC, jump_if_true ? NONZERO : ZERO, dest_label));
		}
		break;
	case EXPR_OPERATOR:
		switch (expr->info.op.kind) {
		// 適用しても結果が変わらない単項演算子
		case OP_NONE: case OP_PARENTHESIS: // スルー
		case OP_ADDRESS: case OP_INDIRECTION: // 状態を変えるだけ
		case OP_ARRAY_TO_POINTER: case OP_FUNC_TO_FPTR: // bitcast
		case OP_PLUS: case OP_NEG: // 0か0でないかは変わらない
			// OP_CASTは上位ビットを切ることで結果が変わる可能性があるので対象外
			// OP_NOTは-1が0に、それ以外が非0になるので、単純な論理逆転にはならず、対象外
			return codegen_conditional_jump(expr->info.op.operands[0], lineno,
				dest_label, jump_if_true, regs_available, stack_extra_offset, status);
			break;
		// 論理否定
		case OP_LNOT:
			return codegen_conditional_jump(expr->info.op.operands[0], lineno,
				dest_label, !jump_if_true, regs_available, stack_extra_offset, status);
		// 比較
		case OP_LESS: case OP_GREATER: case OP_LESS_EQUAL: case OP_GREATER_EQUAL:
		case OP_EQUAL: case OP_NOT_EQUAL:
			{
				expression_node* operand0 = expr->info.op.operands[0];
				expression_node* operand1 = expr->info.op.operands[1];
				codegen_expr_result res0, res1;
				bool invert_comparision = false;
				if (operand1->kind == EXPR_INTEGER_LITERAL && operand1->info.value < 256) {
					if (operand1->info.value == 0) {
						if (expr->info.op.kind == OP_EQUAL) { // hoge == 0
							return codegen_conditional_jump(operand0, lineno, dest_label, !jump_if_true,
								regs_available, stack_extra_offset, status);
						} else if (expr->info.op.kind == OP_NOT_EQUAL) { // hoge != 0
							return codegen_conditional_jump(operand0, lineno, dest_label, jump_if_true,
								regs_available, stack_extra_offset, status);
						}
					}
					res0 = codegen_expr(operand0, lineno, true, false,
						-1, regs_available, stack_extra_offset, status);
					result.insert(result.end(), res0.insts.begin(), res0.insts.end());
					result.push_back(asm_inst(CMP_REG_LIT, res0.result_reg, operand1->info.value));
				} else if (operand0->kind == EXPR_INTEGER_LITERAL && operand0->info.value < 256) {
					if (operand0->info.value == 0) {
						if (expr->info.op.kind == OP_EQUAL) { // 0 == hoge
							return codegen_conditional_jump(operand1, lineno, dest_label, !jump_if_true,
								regs_available, stack_extra_offset, status);
						} else if (expr->info.op.kind == OP_NOT_EQUAL) { // 0 != hoge
							return codegen_conditional_jump(operand1, lineno, dest_label, jump_if_true,
								regs_available, stack_extra_offset, status);
						}
					}
					res1 = codegen_expr(operand1, lineno, true, false,
						-1, regs_available, stack_extra_offset, status);
					result.insert(result.end(), res1.insts.begin(), res1.insts.end());
					result.push_back(asm_inst(CMP_REG_LIT, res1.result_reg, operand0->info.value));
					invert_comparision = true;
				} else {
					if (cmp_expr_info(operand0->hint, operand1->hint) <= 0) {
						res0 = codegen_expr(operand0, lineno, true,
							operand1->hint != nullptr && operand1->hint->func_call_exists,
							-1, regs_available, stack_extra_offset, status);
						res1 = codegen_expr(operand1, lineno, true, false,
							-1, regs_available & ~(1 << res0.result_reg), stack_extra_offset, status);
						result.insert(result.end(), res0.insts.begin(), res0.insts.end());
						result.insert(result.end(), res1.insts.begin(), res1.insts.end());
					} else {
						res1 = codegen_expr(operand1, lineno, true,
							operand0->hint != nullptr && operand0->hint->func_call_exists,
							-1, regs_available, stack_extra_offset, status);
						res0 = codegen_expr(operand0, lineno, true, false,
							-1, regs_available & ~(1 << res1.result_reg), stack_extra_offset, status);
						result.insert(result.end(), res1.insts.begin(), res1.insts.end());
						result.insert(result.end(), res0.insts.begin(), res0.insts.end());
					}
					result.push_back(asm_inst(CMP_REG_REG, res0.result_reg, res1.result_reg));
				}
				int idx_operator, idx_logic;
				switch (expr->info.op.kind) {
				case OP_LESS:          idx_operator = 0; break;
				case OP_GREATER:       idx_operator = 1; break;
				case OP_LESS_EQUAL:    idx_operator = 2; break;
				case OP_GREATER_EQUAL: idx_operator = 3; break;
				case OP_EQUAL:         idx_operator = 4; break;
				case OP_NOT_EQUAL:     idx_operator = 5; break;
				default: throw codegen_error(lineno, "unexpected operator kind");
				}
				idx_logic = (jump_if_true ? 0 : 1) + (invert_comparision ? 2 : 0);
				if (is_arithmetic_type(operand0->type) && is_arithmetic_type(operand1->type)) {
					type_node* converted_type = usual_arithmetic_conversion(operand0->type, operand1->type);
					if (converted_type != nullptr &&
					converted_type->kind == TYPE_INTEGER && converted_type->info.is_signed) {
						idx_logic += 4;
					}
				}
				// true/false : 論理の反転 ( > と <= みたいな)
				// invert_comparision : 左辺と右辺の反転 ( > と < みたいな)
				// {
				//   true_noinvert, false_noinvert, true_invert, false_invert, (unsigned)
				//   true_noinvert, false_noinvert, true_invert, false_invert  (signed)
				// }
				static const jcc_cond cond_table[6][8] = {
					{ // OP_LESS
						L_UNSIGN, GE_UNSIGN, G_UNSIGN, LE_UNSIGN,
						L_SIGN,   GE_SIGN,   G_SIGN,   LE_SIGN
					}, { // OP_GREATER
						G_UNSIGN, LE_UNSIGN, L_UNSIGN, GE_UNSIGN,
						G_SIGN,   LE_SIGN,   L_SIGN,   GE_SIGN
					}, { // OP_LESS_EQUAL
						LE_UNSIGN, G_UNSIGN, GE_UNSIGN, L_UNSIGN,
						LE_SIGN,   G_SIGN,   GE_SIGN,   L_SIGN
					}, { // OP_GREATER_EQUAL
						GE_UNSIGN, L_UNSIGN, LE_UNSIGN, G_UNSIGN,
						GE_SIGN,   L_SIGN,   LE_SIGN,   G_SIGN
					}, { // OP_EQUAL
						EQ, NEQ, EQ, NEQ,
						EQ, NEQ, EQ, NEQ
					}, { // OP_NOT_EQUAL
						NEQ, EQ, NEQ, EQ,
						NEQ, EQ, NEQ, EQ
					}
				};
				result.push_back(asm_inst(JCC, cond_table[idx_operator][idx_logic], dest_label));
			}
			break;
		// 論理AND
		case OP_LAND:
			{
				std::string label;
				if (jump_if_true) label = get_label(status.next_label++);
				std::vector<asm_inst> res0, res1;
				// 左辺がfalseなら、false確定なので、右辺を評価する部分を飛ばす
				// trueの時に飛ぶ設定のときは、右辺を評価する部分の直後に飛ばす (飛び先には飛ばない)
				// falseの時に飛ぶ設定のときは、飛び先に飛ばす
				res0 = codegen_conditional_jump(expr->info.op.operands[0], lineno,
					jump_if_true ? label : dest_label,
					false, regs_available, stack_extra_offset, status);
				// 右辺の評価結果とjump_if_trueに基づき、飛び先に飛ばすかを決定する
				res1 = codegen_conditional_jump(expr->info.op.operands[1], lineno, dest_label,
					jump_if_true, regs_available, stack_extra_offset, status);
				result.insert(result.end(), res0.begin(), res0.end());
				result.insert(result.end(), res1.begin(), res1.end());
				if (jump_if_true) result.push_back(asm_inst(LABEL, label));
			}
			break;
		// 論理OR
		case OP_LOR:
			{
				std::string label;
				if (!jump_if_true) label = get_label(status.next_label++);
				std::vector<asm_inst> res0, res1;
				// 左辺がtrueなら、true確定なので、右辺を評価する部分を飛ばす
				// trueの時に飛ぶ設定のときは、飛び先に飛ばす
				// falseの時に飛ぶ設定のときは、右辺を評価する部分の直後に飛ばす (飛び先には飛ばない)
				res0 = codegen_conditional_jump(expr->info.op.operands[0], lineno,
					jump_if_true ? dest_label : label,
					true, regs_available, stack_extra_offset, status);
				// 右辺の評価結果とjump_if_trueに基づき、飛び先に飛ばすかを決定する
				res1 = codegen_conditional_jump(expr->info.op.operands[1], lineno, dest_label,
					jump_if_true, regs_available, stack_extra_offset, status);
				result.insert(result.end(), res0.begin(), res0.end());
				result.insert(result.end(), res1.begin(), res1.end());
				if (!jump_if_true) result.push_back(asm_inst(LABEL, label));
			}
			break;
		// 条件演算子
		case OP_COND:
			{
				std::vector<asm_inst> res_direct, res_jump;
				auto checkpoint0 = status.save_checkpoint();
				// 普通に評価し、0か0でないかで分岐する
				codegen_expr_result res_expr = codegen_expr(expr, lineno, true, false, -1,
					regs_available, stack_extra_offset, status);
				res_direct.insert(res_direct.end(), res_expr.insts.begin(), res_expr.insts.end());
				res_direct.push_back(asm_inst(TEST_REG_REG, res_expr.result_reg, res_expr.result_reg));
				res_direct.push_back(asm_inst(JCC, jump_if_true ? NONZERO : ZERO, dest_label));
				auto checkpoint1 = status.save_checkpoint();
				status.load_checkpoint(checkpoint0);
				// 条件分岐を使用する
				std::string false_label = get_label(status.next_label++);
				std::string nojump_label = get_label(status.next_label++);
				std::vector<asm_inst> res_cond = codegen_conditional_jump(expr->info.op.operands[0],
					lineno, false_label, false, regs_available, stack_extra_offset, status);
				res_jump.insert(res_jump.end(), res_cond.begin(), res_cond.end());
				std::vector<asm_inst> res_true = codegen_conditional_jump(expr->info.op.operands[1],
					lineno, dest_label, jump_if_true, regs_available, stack_extra_offset, status);
				res_jump.insert(res_jump.end(), res_true.begin(), res_true.end());
				res_jump.push_back(asm_inst(JMP_DIRECT, nojump_label));
				res_jump.push_back(asm_inst(LABEL, false_label));
				std::vector<asm_inst> res_false = codegen_conditional_jump(expr->info.op.operands[2],
					lineno, dest_label, jump_if_true, regs_available, stack_extra_offset, status);
				res_jump.insert(res_jump.end(), res_false.begin(), res_false.end());
				res_jump.push_back(asm_inst(LABEL, nojump_label));
				// 短い方を選ぶ
				if (res_jump.size() <= res_direct.size()) {
					result = res_jump;
				} else {
					result = res_direct;
					status.load_checkpoint(checkpoint1);
				}
			}
			break;
		// その他の演算子
		default:
			{
				codegen_expr_result res = codegen_expr(expr, lineno, true, false, -1,
					regs_available, stack_extra_offset, status);
				result.insert(result.end(), res.insts.begin(), res.insts.end());
				result.push_back(asm_inst(TEST_REG_REG, res.result_reg, res.result_reg));
				result.push_back(asm_inst(JCC, jump_if_true ? NONZERO : ZERO, dest_label));
			}
			break;
		}
		break;
	}
	return result;
}
