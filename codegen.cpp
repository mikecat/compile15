#include <string>
#include <sstream>
#include <map>
#include <vector>
#include "ast.h"
#include "codegen.hpp"

std::string codegen_error::build_message(int lineno, std::string message) {
	std::stringstream ss;
	ss << message << " at line " << lineno;
	return ss.str();
}

struct var_info {
	int offset;
	type_node* type;
	bool is_global;
	bool is_register;
	var_info(int offset_ = 0, type_node* type_ = nullptr, bool isg = false, bool isr = false) :
		offset(offset_), type(type_), is_global(isg), is_register(isr) {}
};

struct codegen_status {
	// global
	int base_address;
	bool old_entry_exists;
	bool old_single_entry_exists;
	std::string old_single_entry_name;
	int gv_offset;
	bool gv_exists;
	// global + function-local
	std::vector<std::map<std::string, var_info*> > var_maps;
	// function-local (specified from global)
	bool entry_function;
	bool old_entry;
	bool old_single_entry;

	// function-local
	int lv_mem_size;
	std::vector<int> lv_mem_offset;
	int lv_reg_size;
	std::vector<int> lv_reg_offset;
	std::vector<int> lv_reg_assign;

	bool call_exists;
	bool gv_access_exists;
	int gv_access_register;
	int registers_written;
	int registers_reserved;
};

std::vector<asm_inst> codegen_gvar(ast_node* ast, codegen_status& status) {
	if (ast == nullptr || ast->kind != NODE_VAR_DEFINE) {
		throw codegen_error(ast == nullptr ? 0 : ast->lineno,
			"non-variable node passed to codegen_gvar()");
	}
	type_node* type = ast->d.var_def.type;
	char* name = ast->d.var_def.name;
	expression_node* initializer = ast->d.var_def.initializer;
	auto& var_map = status.var_maps.back();
	if (var_map.find(name) != var_map.end()) {
		throw codegen_error(ast->lineno,
			std::string("multiple definition of global variable ") + name);
	}
	int align = type->align;
	if (status.gv_offset % align != 0) {
		status.gv_offset = ((status.gv_offset + align - 1) / align) * align;
	}
	var_map[name] = new var_info(status.gv_offset, type, true, false);
	status.gv_offset += type->size;
	if (type->size == 1) status.gv_offset++; // DATABで1個だけデータを置いても2バイト使われる
	std::vector<asm_inst> result;
	std::vector<uint32_t> init_values;
	asm_inst_kind inst = EMPTY;
	type_node* element_type;
	int nelem;
	if (type->kind == TYPE_ARRAY) {
		element_type = type->info.element_type;
		if (element_type->kind == TYPE_ARRAY) {
			throw codegen_error(ast->lineno, "array of array not supported");
		}
		nelem = type->size / element_type->size;
		std::vector<uint32_t> array_init_values;
		if (initializer != nullptr) {
			while (initializer->kind == EXPR_OPERATOR && initializer->info.op.kind == OP_COMMA) {
				expression_node* left = initializer->info.op.operands[0];
				expression_node* right = initializer->info.op.operands[1];
				expression_node* value_node = constfold(right);
				if (value_node->kind == EXPR_INTEGER_LITERAL) {
					array_init_values.push_back(value_node->info.value);
				} else {
					throw codegen_error(ast->lineno, "non-constant initializer");
				}
				initializer = left;
			}
			expression_node* initializer_value_node = constfold(initializer);
			if (initializer_value_node->kind == EXPR_INTEGER_LITERAL) {
				array_init_values.push_back(initializer_value_node->info.value);
			} else {
				throw codegen_error(ast->lineno, "non-constant initializer");
			}
			init_values.insert(init_values.end(), array_init_values.rbegin(), array_init_values.rend());
		}
	} else {
		element_type = type;
		nelem = 1;
		if (initializer != nullptr) {
			expression_node* initializer_value_node = constfold(initializer);
			if (initializer_value_node->kind == EXPR_INTEGER_LITERAL) {
				init_values.push_back(initializer_value_node->info.value);
			} else {
				throw codegen_error(ast->lineno, "non-constant initializer");
			}
		}
	}
	uint32_t mask = 0;
	switch (element_type->size) {
	case 1: inst = DB; mask = UINT32_C(0xff); break;
	case 2: inst = DW; mask = UINT32_C(0xffff); break;
	case 4: inst = DD; mask = UINT32_C(0xffffffff); break;
	default: throw codegen_error(ast->lineno, "unsupported element size");
	}
	auto init_itr = init_values.begin();
	for (int i = 0; i < nelem; i++) {
		uint32_t value;
		if (init_itr == init_values.end()) {
			value = 0;
		} else {
			value = *init_itr;
			init_itr++;
		}
		result.push_back(asm_inst(inst, value & mask));
		if (i == 0) result.back().comment = name;
	}
	return result;
}

