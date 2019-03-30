#include <stdarg.h>
#include "ast.h"
#include "util.h"

expression_node* new_integer_literal(uint32_t value, int is_signed) {
	expression_node* node = malloc_check(sizeof(expression_node));
	node->kind = EXPR_INTEGER_LITERAL;
	node->type = new_prim_type(4, is_signed);
	node->is_variable = 0;
	node->info.value = value;
	return node;
}

expression_node* new_expr_identifier(char* name) {
	expression_node* node = malloc_check(sizeof(expression_node));
	node->kind = EXPR_IDENTIFIER;
	node->type = NULL;
	node->is_variable = 1;
	node->info.ident.name = name;
	node->info.ident.info = NULL;
	return node;
}

expression_node* new_operator(operator_type op, ...) {
	expression_node* node = malloc_check(sizeof(expression_node));
	va_list args;
	node->kind = EXPR_OPERATOR;
	node->type = NULL;
	node->is_variable = (op == OP_INDIRECTION || op == OP_ARRAY_REF);
	node->info.op.kind = op;
	va_start(args, op);
	node->info.op.operands[0] = va_arg(args, expression_node*);
	if (op > OP_DUMMY_BINARY_START) {
		node->info.op.operands[1] = va_arg(args, expression_node*);
	}
	if (op > OP_DUMMY_TERNARY_START) {
		node->info.op.operands[2] = va_arg(args, expression_node*);
	}
	if (op == OP_CAST) {
		node->info.op.cast_to = va_arg(args, type_node*);
	} else {
		node->info.op.cast_to = NULL;
	}
	va_end(args);
	if (op == OP_PARENTHESIS) {
		// カッコの場合、is_variableはオペランドそのまま
		// (上ではまだオペランドを取得していないので設定できない)
		node->is_variable = node->info.op.operands[0]->is_variable;
	}
	if (op == OP_FUNC_CALL) {
		// 引数がある関数呼び出しの場合、引数のデータを設定する
		// まず引数の数を調べる
		int argument_count = 1;
		expression_node* node_ptr = node->info.op.operands[0];
		while (node_ptr->kind == EXPR_OPERATOR && node_ptr->info.op.kind == OP_COMMA) {
			argument_count++;
			node_ptr = node_ptr->info.op.operands[0];
		}
		// 実際に引数のデータを設定する
		node->info.op.argument_num = argument_count;
		node->info.op.arguments = malloc_check(sizeof(expression_node*) * argument_count);
		node_ptr = node->info.op.operands[0];
		for (int i = argument_count - 1; i > 0; i--) {
			node->info.op.arguments[i] = node_ptr->info.op.operands[1];
			node_ptr = node_ptr->info.op.operands[0];
		}
		node->info.op.arguments[0] = node_ptr;
	} else {
		node->info.op.argument_num = 0;
		node->info.op.arguments = NULL;
	}
	set_operator_expression_type(node);
	return node;
}

