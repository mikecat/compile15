#include <stdlib.h>
#include <stdarg.h>
#include "ast.h"
#include "util.h"

ast_node* new_ast_node(node_kind kind, int lineno) {
	ast_node* node = malloc_check(sizeof(ast_node));
	node->kind = kind;
	node->lineno = lineno;
	return node;
}

ast_chain_node* new_chain_node(ast_chain_node* next, ast_node* element) {
	ast_chain_node* cnode = malloc_check(sizeof(ast_chain_node));
	cnode->node = element;
	cnode->next = next;
	return cnode;
}

// もとのchainは開放する
ast_node* ast_chain_to_array(ast_chain_node* chain, int lineno) {
	size_t count = 0;
	ast_chain_node* chain_ptr = chain;
	// 要素数を求める
	while (chain_ptr != NULL) {
		count++;
		chain_ptr = chain_ptr->next;
	}
	// 要素をarrayノードに格納する
	// ついでにchainを開放する
	ast_node* array = new_ast_node(NODE_ARRAY, lineno);
	array->d.array.num = count;
	array->d.array.nodes = malloc_check(sizeof(ast_node) * count);
	chain_ptr = chain;
	for (size_t i = 0; i < count; i++) {
		array->d.array.nodes[count - 1 - i] = chain_ptr->node;
		ast_chain_node* chain_next = chain_ptr->next;
		free(chain_ptr);
		chain_ptr = chain_next;
	}
	return array;
}

type_node* new_prim_type(int size, int is_signed) {
	type_node* node = malloc_check(sizeof(type_node));
	node->kind = TYPE_INTEGER;
	node->size = size;
	node->align = size;
	node->info.is_signed = is_signed;
	return node;
}

type_node* new_ptr_type(type_node* target_type) {
	type_node* node = malloc_check(sizeof(type_node));
	node->kind = TYPE_POINTER;
	node->size = 4;
	node->align = 4;
	node->info.target_type = target_type;
	return node;
}

type_node* new_array_type(int nelem, type_node* element_type) {
	type_node* node = malloc_check(sizeof(type_node));
	node->kind = TYPE_ARRAY;
	node->size = nelem * element_type->size;
	node->align = element_type->align;
	node->info.element_type = element_type;
	return node;
}

type_node* new_function_type(type_node* return_type, ast_node* args_array) {
	type_node* node = malloc_check(sizeof(type_node));
	node->kind = TYPE_FUNCTION;
	node->size = 1;
	node->align = 1;
	node->info.f.return_type = return_type;
	if (args_array == NULL || args_array->kind != NODE_ARRAY) {
		node->info.f.arg_num = -1;
		node->info.f.arg_types = NULL;
	} else {
		node->info.f.arg_num = args_array->d.array.num;
		node->info.f.arg_types = malloc_check(sizeof(type_node*) * args_array->d.array.num);
		for (size_t i = 0; i < args_array->d.array.num; i++) {
			node->info.f.arg_types[i] = args_array->d.array.nodes[i]->d.var_def.type;
		}
	}
	return node;
}

type_node* new_void_type(void) {
	type_node* node = malloc_check(sizeof(type_node));
	node->kind = TYPE_VOID;
	node->size = 1;
	node->align = 1;
	return node;
}

type_node* integer_promotion(type_node* type) {
	if (type == NULL || type->kind != TYPE_INTEGER || type->size >= 4) return type;
	// (ランクがint以下の整数型について)
	// 全値域がintで表せる型であれば、intにする
	// そうでなければ、unsigned intにする
	return new_prim_type(4, type->size < 4 || type->info.is_signed);
}