// 今のブロックに変数を登録し、登録した変数のオフセットを返す
int codegen_register_variable(ast_node* def_node, codegen_status& status,
bool argument_mode, bool pragma_use_register, int pragma_use_register_id) {
	if (def_node == nullptr || def_node->kind != (argument_mode ? NODE_ARGUMENT : NODE_VAR_DEFINE)) {
		throw codegen_error(def_node == nullptr ? 0 : def_node->lineno,
			std::string("tried to register something not ") + (argument_mode ? "an argument" : "a variable"));
	}
	auto& mem_offset = status.lv_mem_offset.back();
	auto& reg_offset = status.lv_reg_offset.back();
	auto& var_map = status.var_maps.back();
	type_node* type = argument_mode ? def_node->d.arg.type : def_node->d.var_def.type;
	char* name = argument_mode ? def_node->d.arg.name : def_node->d.var_def.name;
	int is_register = argument_mode ? def_node->d.arg.is_register : def_node->d.var_def.is_register;
	if (var_map.find(name) != var_map.end()) {
		throw codegen_error(def_node->lineno,
			std::string("multiple definition of ") + (argument_mode ? "argument " : "variable ") + name);
	}
	int reg_specify = -1;
	if (argument_mode) {
		if (def_node->d.arg.pragmas != nullptr && def_node->d.arg.pragmas->kind == NODE_ARRAY) {
			size_t pragma_node_num = def_node->d.arg.pragmas->d.array.num;
			ast_node** pragma_nodes = def_node->d.arg.pragmas->d.array.nodes;
			for (size_t i = 0; i < pragma_node_num; i++) {
				if (pragma_nodes[i]->kind == NODE_PRAGMA) {
					size_t token_num = pragma_nodes[i]->d.array.num;
					ast_node** tokens = pragma_nodes[i]->d.array.nodes;
					if (token_num >= 1 && tokens[0]->kind == NODE_CONTROL_IDENTIFIER &&
					std::string(tokens[0]->d.identifier.name) == "use_register") {
						is_register = 1;
						def_node->d.arg.is_register = 1;
						if (token_num >= 2 && tokens[1]->kind == NODE_CONTROL_INTEGER) {
							if (reg_specify >= 0) {
								throw codegen_error(def_node->lineno, "multiple register specification");
							}
							reg_specify = tokens[1]->d.integer.value;
						}
					}
				}
			}
		}
	} else {
		if (pragma_use_register) {
			is_register = 1;
			def_node->d.var_def.is_register = 1;
			if (pragma_use_register_id >= 0) reg_specify = pragma_use_register_id;
		}
	}
	var_info* vi;
	int offset;
	if (is_register) {
		offset = reg_offset;
		vi = new var_info(reg_offset, type, false, true);
		reg_offset++;
		if (status.lv_reg_size < reg_offset) {
			status.lv_reg_size = reg_offset;
			status.lv_reg_assign.resize(status.lv_reg_size, -1);
		}
		if (reg_specify >= 0) {
			if (status.lv_reg_assign[offset] >= 0 && status.lv_reg_assign[offset] != reg_specify) {
				throw codegen_error(def_node->lineno, "register specification conflict");
			}
			status.lv_reg_assign[offset] = reg_specify;
		}
	} else {
		if (mem_offset % type->align != 0) {
			mem_offset += type->align - (mem_offset % type->align);
		}
		offset = mem_offset;
		vi = new var_info(mem_offset, type, false, false);
		mem_offset += argument_mode ? 4 : type->size;
		if (status.lv_mem_size < mem_offset) status.lv_mem_size = mem_offset;
	}
	var_map[name] = vi;
	if (!argument_mode) def_node->d.var_def.info = vi;
	return offset;
}

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

