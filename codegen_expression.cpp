#include <string>
#include "ast.h"
#include "codegen.hpp"
#include "codegen_internal.hpp"

// 自動挿入用の演算子を自動挿入する
void codegen_add_auto_operator(operator_type op, int pos, expression_node** expr) {
	if (expr == nullptr || *expr == nullptr || (*expr)->type == nullptr) return;
	if ((*expr)->is_variable) {
		if ((*expr)->type->kind != TYPE_ARRAY) {
			switch (op) {
			case OP_PARENTHESIS:
			case OP_SIZEOF: case OP_ADDRESS:
			case OP_POST_INC: case OP_POST_DEC: case OP_PRE_INC: case OP_PRE_DEC:
				// 書き換え用のアドレスをそのままにする
				break;
			case OP_ASSIGN:
			case OP_MUL_ASSIGN: case OP_DIV_ASSIGN: case OP_MOD_ASSIGN:
			case OP_ADD_ASSIGN: case OP_SUB_ASSIGN: case OP_SHL_ASSIGN:
			case OP_SHR_ASSIGN: case OP_AND_ASSIGN: case OP_XOR_ASSIGN:
			case OP_OR_ASSIGN:
				// 右辺のみ、書き換え用のアドレスから値を読み取る
				if (pos == 1) *expr = new_operator(OP_READ_VALUE, *expr);
				break;
			default:
				// 書き換え用のアドレスから値を読み取る
				*expr = new_operator(OP_READ_VALUE, *expr);
				break;
			}
		}
	}
	if ((*expr)->type->kind == TYPE_ARRAY) {
		if (op != OP_SIZEOF && op != OP_ADDRESS) {
			// 「配列」を「配列の先頭要素へのポインタ」に変換する
			*expr = new_operator(OP_ARRAY_TO_POINTER, *expr);
		}
	} else if ((*expr)->type->kind == TYPE_FUNCTION) {
		if (op != OP_SIZEOF && op != OP_ADDRESS) {
			// 「関数」を「関数ポインタ」に変換する
			*expr = new_operator(OP_FUNC_TO_FPTR, *expr);
		}
	}
}

// 式中の識別子を解決する
void codegen_resolve_identifier_expr(expression_node* expr, int lineno, codegen_status& status) {
	if (expr == nullptr) {
		throw codegen_error(lineno, "NULL passed to codegen_resolve_identifier_expr()");
	}
	switch (expr->kind) {
	case EXPR_INTEGER_LITERAL:
		// 何もしない
		break;
	case EXPR_IDENTIFIER:
		// 識別子なので、該当するものを内側のブロックから順に探す
		for (auto itr = status.var_maps.rbegin(); itr != status.var_maps.rend(); itr++) {
			auto ident_info = itr->find(expr->info.ident.name);
			if (ident_info != itr->end()) {
				// 見つかったので、情報をセットして終了
				expr->info.ident.info = ident_info->second;
				expr->type = ident_info->second->type;
				return;
			}
		}
		// 見つからなかったのでエラー
		throw codegen_error(lineno, std::string("identifier ") + expr->info.ident.name + " not found");
	case EXPR_OPERATOR:
		codegen_resolve_identifier_expr(expr->info.op.operands[0], lineno, status);
		codegen_add_auto_operator(expr->info.op.kind, 0, &expr->info.op.operands[0]);
		if (expr->info.op.kind > OP_DUMMY_BINARY_START) {
			codegen_resolve_identifier_expr(expr->info.op.operands[1], lineno, status);
			codegen_add_auto_operator(expr->info.op.kind, 1, &expr->info.op.operands[1]);
		}
		if (expr->info.op.kind > OP_DUMMY_TERNARY_START) {
			codegen_resolve_identifier_expr(expr->info.op.operands[2], lineno, status);
			codegen_add_auto_operator(expr->info.op.kind, 2, &expr->info.op.operands[2]);
		}
		// オペランドの型が決まったはずなので、その計算結果の型を決め直す
		set_operator_expression_type(expr);
		if (expr->type == NULL) {
			throw codegen_error(lineno, "operand type error");
		}
		break;
	}
}