type_node* usual_arithmetic_conversion(type_node* t1, type_node* t2) {
	type_node* t1p = integer_promotion(t1);
	type_node* t2p = integer_promotion(t2);
	if (t1p == NULL || t2p == NULL || t1p->kind != TYPE_INTEGER || t2p->kind != TYPE_INTEGER) return NULL;
	if (t1p->info.is_signed == t2p->info.is_signed) {
		// 両方符号付き or 両方符号なし → ランクが高い方に合わせる
		return t1p->size >= t2p->size ? t1p : t2p;
	} else {
		type_node *us_type, *s_type;
		if (t1p->info.is_signed) {
			us_type = t2p;
			s_type = t1p;
		} else {
			us_type = t1p;
			s_type = t2p;
		}
		if (us_type->size >= s_type->size) {
			// 符号なしのランクが符号付き以上 → 符号なしに合わせる
			return us_type;
		} else if (s_type->size > us_type->size) {
			// 符号なしの値域全てが符号付きで表せる → 符号付きに合わせる
			return s_type;
		} else {
			// 符号付きの方の符号なし版に合わせる
			return new_prim_type(s_type->size, 0);
		}
	}
}

int type_is_compatible(type_node* t1, type_node* t2) {
	if (t1 == NULL || t2 == NULL || t1->kind != t2->kind) return 0;
	switch (t1->kind) {
	case TYPE_INTEGER:
		return t1->size == t2->size && t1->info.is_signed == t2->info.is_signed;
	case TYPE_POINTER:
		return type_is_compatible(t1->info.target_type, t2->info.target_type);
	case TYPE_ARRAY:
		return t1->size == t2->size &&
			type_is_compatible(t1->info.element_type, t2->info.element_type);
	case TYPE_FUNCTION:
		// 戻り値の型が違ったらNG
		if (!type_is_compatible(t1->info.f.return_type, t2->info.f.return_type)) return 0;
		// 引数が不定のものがあればOK
		if (t1->info.f.arg_num < 0 || t2->info.f.arg_num < 0) return 1;
		// 引数の数が違ったらNG
		if (t1->info.f.arg_num != t2->info.f.arg_num) return 0;
		// 対応する引数の型が違ったらNG
		for (int i = 0; i < t1->info.f.arg_num; i++) {
			if (!type_is_compatible(t1->info.f.arg_types[i], t2->info.f.arg_types[i])) return 0;
		}
		return 1;
	case TYPE_VOID:
		return 1;
	}
	return 0;
}

expression_node* new_integer_literal(uint32_t value, int is_signed) {
	expression_node* node = malloc_check(sizeof(expression_node));
	node->kind = EXPR_INTEGER_LITERAL;
	node->type = new_prim_type(4, is_signed);
	node->info.value = value;
	return node;
}

expression_node* new_expr_identifier(char* name) {
	expression_node* node = malloc_check(sizeof(expression_node));
	node->kind = EXPR_IDENTIFIER;
	node->type = NULL;
	node->info.ident.name = name;
	node->info.ident.info = NULL;
	return node;
}

expression_node* new_operator(operator_type op, ...) {
	expression_node* node = malloc_check(sizeof(expression_node));
	va_list args;
	node->kind = EXPR_OPERATOR;
	node->type = NULL;
	node->info.op.kind = op;
	va_start(args, op);
	node->info.op.operands[0] = va_arg(args, expression_node*);
	if (op > OP_DUMMY_BINARY_START) {
		node->info.op.operands[1] = va_arg(args, expression_node*);
	}
	if (op > OP_DUMMY_TERNARY_START) {
		node->info.op.operands[2] = va_arg(args, expression_node*);
	}
	va_end(args);
	set_operator_expression_type(node);
	return node;
}