// 式の前処理を行う
// * 関数呼び出しおよびグローバル変数の参照があるかを調べる
void codegen_preprocess_expr(expression_node* expr, int lineno, codegen_status& status) {
	if (expr == nullptr) {
		throw codegen_error(lineno, "NULL passed to codegen_preprocess_expr()");
	}
	switch (expr->kind) {
	case EXPR_INTEGER_LITERAL:
		// 何もしない
		break;
	case EXPR_IDENTIFIER:
		// グローバル変数なら、使用フラグを立てる
		// グローバルな識別子でも、関数の場合は、グローバル変数とはみなさない
		// TODO: 直接呼び出さず関数ポインタ扱いする場合、関数もグローバル変数扱い(基準アドレスを要求)する
		if (expr->info.ident.info->is_global && expr->info.ident.info->type->kind != TYPE_FUNCTION) {
			status.gv_access_exists = true;
		}
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
		break;
	}
}

// ブロックの前処理を行う
// * 変数に割り当てるレジスタの指定を読み取る
// * 識別子を解決する
// * constfoldをする
// * グローバル変数および関数呼び出しがあるかを調べる
void codegen_preprocess_block(ast_node* ast, codegen_status& status) {
	if (ast == nullptr || ast->kind != NODE_ARRAY) {
		throw codegen_error(ast == nullptr ? 0 : ast->lineno,
			"non-array node passed to codegen_preprocess_block()");
	}
	// このブロック用の情報を作る
	status.lv_mem_offset.push_back(status.lv_mem_offset.back());
	status.lv_reg_offset.push_back(status.lv_reg_offset.back());
	status.var_maps.push_back(std::map<std::string, var_info*>());

	bool pragma_use_register = false;
	int pragma_use_register_id = -1;

	size_t num = ast->d.array.num;
	ast_node** nodes = ast->d.array.nodes;
	for (size_t i = 0; i < num; i++) {
		switch (nodes[i]->kind) {
		case NODE_ARRAY:
			codegen_preprocess_block(nodes[i], status);
			break;
		case NODE_VAR_DEFINE:
			codegen_register_variable(nodes[i], status, false, pragma_use_register, pragma_use_register_id);
			if (nodes[i]->d.var_def.initializer != nullptr) {
				codegen_resolve_identifier_expr(nodes[i]->d.var_def.initializer, nodes[i]->lineno, status);
				nodes[i]->d.var_def.initializer = constfold(nodes[i]->d.var_def.initializer);
				codegen_preprocess_expr(nodes[i]->d.var_def.initializer, nodes[i]->lineno, status);
			}
			break;
		case NODE_FUNC_DEFINE:
			throw codegen_error(nodes[i]->lineno, "cannot define function inside function");
			break;
		case NODE_EXPR:
			codegen_resolve_identifier_expr(nodes[i]->d.expr.expression, nodes[i]->lineno, status);
			nodes[i]->d.expr.expression = constfold(nodes[i]->d.expr.expression);
			codegen_preprocess_expr(nodes[i]->d.expr.expression, nodes[i]->lineno, status);
			break;
		case NODE_EMPTY:
			// 何もしない
			break;
		case NODE_PRAGMA:
			{
				size_t token_num = nodes[i]->d.array.num;
				ast_node** tokens = nodes[i]->d.array.nodes;
				if (token_num >= 1 && tokens[0]->kind == NODE_CONTROL_IDENTIFIER &&
				std::string(tokens[0]->d.identifier.name) == "use_register") {
					pragma_use_register = true;
					if (token_num >= 2 && tokens[1]->kind == NODE_CONTROL_INTEGER) {
						if (pragma_use_register_id >= 0) {
							throw codegen_error(ast->lineno, "multiple register specification");
						}
						pragma_use_register_id = tokens[1]->d.integer.value;
					}
				}
			}
			break;
		default:
			throw codegen_error(nodes[i]->lineno, "unexpected node");
		}
		if (nodes[i]->kind != NODE_PRAGMA) {
			pragma_use_register = false;
			pragma_use_register_id = -1;
		}
	}

	// このブロック用の情報を破棄する
	status.lv_mem_offset.pop_back();
	status.lv_reg_offset.pop_back();
	status.var_maps.pop_back();
}