expr_info* get_operator_hint(expression_node* expr, int lineno) {
	if (expr == nullptr || expr->kind != EXPR_OPERATOR) {
		throw codegen_error(lineno, "invalid argument passed to get_operator_hint()");
	}
	expression_node** operands = expr->info.op.operands;
	switch (expr->info.op.kind) {
	// 後置インクリメント・デクリメント
	case OP_POST_INC: case OP_POST_DEC:
		if (operands[0]->kind == EXPR_IDENTIFIER) {
			var_info* vinfo = operands[0]->info.ident.info;
			if (vinfo->is_register) {
				// レジスタは直接加減算できるので、評価用のみ
				// 4バイト未満の時は、作業用レジスタを追加
				return new expr_info(
					vinfo->type->size == 4 ? 1 : 2,
					operands[0]->hint->func_call_exists);
			} else {
				// 直接参照できるメモリ上の変数の場合、評価用と作業用
				// そうでない場合、アドレス用と評価用と作業用
				// 4バイト未満の時は、作業用レジスタを追加
				bool is_direct_mem = false;
				if (vinfo->is_global) {
					is_direct_mem = (vinfo->offset % vinfo->type->size == 0 &&
						0 <= vinfo->offset && vinfo->offset / vinfo->type->size < 32);
				} else{
					is_direct_mem = (vinfo->offset % 4 == 0 && vinfo->type->size == 4 &&
						0 <= vinfo->offset && vinfo->offset / 4 < 256);
				}
				return new expr_info(
					(is_direct_mem ? 2 : 3) + (vinfo->type->size < 4 ? 1 : 0),
					operands[0]->hint->func_call_exists);
			}
		} else {
			int nregs = operands[0]->hint->num_regs_to_use;
			// アドレス用、評価用、作業用の3個
			// (レジスタ数に余裕が無いときは、退避するより作業用の値を戻して評価用にする方が良さそう)
			if (nregs < 3) nregs = 3;
			return new expr_info(nregs, operands[0]->hint->func_call_exists);
		}
		break;
	// 前置インクリメント
	case OP_PRE_INC: case OP_PRE_DEC:
		if (operands[0]->kind == EXPR_IDENTIFIER) {
			var_info* vinfo = operands[0]->info.ident.info;
			if (vinfo->is_register) {
				// レジスタは直接加減算できるので、追加消費なし (評価 = 変数レジスタ)
				// 4バイト未満の時は、作業用レジスタを使用する
				return new expr_info(
					vinfo->type->size == 4 ? 0 : 1,
					operands[0]->hint->func_call_exists);
			} else {
				// 直接参照できるメモリ上の変数の場合、評価用
				// そうでない場合、アドレス用と評価用
				// 4バイト未満の時は、作業用レジスタを追加する
				bool is_direct_mem = false;
				if (vinfo->is_global) {
					is_direct_mem = (vinfo->offset % vinfo->type->size == 0 &&
						0 <= vinfo->offset && vinfo->offset / vinfo->type->size < 32);
				} else{
					is_direct_mem = (vinfo->offset % 4 == 0 && vinfo->type->size == 4 &&
						0 <= vinfo->offset && vinfo->offset / 4 < 256);
				}
				return new expr_info(
					(is_direct_mem ? 1 : 2) + (vinfo->type->size < 4 ? 1 : 0),
					operands[0]->hint->func_call_exists);
			}
		} else {
			int nregs = operands[0]->hint->num_regs_to_use;
			// アドレス用、評価用の2個
			// 4バイト未満の時は、作業用レジスタを追加する
			int incdec_regs = operands[0]->type->size < 4 ? 3 : 2;
			if (nregs < incdec_regs) nregs = incdec_regs;
			return new expr_info(nregs, operands[0]->hint->func_call_exists);
		}
		break;
	// sizeof : リテラル扱い (VLAは非対応)
	case OP_SIZEOF:
		return new expr_info(1, false);
		break;
	// キャスト
	case OP_CAST:
		{
			int nregs = operands[0]->hint->num_regs_to_use;
			// 評価用
			if (nregs < 1) nregs = 1;
			// 作業用
			if (expr->info.op.cast_to != NULL && expr->type != NULL &&
			expr->info.op.cast_to->size != expr->type->size && nregs < 2) nregs = 2;
			return new expr_info(nregs, operands[0]->hint->func_call_exists);
		}
		break;
	// 論理NOT : 入力と出力を分ける
	case OP_LNOT:
		{
			int nregs = operands[0]->hint->num_regs_to_use;
			if (nregs < 2) nregs = 2;
			return new expr_info(nregs, operands[0]->hint->func_call_exists);
		}
		break;
	// 関数呼び出し(引数なし) : caller-saveを除けば呼び出し先を受け取るのみ
	case OP_FUNC_CALL_NOARGS:
		// 関数呼び出しなので、関数呼び出しありフラグを立てる
		// TODO: 識別子で直接呼び出す時の場合分け (どうせcaller-saveの影響で精度が…？)
		return new expr_info(operands[0]->hint->num_regs_to_use, true);
		break;
	// その他の単項演算子 : 計算結果のレジスタを使って計算→更新なので基本的に消費レジスタ数は同じ
	case OP_PARENTHESIS:
	case OP_ADDRESS: case OP_INDIRECTION: case OP_PLUS: case OP_NEG: case OP_NOT:
	case OP_ARRAY_TO_POINTER: case OP_FUNC_TO_FPTR: case OP_READ_VALUE:
		return new expr_info(operands[0]->hint->num_regs_to_use, operands[0]->hint->func_call_exists);
		break;
	// 両辺(のうちの高々1個)にu8が使える二項演算子
	case OP_ADD:
	case OP_LESS: case OP_GREATER: case OP_LESS_EQUAL: case OP_GREATER_EQUAL:
	case OP_EQUAL: case OP_NOT_EQUAL:
		{
			expression_node *literal, *other;
			bool literal_exists = false;
			if (operands[1]->kind == EXPR_INTEGER_LITERAL) {
				literal = operands[1];
				other = operands[0];
				literal_exists = true;
			} else if (operands[0]->kind == EXPR_INTEGER_LITERAL) {
				literal = operands[0];
				other = operands[1];
				literal_exists = true;
			} else {
				// リテラルは無いが、後の処理のために適当に設定する
				literal = operands[1];
				other = operands[0];
			}
			bool use_u = false;
			if (literal_exists) {
				if (expr->info.op.kind == OP_ADD) {
					// 足し算 : ポインタの処理と正負対応
					uint32_t value = literal->info.value;
					if (other->type != NULL && other->type->kind == TYPE_POINTER) {
						type_node* t_type = other->type->info.target_type;
						if (t_type != NULL) value *= t_type->size;
					}
					// -256 < value < 256
					use_u = value < 256 || value > UINT32_MAX - (256 - 1);
				} else {
					// 比較 : 単純なu8
					// 0 <= value < 256
					use_u = literal->info.value < 256;
				}
			}
			int nregs1 = other->hint->num_regs_to_use, nregs2 = literal->hint->num_regs_to_use;
			int ret = use_u ? nregs1 : (nregs1 == nregs2 ? nregs1 + 1 : (nregs1 > nregs2 ? nregs1 : nregs2));
			return new expr_info(ret <= 0 ? 1 : ret,
				operands[0]->hint->func_call_exists || operands[1]->hint->func_call_exists);
		}
		break;
	// 左辺には使えないが、右辺にはu8またはu5が使える二項演算子
	case OP_SUB:
	case OP_SHL: case OP_SHR:
		{
			bool use_u = false;
			if (operands[1]->kind == EXPR_INTEGER_LITERAL) {
				if (expr->info.op.kind == OP_SUB) {
					uint32_t value = operands[1]->info.value;
					if (operands[0]->type != NULL && operands[0]->type->kind == TYPE_POINTER) {
						type_node* t_type = operands[0]->type->info.target_type;
						if (t_type != NULL) value *= t_type->size;
					}
					// -256 < value < 256
					use_u = value < 256 || value > UINT32_MAX - (256 - 1);
				} else {
					// 0 <= value < 32
					use_u = operands[1]->info.value < 32;
				}
			}
			int nregs1 = operands[0]->hint->num_regs_to_use, nregs2 = operands[1]->hint->num_regs_to_use;
			int ret = use_u ? nregs1 : (nregs1 == nregs2 ? nregs1 + 1 : (nregs1 > nregs2 ? nregs1 : nregs2));
			return new expr_info(ret <= 0 ? 1 : ret,
				operands[0]->hint->func_call_exists || operands[1]->hint->func_call_exists);
		}
		break;
	// 論理演算 (0/1を返す、短絡評価あり)
	case OP_LAND: case OP_LOR:
		{
			int nregs = operands[0]->hint->num_regs_to_use, nregs2 = operands[1]->hint->num_regs_to_use;
			if (nregs2 > nregs) nregs = nregs2;
			return new expr_info(nregs < 2 ? 2 : nregs,
				operands[0]->hint->func_call_exists || operands[1]->hint->func_call_exists);
		}
		 break;
	// 代入
	case OP_ASSIGN:
		{
			int left_regs;
			if (operands[0]->kind == EXPR_IDENTIFIER) {
				var_info* vinfo = operands[0]->info.ident.info;
				if (vinfo->is_register) {
					// レジスタは直接加減算できるので、追加消費なし (評価 = 変数レジスタ)
					// 4バイト未満の時は、作業用レジスタを使用する
					left_regs = vinfo->type->size == 4 ? 0 : 1;
				} else {
					// 直接参照できるメモリ上の変数の場合、評価用
					// そうでない場合、アドレス用と評価用
					// 4バイト未満の時は、作業用レジスタを追加する
					bool is_direct_mem = false;
					if (vinfo->is_global) {
						is_direct_mem = (vinfo->offset % vinfo->type->size == 0 &&
							0 <= vinfo->offset && vinfo->offset / vinfo->type->size < 32);
					} else{
						is_direct_mem = (vinfo->offset % 4 == 0 && vinfo->type->size == 4 &&
							0 <= vinfo->offset && vinfo->offset / 4 < 256);
					}
					left_regs = (is_direct_mem ? 1 : 2) + (vinfo->type->size < 4 ? 1 : 0);
				}
			} else {
				left_regs = operands[0]->hint->num_regs_to_use;
				// アドレス用、評価用の2個
				// 4バイト未満の時は、作業用レジスタを追加する
				int work_regs = operands[0]->type->size < 4 ? 3 : 2;
				if (left_regs < work_regs) left_regs = work_regs;
			}
			int right_regs = operands[1]->hint->num_regs_to_use;
			// TODO: 精度を上げる
			// left_regsで保存するべきなのはアドレス用のみ (評価用はright_regsの値と重なる)
			// レジスタへのu8の代入とかを考えていくと…？
			return new expr_info(left_regs > right_regs ? left_regs : right_regs,
				operands[0]->hint->func_call_exists || operands[1]->hint->func_call_exists);
		}
		break;
	// 複合代入演算子
	case OP_MUL_ASSIGN: //case OP_DIV_ASSIGN: case OP_MOD_ASSIGN:
	case OP_ADD_ASSIGN: case OP_SUB_ASSIGN: case OP_SHL_ASSIGN: case OP_SHR_ASSIGN:
	case OP_AND_ASSIGN: case OP_XOR_ASSIGN: case OP_OR_ASSIGN:
		{
			int left_regs;
			if (operands[0]->kind == EXPR_IDENTIFIER) {
				var_info* vinfo = operands[0]->info.ident.info;
				if (vinfo->is_register) {
					// レジスタは直接加減算できるので、追加消費なし (評価 = 変数レジスタ)
					// 4バイト未満の時は、作業用レジスタを使用する
					left_regs = vinfo->type->size == 4 ? 0 : 1;
				} else {
					// 直接参照できるメモリ上の変数の場合、評価用
					// そうでない場合、アドレス用と評価用
					// 4バイト未満の時は、作業用レジスタを追加する
					bool is_direct_mem = false;
					if (vinfo->is_global) {
						is_direct_mem = (vinfo->offset % vinfo->type->size == 0 &&
							0 <= vinfo->offset && vinfo->offset / vinfo->type->size < 32);
					} else{
						is_direct_mem = (vinfo->offset % 4 == 0 && vinfo->type->size == 4 &&
							0 <= vinfo->offset && vinfo->offset / 4 < 256);
					}
					left_regs = (is_direct_mem ? 1 : 2) + (vinfo->type->size < 4 ? 1 : 0);
				}
			} else {
				left_regs = operands[0]->hint->num_regs_to_use;
				// アドレス用、評価用の2個
				// 4バイト未満の時は、作業用レジスタを追加する
				int work_regs = operands[0]->type->size < 4 ? 3 : 2;
				if (left_regs < work_regs) left_regs = work_regs;
			}
			int right_regs = operands[1]->hint->num_regs_to_use;
			// TODO: 精度を上げる
			// left_regsで保存するべきなのはアドレス用と評価用
			// left_regs == right_regsの時は、値1個だけを保存する右辺を先に評価する
			// u8の利用とかを考えていくと…？
			return new expr_info(left_regs > right_regs ? left_regs : right_regs,
				operands[0]->hint->func_call_exists || operands[1]->hint->func_call_exists);
		}
		break;
	// コンマ (左辺を評価し、それを捨てて右辺を評価)
	case OP_COMMA:
		{
			int nregs1 = operands[0]->hint->num_regs_to_use, nregs2 = operands[1]->hint->num_regs_to_use;
			return new expr_info(nregs1 > nregs2 ? nregs1 : nregs2,
				operands[0]->hint->func_call_exists || operands[1]->hint->func_call_exists);
		}
		break;
	// その他の二項演算子 (割り算は直接行える命令が無さそうなので保留)
	case OP_MUL: //case OP_DIV: case OP_MOD:
	case OP_AND: case OP_XOR: case OP_OR:
		{
			int nregs1 = operands[0]->hint->num_regs_to_use, nregs2 = operands[1]->hint->num_regs_to_use;
			int ret = nregs1 == nregs2 ? nregs1 + 1 : (nregs1 > nregs2 ? nregs1 : nregs2);
			return new expr_info(ret <= 0 ? 1 : ret,
				operands[0]->hint->func_call_exists || operands[1]->hint->func_call_exists);
		}
		break;
	// 条件演算子
	case OP_COND:
		{
			int nregs, nregs2, nregs3;
			nregs = operands[0]->hint->num_regs_to_use;
			nregs2 = operands[1]->hint->num_regs_to_use;
			if (nregs2 > nregs) nregs = nregs2;
			nregs3 = operands[2]->hint->num_regs_to_use;
			if (nregs3 > nregs) nregs = nregs3;
			return new expr_info(nregs, operands[0]->hint->func_call_exists || 
				operands[1]->hint->func_call_exists || operands[2]->hint->func_call_exists);
		}
		break;
	default:
		throw codegen_error(lineno, "unsupported or invalid operator");
	}
}