void set_operator_expression_type(expression_node* node) {
	if (node == NULL || node->kind != EXPR_OPERATOR) return;

	type_node* types[3] = {NULL, NULL, NULL};
	int is_variable[3] = {0, 0, 0};
	types[0] = node->info.op.operands[0]->type;
	is_variable[0] = node->info.op.operands[0]->is_variable;
	if (node->info.op.kind > OP_DUMMY_BINARY_START) {
		types[1] = node->info.op.operands[1]->type;
		is_variable[1] = node->info.op.operands[1]->is_variable;
	}
	if (node->info.op.kind > OP_DUMMY_TERNARY_START) {
		types[2] = node->info.op.operands[2]->type;
		is_variable[2] = node->info.op.operands[2]->is_variable;
	}

	node->type = NULL;
	switch (node->info.op.kind) {
	case OP_NONE:
	case OP_PARENTHESIS:
		node->type = types[0];
		break;
	case OP_FUNC_CALL_NOARGS:
	case OP_FUNC_CALL:
		if (!is_variable[0] && is_pointer_type(types[0])) {
			type_node* pointed_type = types[0]->info.target_type;
			// 呼び出されるのは関数か
			if (is_function_type(pointed_type)) {
				type_node* return_type = pointed_type->info.f.return_type;
				// 戻り値の型は有効か
				if (is_void_type(return_type) ||
				(is_complete_object_type(return_type) && !is_array_type(return_type))) {
					int arg_num = pointed_type->info.f.arg_num;
					type_node** arg_types = pointed_type->info.f.arg_types;
					if (arg_num < 0) {
						// 型に引数の情報が無い
						node->type = return_type;
					} else if (arg_num == node->info.op.argument_num) {
						// 型に引数の情報があるので、チェックする
						int ok = 1;
						for (int i = 0; i < arg_num; i++) {
							if (!is_assignable(arg_types[i], node->info.op.arguments[i])) {
								ok = 0;
								break;
							}
						}
						if (ok) node->type = return_type;
					}
				}
			}
		}
		break;
	case OP_POST_INC:
	case OP_POST_DEC:
	case OP_PRE_INC:
	case OP_PRE_DEC:
		if (is_variable[0] &&
		(is_real_type(types[0]) || is_pointer_type(types[0]))) {
			node->type = types[0];
		}
		break;
	case OP_ADDRESS:
		if (is_variable[0] && types[0] != NULL) {
			node->type = new_ptr_type(types[0]);
		}
		break;
	case OP_INDIRECTION:
		if (!is_variable[0] && is_pointer_type(types[0])) {
			node->type = types[0]->info.target_type;
		}
		break;
	case OP_PLUS:
	case OP_NEG:
		if (!is_variable[0] && is_arithmetic_type(types[0])) {
			node->type = integer_promotion(types[0]);
		}
		break;
	case OP_NOT:
		if (!is_variable[0] && is_integer_type(types[0])) {
			node->type = integer_promotion(types[0]);
		}
		break;
	case OP_LNOT:
		if (!is_variable[0] && is_scalar_type(types[0])) {
			node->type = new_prim_type(4, 1);
		}
		break;
	case OP_SIZEOF:
		// is_variableはどっちでもいい
		if (types[0] != NULL) node->type = new_prim_type(4, 0);
		break;
	case OP_CAST:
		if (!is_variable[0] && types[0] != NULL) {
			type_node* cast_to = node->info.op.cast_to;
			if (is_void_type(cast_to) ||
			(is_scalar_type(node->info.op.cast_to) && is_scalar_type(types[0]))) {
				if (types[0]->kind == TYPE_INTEGER) {
					node->type = integer_promotion(cast_to);
				} else {
					node->type = cast_to;
				}
			}
		}
		break;
	case OP_ARRAY_TO_POINTER:
		// is_variableはどっちでもいい?
		if (is_array_type(types[0])) {
			node->type = new_ptr_type(types[0]->info.element_type);
		}
		break;
	case OP_FUNC_TO_FPTR:
		if (!is_variable[0] && is_function_type(types[0])) {
			node->type = new_ptr_type(types[0]);
		}
		break;
	case OP_READ_VALUE:
		if (is_variable[0] && types[0] != NULL) {
			node->type = types[0];
		}
		break;
	case OP_DUMMY_BINARY_START: break;
	case OP_ARRAY_REF:
		if (!is_variable[0] && !is_variable[1] &&
		((is_pointer_type(types[0]) && is_integer_type(types[1])) ||
		(is_integer_type(types[0]) && is_pointer_type(types[1])))) {
			type_node* ptr_type = is_pointer_type(types[0]) ? types[0] : types[1];
			node->type = ptr_type->info.target_type;
		}
		break;
	case OP_MUL:
	case OP_DIV:
		if (!is_variable[0] && !is_variable[1] &&
		is_arithmetic_type(types[0]) && is_arithmetic_type(types[1])) {
			node->type = usual_arithmetic_conversion(types[0], types[1]);
		}
		break;
	case OP_MOD:
		if (!is_variable[0] && !is_variable[1] &&
		is_integer_type(types[0]) && is_integer_type(types[1])) {
			node->type = usual_arithmetic_conversion(types[0], types[1]);
		}
		break;
	case OP_ADD:
		if (!is_variable[0] && !is_variable[1]) {
			if (is_integer_type(types[0])) {
				if (is_integer_type(types[1])) {
					node->type = usual_arithmetic_conversion(types[0], types[1]);
				} else if (is_pointer_type(types[1]) && is_complete_object_type(types[1]->info.target_type)) {
					node->type = types[1];
				}
			} else if (is_pointer_type(types[0]) && is_complete_object_type(types[0]->info.target_type) &&
			is_integer_type(types[1])) {
				node->type = types[0];
			}
		}
		break;
	case OP_SUB:
		if (!is_variable[0] && !is_variable[1]) {
			if (is_arithmetic_type(types[0]) && is_arithmetic_type(types[1])) {
				node->type = usual_arithmetic_conversion(types[0], types[1]);
			} else if (is_pointer_type(types[0]) && is_complete_object_type(types[0]->info.target_type) &&
			is_integer_type(types[1])) {
				node->type = types[0];
			} else if (is_pointer_type(types[0]) && is_complete_object_type(types[0]->info.target_type) &&
			is_pointer_type(types[1]) && is_complete_object_type(types[1]->info.target_type) &&
			is_compatible_type(types[0]->info.target_type, types[1]->info.target_type)) {
				node->type = new_prim_type(4, 1);
			}
		}
		break;
	case OP_SHL:
	case OP_SHR:
		if (!is_variable[0] && !is_variable[1] &&
		is_integer_type(types[0]) && is_integer_type(types[1])) {
			node->type = integer_promotion(types[0]);
		}
		break;
	case OP_LESS:
	case OP_GREATER:
	case OP_LESS_EQUAL:
	case OP_GREATER_EQUAL:
		if (!is_variable[0] && !is_variable[1]) {
			if ((is_real_type(types[0]) && is_real_type(types[1])) ||
			(is_pointer_type(types[0]) && is_pointer_type(types[1]) &&
			is_compatible_type(types[0]->info.target_type, types[1]->info.target_type) &&
			is_object_type(types[0]->info.target_type) && is_object_type(types[1]->info.target_type))) {
				node->type = new_prim_type(4, 1);
			}
		}
		break;
	case OP_EQUAL:
	case OP_NOT_EQUAL:
		if (!is_variable[0] && !is_variable[1]) {
			if ((is_arithmetic_type(types[0]) && is_arithmetic_type(types[1])) ||
			(is_pointer_type(types[0]) && is_pointer_type(types[1]) &&
			(is_compatible_type(types[0]->info.target_type, types[1]->info.target_type) ||
			is_void_type(types[0]) || is_void_type(types[1])))) {
				// 整数と整数、ポインタとポインタ
				node->type = new_prim_type(4, 1);
			} else {
				// ポインタと null pointer constantの比較かをチェックする
				expression_node* integer_node = NULL;
				if (is_pointer_type(types[0]) && is_integer_type(types[1])) {
					integer_node = node->info.op.operands[1];
				} else if (is_integer_type(types[0]) && is_pointer_type(types[1])) {
					integer_node = node->info.op.operands[0];
				}
				if (integer_node != NULL && integer_node->kind == EXPR_INTEGER_LITERAL &&
				integer_node->info.value == 0) {
					node->type = new_prim_type(4, 1);
				}
			}
		}
		break;
	case OP_AND:
	case OP_XOR:
	case OP_OR:
		if (!is_variable[0] && !is_variable[1] &&
		is_integer_type(types[0]) && is_integer_type(types[1])) {
			node->type = usual_arithmetic_conversion(types[0], types[1]);
		}
		break;
	case OP_LAND:
	case OP_LOR:
		if (!is_variable[0] && !is_variable[1] &&
		is_scalar_type(types[0]) && is_scalar_type(types[1])) {
			node->type = new_prim_type(4, 1);
		}
		break;
	case OP_ASSIGN:
		if (is_variable[0] && !is_variable[1] && is_assignable(types[0], node->info.op.operands[1])) {
			node->type = is_integer_type(types[0]) ? integer_promotion(types[0]) : types[0];
		}
		break;
	case OP_MUL_ASSIGN:
	case OP_DIV_ASSIGN:
		if (is_variable[0] && !is_variable[1] &&
		is_arithmetic_type(types[0]) && is_arithmetic_type(types[1])) {
			node->type = integer_promotion(types[0]);
		}
		break;
	case OP_MOD_ASSIGN:
		if (is_variable[0] && !is_variable[1] &&
		is_integer_type(types[0]) && is_integer_type(types[1])) {
			node->type = integer_promotion(types[0]);
		}
		break;
	case OP_ADD_ASSIGN:
	case OP_SUB_ASSIGN:
		if (is_variable[0] && !is_variable[1] &&
		((is_pointer_type(types[0]) && is_complete_object_type(types[0]->info.target_type) &&
		is_integer_type(types[1])) || (is_arithmetic_type(types[0]) && is_arithmetic_type(types[1])))) {
			node->type = integer_promotion(types[0]);
		}
		break;
	case OP_SHL_ASSIGN:
	case OP_SHR_ASSIGN:
	case OP_AND_ASSIGN:
	case OP_XOR_ASSIGN:
	case OP_OR_ASSIGN:
		if (is_variable[0] && !is_variable[1] &&
		is_integer_type(types[0]) && is_integer_type(types[1])) {
			node->type = integer_promotion(types[0]);
		}
		break;
	case OP_COMMA:
		if (!is_variable[0] && !is_variable[1]) node->type = types[1];
		break;
	case OP_DUMMY_TERNARY_START: break;
	case OP_COND:
		if (!is_variable[0] && !is_variable[1] && !is_variable[2] && is_scalar_type(types[0])) {
			if (is_arithmetic_type(types[1]) && is_arithmetic_type(types[2])) {
				// 整数と整数
				node->type = usual_arithmetic_conversion(types[1], types[2]);
			} else if (is_void_type(types[1]) && is_void_type(types[2])) {
				// voidとvoid
				node->type = types[1];
			} else if (is_pointer_type(types[1]) && is_pointer_type(types[2])) {
				// ポインタとポインタ
				type_node* target1 = types[1]->info.target_type;
				type_node* target2 = types[2]->info.target_type;
				if (is_compatible_type(target1, target2)) {
					node->type = types[1];
				} else if (is_void_type(target1) && is_object_type(target2)) {
					node->type = types[1];
				} else if (is_object_type(target1) && is_void_type(target2)) {
					node->type = types[2];
				}
			} else {
				// ポインタと null pointer constantかをチェックする
				expression_node* integer_node = NULL;
				type_node* pointer_type = NULL;
				if (is_pointer_type(types[1]) && is_integer_type(types[2])) {
					integer_node = node->info.op.operands[2];
					pointer_type = types[1];
				} else if (is_integer_type(types[1]) && is_pointer_type(types[2])) {
					integer_node = node->info.op.operands[1];
					pointer_type = types[2];
				}
				if (integer_node != NULL && integer_node->kind == EXPR_INTEGER_LITERAL &&
				integer_node->info.value == 0) {
					node->type = pointer_type;
				}
			}
		}
		break;
	}
}