// 指定のレジスタに指定の数を置くコードを生成する
std::vector<asm_inst> codegen_put_number(int dest_reg, uint32_t value) {
	std::vector<asm_inst> res[3];
	uint32_t work_values[3] = {value, -value, ~value};
	for (int i = 0; i < 3; i++) {
		uint32_t work_value = work_values[i];
		bool is_first = true;
		for (int j = 31; j >= 7; j--) {
			if ((work_value >> j) & 1) {
				uint32_t current_value = (work_value >> (j - 7)) & 0xff;
				res[i].push_back(asm_inst(is_first ? MOV_LIT : ADD_LIT, dest_reg, current_value));
				if (j > 7) res[i].push_back(asm_inst(SHL_REG_LIT, dest_reg, dest_reg, j - 7));
				work_value &= ~(UINT32_C(0xff) << (j - 7));
				is_first = false;
			}
		}
		if (is_first) {
			res[i].push_back(asm_inst(MOV_LIT, dest_reg, work_value));
		} else if (work_value > 0) {
			res[i].push_back(asm_inst(ADD_LIT, dest_reg, work_value));
		}
	}
	res[1].push_back(asm_inst(NEG_REG, dest_reg, dest_reg));
	res[2].push_back(asm_inst(NOT_REG, dest_reg, dest_reg));
	int best = 0;
	if (res[1].size() < res[best].size()) best = 1;
	if (res[2].size() < res[best].size()) best = 2;
	return res[best];
}

// グローバル変数アクセス用のレジスタを設定する
std::vector<asm_inst> codegen_set_gv_access_register(int dest_reg, int src_reg,
int base_address, codegen_status& status) {
	std::vector<asm_inst> result;
	if (status.old_entry) {
		// グローバル変数の位置が渡されるので、格納する
		if (src_reg != dest_reg) {
			result.push_back(asm_inst(MOV_REG, dest_reg, src_reg));
		}
	} else {
		// RAM領域の0番地の位置が渡されるので、base_addressを足して格納する
		std::vector<asm_inst> num_code = codegen_put_number(dest_reg, base_address);
		if (src_reg == dest_reg) result.push_back(asm_inst(MOV_REG, 12, src_reg));
		result.insert(result.end(), num_code.begin(), num_code.end());
		result.push_back(asm_inst(ADD_REG, dest_reg, src_reg == dest_reg ? 12 : src_reg));
	}
	return result;
}