// 式の前処理を行う
// * 関数呼び出しおよびグローバル変数の参照があるかを調べる
// * スケジューリング用ヒントを設定する
void codegen_preprocess_expr(expression_node* expr, int lineno, codegen_status& status) {
	if (expr == nullptr) {
		throw codegen_error(lineno, "NULL passed to codegen_preprocess_expr()");
	}
	switch (expr->kind) {
	case EXPR_INTEGER_LITERAL:
		// リテラルは1レジスタで置ける
		expr->hint = new expr_info(1, false);
		break;
	case EXPR_IDENTIFIER:
		// グローバル変数なら、使用フラグを立てる
		// グローバルな識別子でも、関数の場合は、グローバル変数とはみなさない
		// TODO: 直接呼び出さず関数ポインタ扱いする場合、関数もグローバル変数扱い(基準アドレスを要求)する
		if (expr->info.ident.info->is_global && expr->info.ident.info->type->kind != TYPE_FUNCTION) {
			status.gv_access_exists = true;
		}
		// レジスタ変数なら、割り当てられたレジスタを直接参照すればいいので使用レジスタ数0
		// それ以外の場合は、アドレスを置くので使用レジスタ数1 (アドレスを置かない場合は親のノードで考える)
		expr->hint = new expr_info(expr->info.ident.info->is_register ? 0 : 1, false);
		break;
	case EXPR_OPERATOR:
		codegen_preprocess_expr(expr->info.op.operands[0], lineno, status);
		if (expr->info.op.kind > OP_DUMMY_BINARY_START) {
			codegen_preprocess_expr(expr->info.op.operands[1], lineno, status);
		}
		if (expr->info.op.kind > OP_DUMMY_TERNARY_START) {
			codegen_preprocess_expr(expr->info.op.operands[2], lineno, status);
		}
		// 関数呼び出しなら、使用フラグを立てる
		if (expr->info.op.kind == OP_FUNC_CALL || expr->info.op.kind == OP_FUNC_CALL_NOARGS) {
			status.call_exists = true;
		}
		// ヒントを設定する (長くなりそうなので分割)
		expr->hint = get_operator_hint(expr, lineno);
		break;
	}
}