void set_operator_expression_type(expression_node* node) {
	if (node == NULL || node->kind != EXPR_OPERATOR) return;

	type_node* types[3] = {NULL, NULL, NULL};
	types[0] = node->info.op.operands[0]->type;
	if (node->info.op.kind > OP_DUMMY_BINARY_START) types[1] = node->info.op.operands[1]->type;
	if (node->info.op.kind > OP_DUMMY_TERNARY_START) types[2] = node->info.op.operands[2]->type;

	if (node->info.op.kind != OP_CAST) node->type = NULL;
	switch (node->info.op.kind) {
	case OP_PARENTHESIS:
		node->type = types[0];
		break;
	case OP_FUNC_CALL_NOARGS:
		if (types[0] != NULL && types[0]->kind == TYPE_FUNCTION) {
			node->type = types[0]->info.f.return_type;
		}
		break;
	case OP_POST_INC:
	case OP_POST_DEC:
	case OP_PRE_INC:
	case OP_PRE_DEC:
		if (types[0] != NULL &&
		(types[0]->kind == TYPE_INTEGER || types[0]->kind == TYPE_ARRAY)) {
			node->type = types[0];
		}
		break;
	case OP_ADDRESS:
		if (types[0] != NULL) {
			node->type = new_ptr_type(types[0]);
		}
		break;
	case OP_INDIRECTION:
		if (types[0] != NULL && types[0]->kind == TYPE_POINTER) {
			node->type = types[0]->info.target_type;
		}
		break;
	case OP_PLUS:
	case OP_NEG:
	case OP_NOT:
		node->type = integer_promotion(types[0]);
		break;
	case OP_LNOT:
		node->type = new_prim_type(4, 1);
		break;
	case OP_SIZEOF:
		node->type = new_prim_type(4, 0);
		break;
	case OP_CAST:
		// 型は外部から与える。ここではわからない
		break;
	case OP_DUMMY_BINARY_START: break;
	case OP_ARRAY_REF:
		if (types[0] != NULL && types[1] != NULL &&
		((types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_INTEGER) ||
		(types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_POINTER))) {
			if (types[0]->kind == TYPE_POINTER) node->type = types[0]->info.target_type;
			else node->type = types[1]->info.target_type;
		}
		break;
	case OP_FUNC_CALL:
		if (types[0] != NULL && types[0]->kind == TYPE_FUNCTION) {
			node->type = types[0]->info.f.return_type;
		}
		break;
	case OP_MUL:
	case OP_DIV:
	case OP_MOD:
		if (types[0] != NULL && types[1] != NULL &&
		types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_INTEGER) {
			node->type = usual_arithmetic_conversion(types[0], types[1]);
		}
		break;
	case OP_ADD:
		if (types[0] != NULL && types[1] != NULL) {
			if (types[0]->kind == TYPE_INTEGER) {
				if (types[1]->kind == TYPE_INTEGER) {
					node->type = usual_arithmetic_conversion(types[0], types[1]);
				} else if (types[1]->kind == TYPE_POINTER &&
				types[1]->info.target_type->kind != TYPE_VOID) {
					node->type = types[1];
				}
			} else if (types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_INTEGER &&
			types[0]->info.target_type->kind != TYPE_VOID) {
				node->type = types[0];
			}
		}
		break;
	case OP_SUB:
		if (types[0] != NULL && types[1] != NULL) {
			if (types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_INTEGER) {
				node->type = usual_arithmetic_conversion(types[0], types[1]);
			} else if (types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_INTEGER &&
			types[0]->info.target_type->kind != TYPE_VOID) {
				node->type = types[0];
			} else if (types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_POINTER &&
			type_is_compatible(types[0]->info.target_type, types[1]->info.target_type) &&
			types[0]->info.target_type->kind != TYPE_VOID) {
				node->type = new_prim_type(4, 1);
			}
		}
		break;
	case OP_SHL:
	case OP_SHR:
		if (types[0] != NULL && types[1] != NULL &&
		types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_INTEGER) {
			node->type = integer_promotion(types[0]);
		}
		break;
	case OP_LESS:
	case OP_GREATER:
	case OP_LESS_EQUAL:
	case OP_GREATER_EQUAL:
		if (types[0] != NULL && types[1] != NULL) {
			if ((types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_INTEGER) ||
			(types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_POINTER &&
			type_is_compatible(types[0]->info.target_type, types[1]->info.target_type))) {
				node->type = new_prim_type(4, 1);
			}
		}
		break;
	case OP_EQUAL:
	case OP_NOT_EQUAL:
		if (types[0] != NULL && types[1] != NULL) {
			if ((types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_INTEGER) ||
			(types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_POINTER &&
			(type_is_compatible(types[0]->info.target_type, types[1]->info.target_type) ||
			types[0]->info.target_type->kind == TYPE_VOID ||
			types[1]->info.target_type->kind == TYPE_VOID))) {
				// 整数と整数、ポインタとポインタ
				node->type = new_prim_type(4, 1);
			} else {
				// ポインタと null pointer constantの比較かをチェックする
				expression_node* integer_node = NULL;
				if (types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_INTEGER) {
					integer_node = node->info.op.operands[1];
				} else if (types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_POINTER) {
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
		if (types[0] != NULL && types[1] != NULL &&
		types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_INTEGER) {
			node->type = usual_arithmetic_conversion(types[0], types[1]);
		}
		break;
	case OP_LAND:
	case OP_LOR:
		node->type = new_prim_type(4, 1);
		break;
	case OP_ASSIGN:
	case OP_MUL_ASSIGN:
	case OP_DIV_ASSIGN:
	case OP_MOD_ASSIGN:
	case OP_ADD_ASSIGN:
	case OP_SUB_ASSIGN:
	case OP_SHL_ASSIGN:
	case OP_SHR_ASSIGN:
	case OP_AND_ASSIGN:
	case OP_XOR_ASSIGN:
	case OP_OR_ASSIGN:
		node->type = types[0];
		break;
	case OP_COMMA:
		node->type = types[1];
		break;
	case OP_DUMMY_TERNARY_START: break;
	case OP_COND:
		if (types[1] != NULL && types[2] != NULL) {
			if (types[1]->kind == TYPE_INTEGER && types[2]->kind == TYPE_INTEGER) {
				// 整数と整数
				node->type = usual_arithmetic_conversion(types[1], types[2]);
			} else if(types[1]->kind == TYPE_POINTER && types[2]->kind == TYPE_POINTER) {
				// ポインタとポインタ
				if (type_is_compatible(types[1]->info.target_type, types[2]->info.target_type)) {
					node->type = types[1];
				} else if (types[1]->info.target_type->kind == TYPE_VOID) {
					node->type = types[1];
				} else if (types[2]->info.target_type->kind == TYPE_VOID) {
					node->type = types[2];
				}
			} else {
				// ポインタと null pointer constantかをチェックする
				expression_node* integer_node = NULL;
				type_node* pointer_type = NULL;
				if (types[1]->kind == TYPE_POINTER && types[2]->kind == TYPE_INTEGER) {
					integer_node = node->info.op.operands[2];
					pointer_type = types[1];
				} else if (types[1]->kind == TYPE_INTEGER && types[2]->kind == TYPE_POINTER) {
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
				new_node->info.value = node->info.op.operands[0]->info.value;
				node = new_node;
			}
			break;
		case OP_NEG:
			if (node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL) {
				expression_node* new_node = malloc_check(sizeof(expression_node));
				new_node->kind = EXPR_INTEGER_LITERAL;
				new_node->type = node->type; // integer promotion後の型
				new_node->info.value = -(node->info.op.operands[0]->info.value);
				node = new_node;
			}
			break;
		case OP_NOT:
			if (node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL) {
				expression_node* new_node = malloc_check(sizeof(expression_node));
				new_node->kind = EXPR_INTEGER_LITERAL;
				new_node->type = node->type; // integer promotion後の型
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
			if (node->type != NULL && node->type->kind == TYPE_INTEGER &&
			node->info.op.operands[0]->kind == EXPR_INTEGER_LITERAL) {
				uint32_t value = node->info.op.operands[0]->info.value;
				int is_signed = node->type->info.is_signed;
				int size = node->type->size;
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
					new_node->info.value = let_node->info.value;
					node = new_node;
				} else {
					expression_node* new_node = new_operator(OP_CAST, let_node);
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