std::vector<asm_inst> codegen_func(ast_node* ast, codegen_status& status) {
	if (ast == nullptr || ast->kind != NODE_FUNC_DEFINE) {
		throw codegen_error(ast == nullptr ? 0 : ast->lineno,
			"non-function node passed to codegen_func()");
	}
	// funciton-localなstatusを初期化
	status.lv_mem_size = 0;
	status.lv_mem_offset.clear();
	status.lv_mem_offset.push_back(0);
	status.lv_reg_size = 0;
	status.lv_reg_offset.clear();
	status.lv_reg_offset.push_back(0);
	status.lv_reg_assign.clear();
	status.call_exists = false;
	status.gv_access_exists = false;
	status.gv_access_register = -1;
	status.registers_written = 0;
	status.registers_reserved = 0;
	// 引数の情報を登録
	status.var_maps.push_back(std::map<std::string, var_info*>());
	int args_on_stack = 0, args_on_reg = 0;
	size_t args_num = 0;
	std::vector<int> reg_args_given;
	std::vector<int> reg_args_assigned;
	bool r1_is_reg_arg = false; // R1はレジスタに保存する引数として使用されている
	if (ast->d.func_def.arguments != NULL && ast->d.func_def.arguments->kind == NODE_ARRAY) {
		args_num = ast->d.func_def.arguments->d.array.num;
		if (args_num > 4) {
			throw codegen_error(ast->lineno, "too many arguments");
		}
		ast_node** args = ast->d.func_def.arguments->d.array.nodes;
		for (size_t i = 0; i < args_num; i++) {
			int assigned = codegen_register_variable(args[i], status, true, false, 0);
			// codegen_register_variable()内でチェックしているので、args[i]はNODE_ARGUMENT
			if (args[i]->d.arg.is_register) {
				args_on_reg |= 1 << i;
				reg_args_given.push_back(i);
				reg_args_assigned.push_back(assigned);
			} else {
				args_on_stack |= 1 << i;
			}
		}
		r1_is_reg_arg = args_num >= 2 && args[1]->d.arg.is_register;
	}
	int args_mem_size = status.lv_mem_size;
	// コード生成に備えた前処理を行う
	codegen_preprocess_block(ast->d.func_def.body, status);

	// レジスタ変数にレジスタを割り当てる
	// グローバル変数が無ければ、アクセス用のレジスタは不要
	// entry関数かつ関数呼び出しが無ければ、好きなレジスタを使えばいいので予約不要
	// 関数呼び出しが無く、かつグローバル変数へのアクセスが無ければ、アクセス用のレジスタは不要
	if (status.gv_exists && ((!status.entry_function && status.gv_access_exists) || status.call_exists)) {
		// R7をグローバル変数領域へのポインタ用に予約する
		status.registers_reserved |= 1 << 7;
		status.gv_access_register = 7;
	}
	// 割り当てるレジスタが指定された変数用のレジスタを予約する
	for (auto itr = status.lv_reg_assign.begin(); itr != status.lv_reg_assign.end(); itr++) {
		if (*itr >= 0) {
			if (*itr > 7) {
				throw codegen_error(ast->lineno, "invalid register specified");
			}
			if ((status.registers_reserved >> *itr) & 1) {
				throw codegen_error(ast->lineno, "register specification conflict");
			}
			status.registers_reserved |= 1 << *itr;
		}
	}
	// 関数呼び出しが無いモードの時、レジスタの引数をそのレジスタに優先して割り当てる
	if (!status.call_exists) {
		for (size_t i = 0; i < reg_args_given.size(); i++) {
			if (status.lv_reg_assign[reg_args_assigned[i]] < 0 && // 割り当てが指定されていない
			!((status.registers_reserved >> reg_args_given[i]) & 1)) { // そのレジスタが空いている
				// 割り当てる
				status.lv_reg_assign[reg_args_assigned[i]] = reg_args_given[i];
				status.registers_reserved |= 1 << reg_args_given[i];
			}
		}
	}
	// 残りの変数を空いているレジスタに割り当てる
	static const int regs_try_order_candidate[2][8] = {
		{3, 2, 1, 0, 7, 6, 5, 4}, // 関数呼び出しが無い時
		{7, 6, 5, 4, 3, 2, 1, 0} // 関数呼び出しがある時
	};
	const int* regs_try_order = regs_try_order_candidate[status.call_exists ? 1 : 0];
	for (int i = 0; i < status.lv_reg_size; i++) {
		if (status.lv_reg_assign[i] < 0) {
			bool ok = false;
			for (int j = 0; j < 8; j++) {
				int reg = regs_try_order[j];
				if (!((status.registers_reserved >> reg) & 1)) {
					status.lv_reg_assign[i] = reg;
					status.registers_reserved |= 1 << reg;
					ok = true;
					break;
				}
			}
			if (!ok) throw codegen_error(ast->lineno, "register exhausted for variables");
		}
	}
	// グローバル変数へのアクセスがあり、かつまだアクセス用のレジスタを割り当てていなければ、割り当てる
	if (status.gv_access_exists && status.gv_access_register < 0) {
		bool ok = false;
		for (int j = 0; j < 8; j++) {
			int reg = regs_try_order[j];
			if (!((status.registers_reserved >> reg) & 1)) {
				status.gv_access_register = reg;
				status.registers_reserved |= 1 << reg;
				ok = true;
				break;
			}
		}
		if (!ok) throw codegen_error(ast->lineno, "register exhausted for global variable access");
	}

	std::vector<asm_inst> result;
	// entry関数かつグローバル変数の位置を用いる場合、グローバル変数の位置を設定する
	bool write_gv_access_register = status.entry_function && status.gv_access_register >= 0;
	// スタックにローカル変数の領域を確保する
	if (status.lv_mem_size > args_mem_size) {
		int delta = (status.lv_mem_size - args_mem_size + 3) / 4;
		if (delta <= 127) {
			result.push_back(asm_inst(SUBSP_LIT, delta));
		} else {
			int reg = args_num < 4 ? 3 : 4;
			status.registers_written |= 1 << reg;
			std::vector<asm_inst> num_code = codegen_put_number(reg, -delta * 4);
			result.insert(result.end(), num_code.begin(), num_code.end());
			result.push_back(asm_inst(ADD_REG, 13, reg));
		}
	}
	// スタックに引数を積む
	if (args_on_stack != 0) {
		result.push_back(asm_inst(PUSH_REGS, args_on_stack));
	}
	// レジスタ上の変数を割り当てたレジスタに書き込む
	for (size_t i = 0; i < reg_args_assigned.size(); i++) {
		// オフセットからそのオフセットに対応するレジスタに変換する
		reg_args_assigned[i] = status.lv_reg_assign[reg_args_assigned[i]];
	}
	bool scheduled_r1_is_gv_access_register = false;
	if (write_gv_access_register && !r1_is_reg_arg) {
		// グローバル変数アクセス用のレジスタの設定が要求され、
		// かつR1をメモリに置くか保存しない場合は、スケジューリングに組み込む
		scheduled_r1_is_gv_access_register = true;
		reg_args_given.push_back(1);
		reg_args_assigned.push_back(status.gv_access_register);
	}
	// 書き込む予定の所が読み込み対象なら、それを先に処理する
	// この関係がループしてしまったら、R12を用いて処理する
	int args_assign_done = 0;
	for (size_t i = 0; i < reg_args_given.size(); i++) {
		// 既に処理済みなら、何もしない
		if ((args_assign_done >> i) & 1) continue;
		// 割り当てられたレジスタとデータがあるレジスタが同じなら、移動しない
		if (reg_args_assigned[i] == reg_args_given[i]) {
			if (reg_args_given[i] == 1 && scheduled_r1_is_gv_access_register) {
				std::vector<asm_inst> gv_code = codegen_set_gv_access_register(
					reg_args_assigned[i], reg_args_given[i], status.base_address, status);
				result.insert(result.end(), gv_code.begin(), gv_code.end());
				status.registers_written |= 1 << reg_args_assigned[i];
			}
			continue;
		}

		std::vector<int> target;
		target.push_back(i);
		size_t current = i;
		bool hit;
		bool loop = false;
		do {
			hit = false;
			for (size_t j = 0; j < reg_args_given.size(); j++) {
				if (!((args_assign_done >> j) & 1) && reg_args_given[j] == reg_args_assigned[current]) {
					// まだ書き込んでいなくて、ぶつかるなら
					hit = true;
					target.push_back(j);
					current = j;
					break; // 同じレジスタが複数の引数にはなっていないはず
				}
			}
			if (hit && current == i) {
				loop = true;
				target.pop_back();
				break;
			}
		} while (hit);
		if (loop) {
			result.push_back(asm_inst(MOV_REG, 12, reg_args_given[target.back()]));
			auto itr = target.rbegin();
			for (itr++; itr != target.rend(); itr++) {
				if (scheduled_r1_is_gv_access_register && reg_args_given[*itr] == 1) {
					std::vector<asm_inst> gv_code = codegen_set_gv_access_register(
						reg_args_assigned[*itr], reg_args_given[*itr], status.base_address, status);
					result.insert(result.end(), gv_code.begin(), gv_code.end());
				} else {
					result.push_back(asm_inst(MOV_REG, reg_args_assigned[*itr], reg_args_given[*itr]));
				}
				status.registers_written |= 1 << reg_args_assigned[*itr];
				args_assign_done |= 1 << *itr;
			}
			if (scheduled_r1_is_gv_access_register && reg_args_given[target.back()] == 1) {
				std::vector<asm_inst> gv_code = codegen_set_gv_access_register(
					reg_args_assigned[target.back()], 12, status.base_address, status);
				result.insert(result.end(), gv_code.begin(), gv_code.end());
			} else {
				result.push_back(asm_inst(MOV_REG, reg_args_assigned[target.back()], 12));
			}
			status.registers_written |= 1 << reg_args_assigned[target.back()];
			args_assign_done |= 1 << target.back();
		} else {
			for (auto itr = target.rbegin(); itr != target.rend(); itr++) {
				if (scheduled_r1_is_gv_access_register && reg_args_given[*itr] == 1) {
					std::vector<asm_inst> gv_code = codegen_set_gv_access_register(
						reg_args_assigned[*itr], reg_args_given[*itr], status.base_address, status);
					result.insert(result.end(), gv_code.begin(), gv_code.end());
				} else {
					result.push_back(asm_inst(MOV_REG, reg_args_assigned[*itr], reg_args_given[*itr]));
				}
				status.registers_written |= 1 << reg_args_assigned[*itr];
				args_assign_done |= 1 << *itr;
			}
		}
	}
	if (write_gv_access_register && r1_is_reg_arg) {
		// グローバル変数アクセス用のレジスタの設定が要求され、
		// かつR1をレジスタに置く場合は、その配置後に処理する
		for (size_t i = 0; i < reg_args_given.size(); i++) {
			if (reg_args_given[i] == 1) {
				// R1の割り当て先からデータを取る
				std::vector<asm_inst> gv_code = codegen_set_gv_access_register(
					status.gv_access_register, reg_args_assigned[i], status.base_address, status);
				result.insert(result.end(), gv_code.begin(), gv_code.end());
				args_assign_done |= 1 << status.gv_access_register;
			}
		}
	}

	// 本体のコードを生成する

	// 値を書き込んだcallee-saveレジスタの退避コードを追加する
	int regs_to_backup = status.registers_written & 0xf0;
	if (status.call_exists) regs_to_backup |= 0x100;
	if (regs_to_backup != 0) {
		result.insert(result.begin(), asm_inst(PUSH_REGS, regs_to_backup));
	}
	// 関数のラベルを追加する
	result.insert(result.begin(), asm_inst(LABEL, ast->d.func_def.name));

	// スタック上の引数とローカル変数を取り除く
	if (status.lv_mem_size > 0) {
		int delta = (status.lv_mem_size + 3) / 4;
		if (delta <= 127) {
			result.push_back(asm_inst(ADDSP_LIT, delta));
		} else {
			std::vector<asm_inst> num_code = codegen_put_number(3, delta * 4);
			result.insert(result.end(), num_code.begin(), num_code.end());
			result.push_back(asm_inst(ADD_REG, 13, 3));
		}
	}
	// 退避したレジスタを戻して関数から戻る
	if (regs_to_backup != 0) {
		result.push_back(asm_inst(POP_REGS, regs_to_backup));
	}
	if (!(regs_to_backup & 0x100)) {
		result.push_back(asm_inst(RET));
	}

	// 引数の情報を破棄
	status.var_maps.pop_back();

	return result;
}

