#include "ast.h"
#include "util.h"

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

int is_integer_type(type_node* type) {
	return type != NULL && type->kind == TYPE_INTEGER;
}

int is_real_type(type_node* type) {
	return type != NULL && type->kind == TYPE_INTEGER;
}

int is_arithmetic_type(type_node* type) {
	return type != NULL && type->kind == TYPE_INTEGER;
}

int is_pointer_type(type_node* type) {
	return type != NULL && type->kind == TYPE_POINTER;
}

int is_scalar_type(type_node* type) {
	return type != NULL && (type->kind == TYPE_INTEGER || type->kind == TYPE_POINTER);
}

int is_array_type(type_node* type) {
	return type != NULL && type->kind == TYPE_ARRAY;
}

int is_function_type(type_node* type) {
	return type != NULL && type->kind == TYPE_FUNCTION;
}

int is_object_type(type_node* type) {
	return type != NULL &&
		(type->kind == TYPE_INTEGER ||
		type->kind == TYPE_POINTER ||
		type->kind == TYPE_ARRAY);
}

int is_complete_object_type(type_node* type) {
	return type != NULL &&
		(type->kind == TYPE_INTEGER ||
		type->kind == TYPE_POINTER ||
		(type->kind == TYPE_ARRAY && type->size >= 0));
}

int is_void_type(type_node* type) {
	return type != NULL && type->kind == TYPE_VOID;
}

int is_compatible_type(type_node* t1, type_node* t2) {
	if (t1 == NULL || t2 == NULL || t1->kind != t2->kind) return 0;
	switch (t1->kind) {
	case TYPE_INTEGER:
		return t1->size == t2->size && t1->info.is_signed == t2->info.is_signed;
	case TYPE_POINTER:
		return is_compatible_type(t1->info.target_type, t2->info.target_type);
	case TYPE_ARRAY:
		return t1->size == t2->size &&
			is_compatible_type(t1->info.element_type, t2->info.element_type);
	case TYPE_FUNCTION:
		// 戻り値の型が違ったらNG
		if (!is_compatible_type(t1->info.f.return_type, t2->info.f.return_type)) return 0;
		// 引数が不定のものがあればOK
		if (t1->info.f.arg_num < 0 || t2->info.f.arg_num < 0) return 1;
		// 引数の数が違ったらNG
		if (t1->info.f.arg_num != t2->info.f.arg_num) return 0;
		// 対応する引数の型が違ったらNG
		for (int i = 0; i < t1->info.f.arg_num; i++) {
			if (!is_compatible_type(t1->info.f.arg_types[i], t2->info.f.arg_types[i])) return 0;
		}
		return 1;
	case TYPE_VOID:
		return 1;
	}
	return 0;
}