// constfoldするとともに、カッコを除去する
// 再利用できるところはしながら、新しいノードを返す (→残念ながらfree()すると危険！)
expression_node* constfold(expression_node* node) {
	if (node == NULL) return NULL;
	if (node->kind == EXPR_OPERATOR) {
		// オペランドのconstfoldをする
		node->info.op.operands[0] = constfold(node->info.op.operands[0]);
		if (node->info.op.kind > OP_DUMMY_BINARY_START) {
			node->info.op.operands[1] = constfold(node->info.op.operands[1]);
		}
		if (node->info.op.kind > OP_DUMMY_TERNARY_START) {
			node->info.op.operands[2] = constfold(node->info.op.operands[2]);
		}
		// このノードのconstfoldをする
		switch (node->info.op.kind) {
		case OP_PARENTHESIS:
			node = node->info.op.operands[0]; // カッコの除去
			break;
		case OP_PLUS:
			if (node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL) {
				expression_node* new_node = malloc_check(sizeof(expression_node));
				new_node->kind = EXPR_INTEGER_LITERAL;
				new_node->type = node->type; // integer promotion後の型
				new_node->is_variable = 0;
				new_node->hint = NULL;
				new_node->info.value = node->info.op.operands[0]->info.value;
				node = new_node;
			}
			break;
		case OP_NEG:
			if (node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL) {
				expression_node* new_node = malloc_check(sizeof(expression_node));
				new_node->kind = EXPR_INTEGER_LITERAL;
				new_node->type = node->type; // integer promotion後の型
				new_node->is_variable = 0;
				new_node->hint = NULL;
				new_node->info.value = -(node->info.op.operands[0]->info.value);
				node = new_node;
			}
			break;
		case OP_NOT:
			if (node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL) {
				expression_node* new_node = malloc_check(sizeof(expression_node));
				new_node->kind = EXPR_INTEGER_LITERAL;
				new_node->type = node->type; // integer promotion後の型
				new_node->is_variable = 0;
				new_node->hint = NULL;
				new_node->info.value = ~(node->info.op.operands[0]->info.value);
				node = new_node;
			}
			break;
		case OP_LNOT:
			if (node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL) {
				node = new_integer_literal(node->info.op.operands[0]->info.value == 0 ? 1 : 0, 1);
			}
			break;
		case OP_SIZEOF:
			if (node->info.op.operands[0]->type != NULL) {
				node = new_integer_literal(node->info.op.operands[0]->type->size, 0);
			}
			break;
		case OP_CAST:
			if (node->info.op.cast_to != NULL && node->info.op.cast_to->kind == TYPE_INTEGER &&
			node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL) {
				uint32_t value = node->info.op.operands[0]->info.value;
				int is_signed = node->info.op.cast_to->info.is_signed;
				int size = node->info.op.cast_to->size;
				if (size < 4) {
					uint32_t mask = UINT32_C(0xffffffff) >> (8 * (4 - size));
					value &= mask;
					if (is_signed) {
						uint32_t sign_mask = UINT32_C(1) << (8 * size - 1);
						if (value & sign_mask) value |= ~mask;
					}
				}
				expression_node* new_node = malloc_check(sizeof(expression_node));
				new_node->kind = EXPR_INTEGER_LITERAL;
				new_node->type = node->type;
				new_node->is_variable = 0;
				new_node->hint = NULL;
				new_node->info.value = value;
				node = new_node;
			}
			break;
		case OP_MUL: case OP_DIV: case OP_MOD:
		case OP_ADD: case OP_SUB: case OP_SHL: case OP_SHR:
		case OP_AND: case OP_XOR: case OP_OR:
			if (node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL &&
			node->info.op.operands[1]->kind == EXPR_INTEGER_LITERAL) {
				uint32_t value1 = node->info.op.operands[0]->info.value;
				uint32_t value2 = node->info.op.operands[1]->info.value;
				uint32_t let_value;
				int is_signed = node->type == NULL || node->type->kind != TYPE_INTEGER ||
					node->type->info.is_signed;
				if ((node->info.op.kind == OP_DIV || node->info.op.kind == OP_MOD) && value2 == 0) {
					break; // ゼロ除算
				}
				switch (node->info.op.kind) {
				case OP_MUL:
					let_value = value1 * value2;
					break;
				case OP_DIV:
					if (is_signed) {
						int32_t svalue1, svalue2;
						svalue1 =
							value1 == UINT32_C(0x80000000) ? -INT32_C(0x7fffffff) - 1 :
							value1 & UINT32_C(0x80000000) ? -((int32_t)(-value1)) :
							(int32_t)value1;
						svalue2 =
							value2 == UINT32_C(0x80000000) ? -INT32_C(0x7fffffff) - 1 :
							value2 & UINT32_C(0x80000000) ? -((int32_t)(-value2)) :
							(int32_t)value2;
						let_value = svalue1 / svalue2;
					} else {
						let_value = value1 * value2;
					}
					break;
				case OP_MOD:
					if (is_signed) {
						int32_t svalue1, svalue2;
						svalue1 =
							value1 == UINT32_C(0x80000000) ? -INT32_C(0x7fffffff) - 1 :
							value1 & UINT32_C(0x80000000) ? -((int32_t)(-value1)) :
							(int32_t)value1;
						svalue2 =
							value2 == UINT32_C(0x80000000) ? -INT32_C(0x7fffffff) - 1 :
							value2 & UINT32_C(0x80000000) ? -((int32_t)(-value2)) :
							(int32_t)value2;
						let_value = svalue1 % svalue2;
					} else {
						let_value = value1 * value2;
					}
					break;
				case OP_ADD:
					let_value = value1 + value2;
					break;
				case OP_SUB:
					let_value = value1 - value2;
					break;
				case OP_SHL:
					let_value = value1 << (value2 & 31);
					break;
				case OP_SHR:
					if (is_signed && (value1 & UINT32_C(0x80000000))) {
						uint64_t value1_ex = UINT64_C(0xffffffff00000000) | value1;
						let_value = (uint32_t)(value1_ex >> (value2 & 31));
					} else {
						let_value = value1 >> (value2 & 31);
					}
					break;
				case OP_AND:
					let_value = value1 & value2;
					break;
				case OP_XOR:
					let_value = value1 ^ value2;
					break;
				case OP_OR:
					let_value = value1 | value2;
					break;
				default:
					let_value = 0;
					break;
				}
				expression_node* new_node = malloc_check(sizeof(expression_node));
				new_node->kind = EXPR_INTEGER_LITERAL;
				new_node->type = node->type; // usual arithmetic conversion後の型
				new_node->is_variable = 0;
				new_node->hint = NULL;
				new_node->info.value = let_value;
				node = new_node;
			}
			break;
		case OP_LESS: case OP_GREATER: case OP_LESS_EQUAL: case OP_GREATER_EQUAL:
		case OP_EQUAL: case OP_NOT_EQUAL:
			if (node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL &&
			node->info.op.operands[1]->kind == EXPR_INTEGER_LITERAL) {
				uint32_t value1 = node->info.op.operands[0]->info.value;
				uint32_t value2 = node->info.op.operands[1]->info.value;
				uint32_t sign1 = value1 & UINT32_C(0x80000000);
				uint32_t sign2 = value2 & UINT32_C(0x80000000);
				uint32_t let_value;
				type_node* uaced_type = usual_arithmetic_conversion(
					node->info.op.operands[0]->type, node->info.op.operands[1]->type);
				int is_signed = uaced_type == NULL || uaced_type->kind != TYPE_INTEGER ||
					uaced_type->info.is_signed;
				switch (node->info.op.kind) {
				case OP_LESS:
					let_value =
						is_signed && sign1 && !sign2 ? 1 : // 負 < 非負 → true
						is_signed && !sign1 && sign2 ? 0 : // 非負 < 負 → false
						value1 < value2;
					break;
				case OP_GREATER:
					let_value =
						is_signed && sign1 && !sign2 ? 0 : // 負 > 非負 → false
						is_signed && !sign1 && sign2 ? 1 : // 非負 > 負 → true
						value1 > value2;
					break;
				case OP_LESS_EQUAL:
					let_value =
						is_signed && sign1 && !sign2 ? 1 : // 負 <= 非負 → true
						is_signed && !sign1 && sign2 ? 0 : // 非負 <= 負 → false
						value1 <= value2;
					break;
				case OP_GREATER_EQUAL:
					let_value =
						is_signed && sign1 && !sign2 ? 0 : // 負 >= 非負 → false
						is_signed && !sign1 && sign2 ? 1 : // 非負 = 負 → true
						value1 >= value2;
					break;
				case OP_EQUAL:
					let_value = (value1 == value2);
					break;
				case OP_NOT_EQUAL:
					let_value = (value1 != value2);
					break;
				default:
					let_value = 0;
					break;
				}
				node = new_integer_literal(let_value, 1);
			}
			break;
		case OP_LAND:
			if (node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL) {
				if (node->info.op.operands[0]->info.value == 0) {
					// 0 && hoge → hogeを評価せずに0
					node = new_integer_literal(0, 1);
				} else if (node->info.op.operands[1]->kind == EXPR_INTEGER_LITERAL) {
					// 非0 && hoge → hogeを評価して非0なら1、0なら0
					node = new_integer_literal(node->info.op.operands[1]->info.value != 0 ? 1 : 0, 1);
				}
			}
			break;
		case OP_LOR:
			if (node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL) {
				if (node->info.op.operands[0]->info.value != 0) {
					// 非0 || hoge → hogeを評価せずに1
					node = new_integer_literal(1, 1);
				} else if (node->info.op.operands[1]->kind == EXPR_INTEGER_LITERAL) {
					// 0 || hoge → hogeを評価して非0なら1、0なら0
					node = new_integer_literal(node->info.op.operands[1]->info.value != 0 ? 1 : 0, 1);
				}
			}
			break;
		case OP_COMMA:
			if (node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL) {
				// 左辺が定数なら、捨てる (定数でないなら、副作用があり得るので捨てない)
				node = node->info.op.operands[1];
			}
			break;
		case OP_COND:
			if (node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL && node->type != NULL) {
				expression_node* let_node =
					node->info.op.operands[node->info.op.operands[0]->info.value != 0 ? 1 : 2];
				// let_nodeだけのチェックでは0とポインタなどの可能性があるので、nodeの型も確認する
				if (node->type->kind == TYPE_INTEGER && let_node->kind == EXPR_INTEGER_LITERAL) {
					expression_node* new_node = malloc_check(sizeof(expression_node));
					new_node->kind = EXPR_INTEGER_LITERAL;
					new_node->type = node->type; // usual arithmetic conversion後の型
					new_node->is_variable = 0;
					new_node->hint = NULL;
					new_node->info.value = let_node->info.value;
					node = new_node;
				} else {
					expression_node* new_node = new_operator(OP_CAST, let_node, node->type);
					new_node->type = node->type;
					node = new_node;
				}
			}
			break;
		default:
			// 何もしない
			break;
		}
	}
	return node;
}