std::vector<asm_inst> codegen(ast_node* ast) {
	if (ast == nullptr || ast->kind != NODE_ARRAY) {
		throw codegen_error(ast == nullptr ? 0 : ast->lineno,
			"top-level AST not array");
	}
	std::vector<asm_inst> result;
	codegen_status status;
	status.base_address = 0x700;
	status.old_entry_exists = false;
	status.old_single_entry_exists = false;
	status.old_single_entry_name = "";
	status.entry_function = false;
	status.old_entry = false;
	status.old_single_entry = false;
	status.gv_offset = 0;
	status.gv_exists = false;
	status.var_maps.push_back(std::map<std::string, var_info*>());

	bool base_address_specified = false;
	for (size_t i = 0; i < ast->d.array.num; i++) {
		ast_node* node = ast->d.array.nodes[i];
		if (node->kind == NODE_VAR_DEFINE) {
			std::vector<asm_inst> var_inst = codegen_gvar(node, status);
			result.insert(result.end(), var_inst.begin(), var_inst.end());
			status.gv_exists = true;
		} else if (node->kind == NODE_FUNC_DEFINE) {
			status.var_maps.back()[node->d.func_def.name] = new var_info(0,
				new_function_type(node->d.func_def.return_type, node->d.func_def.arguments), true, false);
		} else if (node->kind == NODE_PRAGMA) {
			size_t token_num = node->d.array.num;
			ast_node** tokens = node->d.array.nodes;
			if (token_num >= 1 && tokens[0]->kind == NODE_CONTROL_IDENTIFIER &&
			std::string(tokens[0]->d.identifier.name) == "base_address") {
				if (base_address_specified) {
					throw codegen_error(node->lineno, "multiple base address specification found");
				}
				if (token_num >= 2 && tokens[1]->kind == NODE_CONTROL_INTEGER) {
					status.base_address = tokens[1]->d.integer.value;
					base_address_specified = true;
				} else {
					throw codegen_error(node->lineno, "invalid base address sepcification");
				}
			}
		}
	}

	for (size_t i = 0; i < ast->d.array.num; i++) {
		if (ast->d.array.nodes[i]->kind == NODE_PRAGMA) {
			size_t token_num = ast->d.array.nodes[i]->d.array.num;
			ast_node** tokens = ast->d.array.nodes[i]->d.array.nodes;
			if (token_num >= 1 && tokens[0]->kind == NODE_CONTROL_IDENTIFIER &&
			std::string(tokens[0]->d.identifier.name) == "entry") {
				if (status.entry_function) {
					throw codegen_error(tokens[0]->lineno, "multiple entry specified for one function");
				}
				status.entry_function = true;
				for (size_t i = 1; i < token_num; i++) {
					if (tokens[i]->kind == NODE_CONTROL_IDENTIFIER) {
						std::string name = tokens[i]->d.identifier.name;
						if (name == "old") status.old_entry = true;
						else if (name == "single") status.old_single_entry = true;
					}
				}
			}
		} else {
			if (ast->d.array.nodes[i]->kind == NODE_FUNC_DEFINE) {
				if (status.entry_function && status.old_entry) {
					if (status.old_single_entry_exists ||
					(status.old_single_entry && status.old_entry_exists)) {
						throw codegen_error(ast->d.array.nodes[i]->lineno,
							"multiple old entry function found while single is specified");
					}
					status.old_entry_exists = true;
					if (status.old_single_entry) {
						status.old_single_entry_exists = true;
						status.old_single_entry_name = ast->d.array.nodes[i]->d.func_def.name;
					}
				}
				std::vector<asm_inst> func_inst = codegen_func(ast->d.array.nodes[i], status);
				result.insert(result.end(), func_inst.begin(), func_inst.end());
			}
			status.entry_function = false;
			status.old_entry = false;
			status.old_single_entry = false;
		}
	}

	// old entry用のコードを追加する
	if (status.old_entry_exists) {
		if (status.old_single_entry_exists) {
			result.insert(result.begin(), asm_inst(JMP_DIRECT, status.old_single_entry_name));
		} else {
			result.insert(result.begin(), asm_inst(ADD_REG, 15, 0));
		}
		result.insert(result.begin(), asm_inst(MOV_REG, 1, 15));
	}

	return result;
}
